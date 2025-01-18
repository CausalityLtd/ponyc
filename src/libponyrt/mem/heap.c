#include "heap.h"
#include "pagemap.h"
#include "../ds/fun.h"
#include "../actor/actor.h"
#include "ponyassert.h"
#include <string.h>

#include <platform.h>
#include <dtrace.h>

#ifdef USE_ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#endif

typedef struct chunk_t
{
  // used for pointer tagging
  // bit 0 (lowest bit) for keeping track of chunk type (1 = small; 0 = large)
  // bit 1 for keeping track of chunks to be cleared
  char* m;
  union
  {
    struct
    {
      // small chunk
      // mutable
      uint32_t slots;
      uint32_t shallow;
      uint32_t finalisers;
    } small;
    struct
    {
      // large chunk
      // immutable
      size_t size;
    } large;
  };
  struct chunk_t* next;
} chunk_t;

// include it after defining `chunk_t`
#include "heap_chunk_sorting.h"

#define CHUNK_TYPE_BITMASK (uintptr_t)0x1
#define CHUNK_NEEDS_TO_BE_CLEARED_BITMASK (uintptr_t)0x2
#define SMALL_CHUNK_SIZECLASS_BITMASK (uintptr_t)0x1C
// TODO: figure out how to calculate the `2` from `SMALL_CHUNK_SIZECLASS_BITMASK` at compile time
#define SMALL_CHUNK_SIZECLASS_SHIFT (uintptr_t)2
#define LARGE_CHUNK_SLOT_BITMASK_SHIFT_AMOUNT (uintptr_t)2
#define LARGE_CHUNK_SLOT_BITMASK ((uintptr_t)0x1 << LARGE_CHUNK_SLOT_BITMASK_SHIFT_AMOUNT)
#define LARGE_CHUNK_SHALLOW_BITMASK_SHIFT_AMOUNT (uintptr_t)3
#define LARGE_CHUNK_SHALLOW_BITMASK ((uintptr_t)0x1 << LARGE_CHUNK_SHALLOW_BITMASK_SHIFT_AMOUNT)
#define LARGE_CHUNK_FINALISER_BITMASK_SHIFT_AMOUNT (uintptr_t)4
#define LARGE_CHUNK_FINALISER_BITMASK ((uintptr_t)0x1 << LARGE_CHUNK_FINALISER_BITMASK_SHIFT_AMOUNT)
#define CHUNK_M_BITMASK ~(CHUNK_TYPE_BITMASK | CHUNK_NEEDS_TO_BE_CLEARED_BITMASK | SMALL_CHUNK_SIZECLASS_BITMASK | LARGE_CHUNK_SLOT_BITMASK | LARGE_CHUNK_SHALLOW_BITMASK | LARGE_CHUNK_FINALISER_BITMASK)

enum
{
  FORCE_NO_FINALISERS = 0,
  FORCE_ALL_FINALISERS = 0xFFFFFFFF,
};

typedef char block_t[POOL_ALIGN];
typedef void (*chunk_fn)(chunk_t* chunk, uint32_t mark);

#define SIZECLASS_SIZE(sizeclass) (HEAP_MIN << (sizeclass))
#define SIZECLASS_MASK(sizeclass) (~(SIZECLASS_SIZE(sizeclass) - 1))

#define EXTERNAL_PTR(p, sizeclass) \
  ((void*)((uintptr_t)p & SIZECLASS_MASK(sizeclass)))

#define FIND_SLOT(ext, base) \
  ((uint32_t)1 << ((uintptr_t)((char*)(ext) - (char*)(base)) >> HEAP_MINBITS))

static const uint32_t sizeclass_empty[HEAP_SIZECLASSES] =
{
  0xFFFFFFFF,
  0x55555555,
  0x11111111,
  0x01010101,
  0x00010001
};

static const uint32_t sizeclass_init[HEAP_SIZECLASSES] =
{
  0xFFFFFFFE,
  0x55555554,
  0x11111110,
  0x01010100,
  0x00010000
};

#ifdef USE_RUNTIMESTATS
static const uint32_t sizeclass_slot_count[HEAP_SIZECLASSES] =
{
  32, 16, 8, 4, 2
};
#endif

static const uint8_t sizeclass_table[HEAP_MAX / HEAP_MIN] =
{
  0, 1, 2, 2, 3, 3, 3, 3,
  4, 4, 4, 4, 4, 4, 4, 4
};

static size_t heap_initialgc = 1 << 14;
static double heap_nextgc_factor = 2.0;

#ifdef USE_RUNTIMESTATS
/** Get the total number of allocations on the heap.
 */
size_t ponyint_heap_alloc_counter(pony_actor_t* actor)
{
  return actor->actorstats.heap_alloc_counter;
}

/** Get the total number of heap allocations freed.
 */
size_t ponyint_heap_free_counter(pony_actor_t* actor)
{
  return actor->actorstats.heap_free_counter;
}

/** Get the total number of GC iterations run.
 */
size_t ponyint_heap_gc_counter(pony_actor_t* actor)
{
  return actor->actorstats.heap_gc_counter;
}

/** Get the memory used by the heap.
 */
size_t ponyint_heap_mem_size(pony_actor_t* actor)
{
  // include memory that is in use by the heap but not counted as part of
  // `used` like `chunk_t`. also, don't include "fake used" for purposes of
  // triggering GC.
  return actor->actorstats.heap_mem_used;
}

/** Get the memory allocated by the heap.
 */
size_t ponyint_heap_alloc_size(pony_actor_t* actor)
{
  return actor->actorstats.heap_mem_allocated;
}

/** Get the memory overhead used by the heap.
 */
size_t ponyint_heap_overhead_size(pony_actor_t* actor)
{
  return actor->actorstats.heap_mem_allocated - actor->actorstats.heap_mem_used;
}

#endif

// check HEAP_RECYCLE_SIZECLASSES size
// if more are needed update the switch statement in `heap_recycle_index`
pony_static_assert(HEAP_RECYCLE_SIZECLASSES <= 6, "Too many HEAP_RECYCLE_SIZECLASSES! There must be at most 6!");

static uint32_t heap_recycle_index(size_t size)
{
  switch (size)
  {
    case HEAP_RECYCLE_MIXED_SIZE:
      return HEAP_RECYCLE_MIXED;

    case HEAP_RECYCLE_MIXED_SIZE << 1:
      return 1;

    case HEAP_RECYCLE_MIXED_SIZE << 2:
      return 2;

    case HEAP_RECYCLE_MIXED_SIZE << 3:
      return 3;

    case HEAP_RECYCLE_MIXED_SIZE << 4:
      return 4;

    default:
      return 5;
  }
}

static bool get_chunk_is_small_chunk(chunk_t* chunk)
{
  return (((uintptr_t)chunk->m & CHUNK_TYPE_BITMASK) == CHUNK_TYPE_BITMASK);
}

static void set_chunk_is_small_chunk(chunk_t* chunk)
{
  chunk->m = (char*)((uintptr_t)chunk->m | CHUNK_TYPE_BITMASK);
}

static uint32_t get_large_chunk_slot(chunk_t* chunk)
{
  pony_assert(!get_chunk_is_small_chunk(chunk));
  // `!!` to normalize to 1 or 0
  return !!(((uintptr_t)chunk->m & LARGE_CHUNK_SLOT_BITMASK) == LARGE_CHUNK_SLOT_BITMASK);
}

static void set_large_chunk_slot(chunk_t* chunk, uint32_t slot)
{
  pony_assert(!get_chunk_is_small_chunk(chunk));
  // `!!` to normalize to 1 or 0
  slot = !!slot;
  // left shift size to get bits in the right spot for OR'ing into `chunk->m`
  slot = slot << LARGE_CHUNK_SLOT_BITMASK_SHIFT_AMOUNT;
  pony_assert(slot == LARGE_CHUNK_SLOT_BITMASK || slot == 0);
  chunk->m = (char*)(((uintptr_t)chunk->m & ~LARGE_CHUNK_SLOT_BITMASK) | slot);
}

static uint32_t get_large_chunk_shallow(chunk_t* chunk)
{
  pony_assert(!get_chunk_is_small_chunk(chunk));
  // `!!` to normalize to 1 or 0
  return (((uintptr_t)chunk->m & LARGE_CHUNK_SHALLOW_BITMASK) == LARGE_CHUNK_SHALLOW_BITMASK);
}

static void set_large_chunk_shallow(chunk_t* chunk, uint32_t shallow)
{
  pony_assert(!get_chunk_is_small_chunk(chunk));
  // `!!` to normalize to 1 or 0
  shallow = !!shallow;
  // left shift size to get bits in the right spot for OR'ing into `chunk->m`
  shallow = shallow << LARGE_CHUNK_SHALLOW_BITMASK_SHIFT_AMOUNT;
  pony_assert(shallow == LARGE_CHUNK_SHALLOW_BITMASK || shallow == 0);
  chunk->m = (char*)(((uintptr_t)chunk->m & ~LARGE_CHUNK_SHALLOW_BITMASK) | shallow);
}

static uint32_t get_large_chunk_finaliser(chunk_t* chunk)
{
  pony_assert(!get_chunk_is_small_chunk(chunk));
  // `!!` to normalize to 1 or 0
  return !!(((uintptr_t)chunk->m & LARGE_CHUNK_FINALISER_BITMASK) == LARGE_CHUNK_FINALISER_BITMASK);
}

static void set_large_chunk_finaliser(chunk_t* chunk, uint32_t finaliser)
{
  pony_assert(!get_chunk_is_small_chunk(chunk));
  // `!!` to normalize to 1 or 0
  finaliser = !!finaliser;
  // left shift size to get bits in the right spot for OR'ing into `chunk->m`
  finaliser = finaliser << LARGE_CHUNK_FINALISER_BITMASK_SHIFT_AMOUNT;
  pony_assert(finaliser == LARGE_CHUNK_FINALISER_BITMASK || finaliser == 0);
  chunk->m = (char*)(((uintptr_t)chunk->m & ~LARGE_CHUNK_FINALISER_BITMASK) | finaliser);
}

static size_t get_small_chunk_size(chunk_t* chunk)
{
  pony_assert(get_chunk_is_small_chunk(chunk));
  return ((uintptr_t)chunk->m & SMALL_CHUNK_SIZECLASS_BITMASK) >> SMALL_CHUNK_SIZECLASS_SHIFT;
}

static void set_small_chunk_size(chunk_t* chunk, size_t size)
{
  pony_assert(get_chunk_is_small_chunk(chunk));
  pony_assert(size < HEAP_SIZECLASSES);

  // left shift size to get bits in the right spot for OR'ing into `chunk->m`
  size = size << SMALL_CHUNK_SIZECLASS_SHIFT;
  chunk->m = (char*)(((uintptr_t)chunk->m & ~SMALL_CHUNK_SIZECLASS_BITMASK) | (size & SMALL_CHUNK_SIZECLASS_BITMASK));
}

static bool get_chunk_needs_clearing(chunk_t* chunk)
{
  return (((uintptr_t)chunk->m & CHUNK_NEEDS_TO_BE_CLEARED_BITMASK) == CHUNK_NEEDS_TO_BE_CLEARED_BITMASK);
}

static void set_chunk_needs_clearing(chunk_t* chunk)
{
  chunk->m = (char*)((uintptr_t)chunk->m | CHUNK_NEEDS_TO_BE_CLEARED_BITMASK);
}

static void unset_chunk_needs_clearing(chunk_t* chunk)
{
  chunk->m = (char*)((uintptr_t)chunk->m & ~CHUNK_NEEDS_TO_BE_CLEARED_BITMASK);
}

static char* get_m(chunk_t* chunk)
{
  return (char*)((uintptr_t)chunk->m & CHUNK_M_BITMASK);
}

static void clear_chunk_flags(chunk_t* chunk)
{
  chunk->m = (char*)((uintptr_t)chunk->m & CHUNK_M_BITMASK);
}

static void large_pagemap(char* m, size_t size, chunk_t* chunk, pony_actor_t* actor)
{
  ponyint_pagemap_set_bulk(m, chunk, actor, size);
}

static void maybe_clear_chunk(chunk_t* chunk)
{
  // handle delayed clearing of the chunk since we don't do it
  // before starting GC anymore as an optimization
  if(get_chunk_needs_clearing(chunk))
  {
    if(get_chunk_is_small_chunk(chunk))
    {
      chunk->small.slots = sizeclass_empty[get_small_chunk_size(chunk)];
      chunk->small.shallow = sizeclass_empty[get_small_chunk_size(chunk)];
    }
    else
    {
      set_large_chunk_slot(chunk, 1);
      set_large_chunk_shallow(chunk, 1);
    }
    unset_chunk_needs_clearing(chunk);
  }
}

static void final_small(chunk_t* chunk, uint32_t force_finalisers_mask)
{
  pony_assert(get_chunk_is_small_chunk(chunk));
  // this function is only called in one of two scenarios.
  // 1. when the full chunk is being freed or destroyed. In this case
  //    `force_finalisers_mask` should always be `FORCE_ALL_FINALISERS` to make
  //    all finalisers get run no matter what.
  // 2. when part of the chunk is being freed. In this case
  //    `force_finalisers_mask` should always be `0` to make sure that
  //    only those finalisers get run as per the free `chunk->small.slots`
  pony_assert(force_finalisers_mask == FORCE_NO_FINALISERS ||
    force_finalisers_mask == FORCE_ALL_FINALISERS);
  uint32_t finalisers = chunk->small.finalisers & (chunk->small.slots | force_finalisers_mask);

  // exit early before any memory writes if no finalisers to run
  if(finalisers == 0)
    return;

  // unset all finalisers bits since we're going to run them all now
  chunk->small.finalisers = chunk->small.finalisers & ~(chunk->small.slots | force_finalisers_mask);

  // run any finalisers that need to be run
  void* p = NULL;

  uint64_t bit = 0;

  // if there's a finaliser to run for a used slot
  while(finalisers != 0)
  {
    bit = __pony_ctz(finalisers);
    p = get_m(chunk) + (bit << HEAP_MINBITS);

    // run finaliser
    pony_assert((*(pony_type_t**)p)->final != NULL);
    (*(pony_type_t**)p)->final(p);

    // clear bit just found in our local finaliser map
    finalisers &= (finalisers - 1);
  }
}

static void final_large(chunk_t* chunk, uint32_t mark)
{
  pony_assert(!get_chunk_is_small_chunk(chunk));
  if(get_large_chunk_finaliser(chunk) == 1)
  {
    // run finaliser
    char* m = get_m(chunk);
    pony_assert((*(pony_type_t**)m)->final != NULL);
    (*(pony_type_t**)m)->final(m);
    set_large_chunk_finaliser(chunk, 0);
  }
  (void)mark;
}

static void destroy_small(chunk_t* chunk, uint32_t mark)
{
  pony_assert(get_chunk_is_small_chunk(chunk));
  (void)mark;

  // run any finalisers that need running
  final_small(chunk, FORCE_ALL_FINALISERS);

  char* m = get_m(chunk);
  ponyint_pagemap_set(m, NULL, NULL);
#if !defined(PONY_NDEBUG) && !defined(USE_ADDRESS_SANITIZER)
  memset(m, 0, sizeof(block_t));
#endif
  POOL_FREE(block_t, m);
  POOL_FREE(chunk_t, chunk);
}

static void destroy_large(chunk_t* chunk, uint32_t mark)
{
  pony_assert(!get_chunk_is_small_chunk(chunk));

  (void)mark;

  // run any finalisers that need running
  final_large(chunk, mark);

  char* m = get_m(chunk);
  large_pagemap(m, chunk->large.size, NULL, NULL);

  if(m != NULL)
  {
#if !defined(PONY_NDEBUG) && !defined(USE_ADDRESS_SANITIZER)
    memset(m, 0, chunk->large.size);
#endif
    ponyint_pool_free_size(chunk->large.size, m);
  }

  POOL_FREE(chunk_t, chunk);
}

static void destroy_mixed(chunk_t* chunk, uint32_t mark)
{
  if(get_chunk_is_small_chunk(chunk))
    destroy_small(chunk, mark);
  else
    destroy_large(chunk, mark);
}

static size_t sweep_small(chunk_t* chunk, chunk_t** avail, chunk_t** full, chunk_t** to_destroy,
#ifdef USE_RUNTIMESTATS
  uint32_t empty, size_t size, size_t* mem_allocated, size_t* mem_used,
  size_t* num_allocated)
#else
  uint32_t empty, size_t size)
#endif
{
#ifdef USE_RUNTIMESTATS
  size_t num_slots_used = 0;
#endif

  size_t used = 0;
  chunk_t* next;

  while(chunk != NULL)
  {
    pony_assert(get_chunk_is_small_chunk(chunk));
    next = chunk->next;

    maybe_clear_chunk(chunk);

    chunk->small.slots &= chunk->small.shallow;

    set_chunk_needs_clearing(chunk);

    if(chunk->small.slots == 0)
    {
#ifdef USE_RUNTIMESTATS
      *mem_allocated += POOL_ALLOC_SIZE(chunk_t);
      *mem_allocated += POOL_ALLOC_SIZE(block_t);
      *mem_used += sizeof(chunk_t);
      *mem_used += sizeof(block_t);
      num_slots_used += sizeclass_slot_count[ponyint_heap_index(size)];
#endif
      used += sizeof(block_t);
      chunk->next = *full;
      *full = chunk;
    } else if(chunk->small.slots == empty) {
      // run finalisers for freed slots
      final_small(chunk, FORCE_ALL_FINALISERS);

      // cannot destroy chunk yet since the heap has not been fully swept yet
      // and another object may still reference this chunk and access it in a finaliser
      chunk->next = *to_destroy;
      *to_destroy = chunk;
    } else {
#ifdef USE_RUNTIMESTATS
      *mem_allocated += POOL_ALLOC_SIZE(chunk_t);
      *mem_allocated += POOL_ALLOC_SIZE(block_t);
      *mem_used += sizeof(chunk_t);
      *mem_used += (sizeof(block_t) - (__pony_popcount(chunk->small.slots) * size));
      num_slots_used += (sizeclass_slot_count[ponyint_heap_index(size)] - __pony_popcount(chunk->small.slots));
#endif
      used += (sizeof(block_t) -
        (__pony_popcount(chunk->small.slots) * size));

      // run finalisers for freed slots
      final_small(chunk, FORCE_NO_FINALISERS);

#if defined(USE_ADDRESS_SANITIZER)
      uint32_t empty_slots = chunk->small.slots;
      void* p = NULL;
      uint64_t bit = 0;

      // if there's an empty slot to poison memory for
      while(empty_slots != 0)
      {
        bit = __pony_ctz(empty_slots);
        p = get_m(chunk) + (bit << HEAP_MINBITS);

        ASAN_POISON_MEMORY_REGION(p, size);

        // clear bit for the slot we just poisoned
        empty_slots &= (empty_slots - 1);
      }
#endif

      chunk->next = *avail;
      *avail = chunk;
    }

    chunk = next;
  }

#ifdef USE_RUNTIMESTATS
  *num_allocated += num_slots_used;
#endif

  return used;
}

#ifdef USE_RUNTIMESTATS
static chunk_t* sweep_large(chunk_t* chunk, chunk_t* to_destroy_sized_array[], size_t* used, size_t* mem_allocated,
  size_t* mem_used, size_t* num_allocated)
#else
static chunk_t* sweep_large(chunk_t* chunk, chunk_t* to_destroy_sized_array[], size_t* used)
#endif
{
#ifdef USE_RUNTIMESTATS
  size_t num_slots_used = 0;
#endif

  chunk_t* list = NULL;
  chunk_t* next;

  while(chunk != NULL)
  {
    pony_assert(!get_chunk_is_small_chunk(chunk));
    next = chunk->next;

    maybe_clear_chunk(chunk);

    set_large_chunk_slot(chunk, get_large_chunk_slot(chunk) & get_large_chunk_shallow(chunk));

    set_chunk_needs_clearing(chunk);

    if(get_large_chunk_slot(chunk) == 0)
    {
      chunk->next = list;
      list = chunk;
#ifdef USE_RUNTIMESTATS
      *mem_allocated += POOL_ALLOC_SIZE(chunk_t);
      *mem_allocated += ponyint_pool_used_size(chunk->large.size);
      *mem_used += sizeof(chunk_t);
      *mem_used += chunk->large.size;
      num_slots_used++;
#endif
      *used += chunk->large.size;
    } else {
      // run finaliser
      final_large(chunk, 0);

      // cannot destroy chunk yet since the heap has not been fully swept yet
      // and another object may still reference this chunk and access it in a finaliser
      uint32_t destroy_index = heap_recycle_index(chunk->large.size);
      if(destroy_index >= HEAP_RECYCLE_LARGE_OVERFLOW)
      {
        // if the chunk overflow size, put in the overflow list to be sorted
        chunk->next = to_destroy_sized_array[HEAP_RECYCLE_LARGE_OVERFLOW];
        to_destroy_sized_array[HEAP_RECYCLE_LARGE_OVERFLOW] = chunk;
      } else {
        // otherwise put it in the sized recycle lists
        chunk->next = to_destroy_sized_array[destroy_index];
        to_destroy_sized_array[destroy_index] = chunk;
      }
    }

    chunk = next;
  }

#ifdef USE_RUNTIMESTATS
  *num_allocated += num_slots_used;
#endif

  return list;
}

static void chunk_list(chunk_fn f, chunk_t* current, uint32_t mark)
{
  chunk_t* next;

  while(current != NULL)
  {
    next = current->next;
    f(current, mark);
    current = next;
  }
}

uint32_t ponyint_heap_index(size_t size)
{
  // size is in range 1..HEAP_MAX
  // change to 0..((HEAP_MAX / HEAP_MIN) - 1) and look up in table
  return sizeclass_table[(size - 1) >> HEAP_MINBITS];
}

void ponyint_heap_setinitialgc(size_t size)
{
  heap_initialgc = (size_t)1 << size;
}

void ponyint_heap_setnextgcfactor(double factor)
{
  if(factor < 1.0)
    factor = 1.0;

  DTRACE1(GC_THRESHOLD, factor);
  heap_nextgc_factor = factor;
}

void ponyint_heap_init(heap_t* heap)
{
  memset(heap, 0, sizeof(heap_t));
  heap->next_gc = heap_initialgc;
}

void ponyint_heap_destroy(heap_t* heap)
{
  chunk_list(destroy_large, heap->large, 0);

  for(int i = 0; i < HEAP_RECYCLE_SIZECLASSES; i++)
  {
    if(HEAP_RECYCLE_MIXED == i)
      chunk_list(destroy_mixed, heap->recyclable[i], 0);
    else
      chunk_list(destroy_large, heap->recyclable[i], 0);
  }

  for(int i = 0; i < HEAP_SIZECLASSES; i++)
  {
    chunk_list(destroy_small, heap->small_free[i], 0);
    chunk_list(destroy_small, heap->small_full[i], 0);
  }
}

void ponyint_heap_final(heap_t* heap)
{
  chunk_list(final_large, heap->large, 0);

  for(int i = 0; i < HEAP_SIZECLASSES; i++)
  {
    chunk_list(final_small, heap->small_free[i], FORCE_ALL_FINALISERS);
    chunk_list(final_small, heap->small_full[i], FORCE_ALL_FINALISERS);
  }
}

void* ponyint_heap_alloc(pony_actor_t* actor, heap_t* heap, size_t size,
  uint32_t track_finalisers_mask)
{
  // 1. when `track_finalisers_mask == TRACK_NO_FINALISERS` then finalisers are
  //    disabled
  // 2. when `track_finalisers_mask == TRACK_ALL_FINALISERS` then
  //    finalisers are enabled
  pony_assert(track_finalisers_mask == TRACK_NO_FINALISERS ||
    track_finalisers_mask == TRACK_ALL_FINALISERS);

  if(size == 0)
  {
    return NULL;
  } else if(size <= HEAP_MAX) {
    return ponyint_heap_alloc_small(actor, heap, ponyint_heap_index(size),
      track_finalisers_mask);
  } else {
    return ponyint_heap_alloc_large(actor, heap, size, track_finalisers_mask);
  }
}

void* ponyint_heap_alloc_small(pony_actor_t* actor, heap_t* heap,
  uint32_t sizeclass, uint32_t track_finalisers_mask)
{
  // 1. when `track_finalisers_mask == TRACK_NO_FINALISERS` then finalisers are
  //    disabled
  // 2. when `track_finalisers_mask == TRACK_ALL_FINALISERS` then
  //    finalisers are enabled
  pony_assert(track_finalisers_mask == TRACK_NO_FINALISERS ||
    track_finalisers_mask == TRACK_ALL_FINALISERS);

  chunk_t* chunk = heap->small_free[sizeclass];
  void* m;

  // If there are none in this size class, get a new one.
  if(chunk != NULL)
  {
    // Clear and use the first available slot.
    uint32_t slots = chunk->small.slots;
    uint32_t bit = __pony_ctz(slots);
    slots &= ~((uint32_t)1 << bit);

    m = get_m(chunk) + (bit << HEAP_MINBITS);
    chunk->small.slots = slots;

    // note if a finaliser needs to run or not
    chunk->small.finalisers |= (track_finalisers_mask & ((uint32_t)1 << bit));

    if(slots == 0)
    {
      heap->small_free[sizeclass] = chunk->next;
      chunk->next = heap->small_full[sizeclass];
      heap->small_full[sizeclass] = chunk;
    }
  } else {
    chunk_t* n = NULL;

    // recycle a mixed chunk if available because it avoids setting the pagemap
    if (NULL != heap->recyclable[HEAP_RECYCLE_MIXED])
    {
      n = heap->recyclable[HEAP_RECYCLE_MIXED];
      heap->recyclable[HEAP_RECYCLE_MIXED] = n->next;

      // reset since it might have been a large chunk in a previous life
      clear_chunk_flags(n);
    }

    // If there are none in the recyclable list, get a new one.
    if (NULL == n)
    {
      n = (chunk_t*) POOL_ALLOC(chunk_t);
      n->m = (char*) POOL_ALLOC(block_t);
      ponyint_pagemap_set(get_m(n), n, actor);

#if defined(USE_ADDRESS_SANITIZER)
      ASAN_POISON_MEMORY_REGION(get_m(n), sizeof(block_t));
#endif
    }

    // we should always have a chunk and m at this point
    pony_assert(n != NULL);
    pony_assert(n->m != NULL);

    set_chunk_is_small_chunk(n);

    set_small_chunk_size(n, sizeclass);

#ifdef USE_RUNTIMESTATS
    actor->actorstats.heap_mem_used += sizeof(chunk_t);
    actor->actorstats.heap_mem_allocated += POOL_ALLOC_SIZE(chunk_t);
    actor->actorstats.heap_mem_allocated += POOL_ALLOC_SIZE(block_t);
#endif

    // note if a finaliser needs to run or not
    n->small.finalisers = (track_finalisers_mask & 1);

    // Clear the first bit.
    n->small.slots = sizeclass_init[sizeclass];
    n->next = NULL;

    // this is required for the heap garbage collection as it needs to know that
    // this chunk needs to be cleared
    set_chunk_needs_clearing(n);

    heap->small_free[sizeclass] = n;
    chunk = n;

    // Use the first slot.
    m = get_m(chunk);
  }

#ifdef USE_RUNTIMESTATS
  actor->actorstats.heap_alloc_counter++;
  actor->actorstats.heap_num_allocated++;
  actor->actorstats.heap_mem_used += SIZECLASS_SIZE(sizeclass);
#endif

#if defined(USE_ADDRESS_SANITIZER)
  ASAN_UNPOISON_MEMORY_REGION(m, SIZECLASS_SIZE(sizeclass));
#endif

  heap->used += SIZECLASS_SIZE(sizeclass);
  return m;
}

void* ponyint_heap_alloc_large(pony_actor_t* actor, heap_t* heap, size_t size,
  uint32_t track_finalisers_mask)
{
  // 1. when `track_finalisers_mask == TRACK_NO_FINALISERS` then finalisers are
  //    disabled
  // 2. when `track_finalisers_mask == TRACK_ALL_FINALISERS` then
  //    finalisers are enabled
  pony_assert(track_finalisers_mask == TRACK_NO_FINALISERS ||
    track_finalisers_mask == TRACK_ALL_FINALISERS);

  size = ponyint_pool_adjust_size(size);

  chunk_t* chunk = NULL;

  uint32_t recycle_index = heap_recycle_index(size);
  if(recycle_index >= HEAP_RECYCLE_LARGE_OVERFLOW)
  {
    // recycle a large chunk if available because it avoids setting the pagemap
    if (NULL != heap->recyclable[HEAP_RECYCLE_LARGE_OVERFLOW])
    {
      pony_assert(size > HEAP_RECYCLE_MIXED_SIZE);
      chunk_t* recycle = heap->recyclable[HEAP_RECYCLE_LARGE_OVERFLOW];
      chunk_t* prev = NULL;

      // short circuit as soon as we see a chunk that is too big because this list is sorted by size
      while (NULL != recycle && recycle->large.size <= size)
      {
        // we only recycle if the size is exactly what is required
        if (recycle->large.size == size)
        {
          if (NULL == prev)
            heap->recyclable[HEAP_RECYCLE_LARGE_OVERFLOW] = recycle->next;
          else
            prev->next = recycle->next;

          chunk = recycle;
          chunk->next = NULL;
          break;
        } else {
          prev = recycle;
          recycle = recycle->next;
        }
      }
    }
  } else {
    pony_assert(recycle_index < HEAP_RECYCLE_LARGE_OVERFLOW);
    pony_assert((HEAP_RECYCLE_MIXED != recycle_index && size > HEAP_RECYCLE_MIXED_SIZE) || (HEAP_RECYCLE_MIXED == recycle_index && HEAP_RECYCLE_MIXED_SIZE == size));
    // recycle a chunk from a sized list if available because it avoids setting the pagemap
    if (NULL != heap->recyclable[recycle_index])
    {
      chunk = heap->recyclable[recycle_index];
      heap->recyclable[recycle_index] = chunk->next;

      // reset since it might have been a small chunk in a previous life
      clear_chunk_flags(chunk);
      chunk->large.size = size;
      chunk->next = NULL;
    }
  }

  // If there is no suitable one in the recyclable list, get a new one.
  if (NULL == chunk)
  {
    chunk = (chunk_t*) POOL_ALLOC(chunk_t);
    chunk->large.size = size;
    chunk->m = (char*) ponyint_pool_alloc_size(size);
    large_pagemap(get_m(chunk), size, chunk, actor);

#if defined(USE_ADDRESS_SANITIZER)
    ASAN_POISON_MEMORY_REGION(get_m(chunk), size);
#endif
  }

  // we should always have a chunk and m of the right size at this point
  pony_assert(chunk != NULL);
  pony_assert(chunk->m != NULL);
  pony_assert(chunk->large.size == size);

#ifdef USE_RUNTIMESTATS
  actor->actorstats.heap_mem_used += sizeof(chunk_t);
  actor->actorstats.heap_mem_used += chunk->large.size;
  actor->actorstats.heap_mem_allocated += POOL_ALLOC_SIZE(chunk_t);
  actor->actorstats.heap_mem_allocated += ponyint_pool_used_size(size);
  actor->actorstats.heap_alloc_counter++;
  actor->actorstats.heap_num_allocated++;
#endif
  set_large_chunk_slot(chunk, 0);

  // this is required for the heap garbage collection as it needs to know that
  // this chunk needs to be cleared
  set_chunk_needs_clearing(chunk);

  // note if a finaliser needs to run or not
  set_large_chunk_finaliser(chunk, (track_finalisers_mask & 1));

  chunk->next = heap->large;
  heap->large = chunk;
  heap->used += chunk->large.size;

#if defined(USE_ADDRESS_SANITIZER)
  ASAN_UNPOISON_MEMORY_REGION(get_m(chunk), chunk->large.size);
#endif

  return get_m(chunk);
}

void* ponyint_heap_realloc(pony_actor_t* actor, heap_t* heap, void* p,
  size_t size, size_t copy)
{
  if(p == NULL)
    return ponyint_heap_alloc(actor, heap, size, TRACK_NO_FINALISERS);

#ifdef USE_RUNTIMESTATS
  actor->actorstats.heap_realloc_counter++;
#endif

  chunk_t* chunk = ponyint_pagemap_get_chunk(p);

  // We can't realloc memory that wasn't pony_alloc'ed since we can't know how
  // much to copy from the previous location.
  pony_assert(chunk != NULL);

  size_t oldsize;

  if(get_chunk_is_small_chunk(chunk))
  {
    size_t chunk_size = get_small_chunk_size(chunk);
    // Previous allocation was a ponyint_heap_alloc_small.
    void* ext = EXTERNAL_PTR(p, chunk_size);

    oldsize = SIZECLASS_SIZE(chunk_size) - ((uintptr_t)p - (uintptr_t)ext);
  } else {
    // Previous allocation was a ponyint_heap_alloc_large.

    oldsize = chunk->large.size - ((uintptr_t)p - (uintptr_t)get_m(chunk));
  }

  pony_assert(copy <= size);
  // Determine how much memory to copy.
  if(copy > size)
    copy = size;

  if(oldsize > copy)
    oldsize = copy;

  // Get new memory and copy from the old memory.
  void* q = ponyint_heap_alloc(actor, heap, size, TRACK_NO_FINALISERS);
  memcpy(q, p, oldsize);
  return q;
}

void ponyint_heap_used(heap_t* heap, size_t size)
{
  heap->used += size;
}

bool ponyint_heap_startgc(heap_t* heap
#ifdef USE_RUNTIMESTATS
  , pony_actor_t* actor)
#else
  )
#endif
{
  if(heap->used <= heap->next_gc)
    return false;

  // reset used to zero
  heap->used = 0;
#ifdef USE_RUNTIMESTATS
  actor->actorstats.heap_mem_allocated = 0;
  actor->actorstats.heap_mem_used = 0;
#endif
  return true;
}

bool ponyint_heap_mark(chunk_t* chunk, void* p)
{
  // If it's an internal pointer, we shallow mark it instead. This will
  // preserve the external pointer, but allow us to mark and recurse the
  // external pointer in the same pass.
  bool marked;

  maybe_clear_chunk(chunk);

  if(get_chunk_is_small_chunk(chunk))
  {
    // Calculate the external pointer.
    void* ext = EXTERNAL_PTR(p, get_small_chunk_size(chunk));

    // Shift to account for smallest allocation size.
    uint32_t slot = FIND_SLOT(ext, get_m(chunk));

    // Check if it was already marked.
    marked = (chunk->small.slots & slot) == 0;

    // A clear bit is in-use, a set bit is available.
    if(p == ext)
      chunk->small.slots &= ~slot;
    else
      chunk->small.shallow &= ~slot;
  } else {
    marked = get_large_chunk_slot(chunk) == 0;

    if(p == get_m(chunk))
      set_large_chunk_slot(chunk, 0);
    else
      set_large_chunk_shallow(chunk, 0);
  }

  return marked;
}

void ponyint_heap_mark_shallow(chunk_t* chunk, void* p)
{
  maybe_clear_chunk(chunk);

  if(get_chunk_is_small_chunk(chunk))
  {
    // Calculate the external pointer.
    void* ext = EXTERNAL_PTR(p, get_small_chunk_size(chunk));

    // Shift to account for smallest allocation size.
    uint32_t slot = FIND_SLOT(ext, get_m(chunk));

    // A clear bit is in-use, a set bit is available.
    chunk->small.shallow &= ~slot;
  } else {
    set_large_chunk_shallow(chunk, 0);
  }
}

void ponyint_heap_free(chunk_t* chunk, void* p)
{
  if(!get_chunk_is_small_chunk(chunk))
  {
    if(p == get_m(chunk))
    {
      // run finaliser if needed
      final_large(chunk, 0);

      ponyint_pool_free_size(chunk->large.size, get_m(chunk));
      chunk->m = NULL;
      set_large_chunk_slot(chunk, 1);
    }
    return;
  }

  // Calculate the external pointer.
  void* ext = EXTERNAL_PTR(p, get_small_chunk_size(chunk));

  if(p == ext)
  {
    // Shift to account for smallest allocation size.
    uint32_t slot = FIND_SLOT(ext, get_m(chunk));

    // check if there's a finaliser to run
    if((chunk->small.finalisers & slot) != 0)
    {
      // run finaliser
      (*(pony_type_t**)p)->final(p);

      // clear finaliser
      chunk->small.finalisers &= ~slot;
    }

    // free slot
    chunk->small.slots |= slot;
  }
}

void ponyint_heap_endgc(heap_t* heap
#ifdef USE_RUNTIMESTATS
  , pony_actor_t* actor)
#else
  )
#endif
{
  size_t used = 0;
#ifdef USE_RUNTIMESTATS
  size_t mem_allocated = 0;
  size_t mem_used = 0;
  size_t num_allocated = 0;
#endif

  // make a copy of all the heap list pointers into a temporary heap
  size_t amount_to_copy_clear = offsetof(heap_t, recyclable);
  heap_t theap = {};
  heap_t* temp_heap = &theap;
  memcpy(temp_heap, heap, amount_to_copy_clear);

  // set all the heap list pointers to NULL in the real heap to ensure that
  // any new allocations by finalisers during the GC process don't reuse memory
  // freed during the GC process
  memset(heap, 0, amount_to_copy_clear);

  // sweep all the chunks in the temporary heap while accumulating chunks to recycle
  // NOTE: the real heap will get new chunks allocated and added to it during this process
  //       if a finaliser allocates memory... we have to make sure that finalisers don't
  //       reuse memory being freed or we'll end up with a use-after-free bug due to
  //       potential cross objects references in finalisers after memory has been reused.
  for(int i = 0; i < HEAP_SIZECLASSES; i++)
  {
    chunk_t* list1 = temp_heap->small_free[i];
    chunk_t* list2 = temp_heap->small_full[i];

    temp_heap->small_free[i] = NULL;
    temp_heap->small_full[i] = NULL;

    chunk_t** avail = &temp_heap->small_free[i];
    chunk_t** full = &temp_heap->small_full[i];

    size_t size = SIZECLASS_SIZE(i);
    uint32_t empty = sizeclass_empty[i];

#ifdef USE_RUNTIMESTATS
    used += sweep_small(list1, avail, full, &temp_heap->recyclable[HEAP_RECYCLE_MIXED], empty, size,
      &mem_allocated, &mem_used, &num_allocated);
    used += sweep_small(list2, avail, full, &temp_heap->recyclable[HEAP_RECYCLE_MIXED], empty, size,
      &mem_allocated, &mem_used, &num_allocated);
#else
    used += sweep_small(list1, avail, full, &temp_heap->recyclable[HEAP_RECYCLE_MIXED], empty, size);
    used += sweep_small(list2, avail, full, &temp_heap->recyclable[HEAP_RECYCLE_MIXED], empty, size);
#endif
  }

#ifdef USE_RUNTIMESTATS
  temp_heap->large = sweep_large(temp_heap->large, temp_heap->recyclable, &used, &mem_allocated, &mem_used,
    &num_allocated);
#else
  temp_heap->large = sweep_large(temp_heap->large, temp_heap->recyclable, &used);
#endif

  // we need to combine the temporary heap lists with the real heap lists
  // because the real heap might not be empty due to allocations in finalisers
  // any new chunks with no finalisers to run can be destroyed immediately
  // the new allocations will get GC'd in the next GC cycle even though they
  // are definitely not reachable by the actor anymore
  for(int i = 0; i < HEAP_SIZECLASSES; i++)
  {
    // iterate to the end of the real heap list and append the temporary heap list
    // because the real heap list is likely to be smaller than the temporary heap list
    // and remove any chunks that have no finalisers to run immediately
    chunk_t** sc_temp = &heap->small_free[i];
    while (*sc_temp != NULL)
    {
      if ((*sc_temp)->small.finalisers == 0)
      {
        chunk_t* temp = *sc_temp;
        *sc_temp = temp->next;
        temp->next = temp_heap->recyclable[HEAP_RECYCLE_MIXED];
        temp_heap->recyclable[HEAP_RECYCLE_MIXED] = temp;
      } else {
        sc_temp = &(*sc_temp)->next;
      }
    }
    *sc_temp = temp_heap->small_free[i];

    // iterate to the end of the real heap list and append the temporary heap list
    // because the real heap list is likely to be smaller than the temporary heap list
    // and remove any chunks that have no finalisers to run immediately
    sc_temp = &heap->small_full[i];
    while (*sc_temp != NULL)
    {
      if ((*sc_temp)->small.finalisers == 0)
      {
        chunk_t* temp = *sc_temp;
        *sc_temp = temp->next;
        temp->next = temp_heap->recyclable[HEAP_RECYCLE_MIXED];
        temp_heap->recyclable[HEAP_RECYCLE_MIXED] = temp;
      } else {
        sc_temp = &(*sc_temp)->next;
      }
    }
    *sc_temp = temp_heap->small_full[i];
  }

  // iterate to the end of the real heap list and append the temporary heap list
  // because the real heap list is likely to be smaller than the temporary heap list
  // and remove any chunks that have no finalisers to run immediately
  chunk_t** lc_temp = &heap->large;
  while (*lc_temp != NULL)
  {
    if (get_large_chunk_finaliser(*lc_temp) == 0)
    {
      chunk_t* temp = *lc_temp;
      *lc_temp = temp->next;

      uint32_t recycle_index = heap_recycle_index(temp->large.size);
      if(recycle_index >= HEAP_RECYCLE_LARGE_OVERFLOW)
      {
        // if the chunk overflow size, put in the overflow list to be sorted
        temp->next = temp_heap->recyclable[HEAP_RECYCLE_LARGE_OVERFLOW];
        temp_heap->recyclable[HEAP_RECYCLE_LARGE_OVERFLOW] = temp;
      } else {
        // otherwise put it in the sized recycle lists
        temp->next = temp_heap->recyclable[recycle_index];
        temp_heap->recyclable[recycle_index] = temp;
      }
    } else {
      lc_temp = &(*lc_temp)->next;
    }
  }
  *lc_temp = temp_heap->large;

  // destroy all the chunks that didn't get re-used since the last GC run so
  // that other actors can re-use the memory from the pool
  for(int i = 0; i < HEAP_RECYCLE_SIZECLASSES; i++)
  {
    if(HEAP_RECYCLE_MIXED == i)
      chunk_list(destroy_mixed, heap->recyclable[i], 0);
    else
      chunk_list(destroy_large, heap->recyclable[i], 0);
  }


  // save any chunks that can be recycled from this GC run
  // sort large chunks by the size of the chunks
  for(int i = 0; i < HEAP_RECYCLE_SIZECLASSES; i++)
  {
    if(HEAP_RECYCLE_LARGE_OVERFLOW == i)
      heap->recyclable[i] = sort_large_chunk_list_by_size(temp_heap->recyclable[i]);
    else
      heap->recyclable[i] = temp_heap->recyclable[i];
  }

#if defined(USE_ADDRESS_SANITIZER)
  for(int i = 0; i < HEAP_RECYCLE_SIZECLASSES; i++)
  {
    chunk_t* sc = heap->recyclable[i];
    while (sc != NULL)
    {
      if(get_chunk_is_small_chunk(sc))
      {
        ASAN_POISON_MEMORY_REGION(get_m(sc), sizeof(block_t));
      } else {
        ASAN_POISON_MEMORY_REGION(get_m(sc), sc->large.size);
      }
      sc = sc->next;
    }
  }
#endif

  // Foreign object sizes will have been added to heap->used already. Here we
  // add local object sizes as well and set the next gc point for when memory
  // usage has increased.
  heap->used += used;
#ifdef USE_RUNTIMESTATS
  actor->actorstats.heap_mem_allocated += mem_allocated;
  actor->actorstats.heap_mem_used += mem_used;
  actor->actorstats.heap_free_counter += (actor->actorstats.heap_num_allocated - num_allocated);
  actor->actorstats.heap_num_allocated = num_allocated;
  actor->actorstats.heap_gc_counter++;
#endif
  heap->next_gc = (size_t)((double)heap->used * heap_nextgc_factor);

  if(heap->next_gc < heap_initialgc)
    heap->next_gc = heap_initialgc;
}

size_t ponyint_heap_size(chunk_t* chunk)
{
  if(get_chunk_is_small_chunk(chunk))
    return SIZECLASS_SIZE(get_small_chunk_size(chunk));

  return chunk->large.size;
}

// C99 requires inline symbols to be present in a compilation unit for un-
// optimized code

uint32_t __pony_clz(uint32_t x);
uint32_t __pony_clzzu(size_t x);
uint32_t __pony_ffszu(size_t x);
uint32_t __pony_ctz(uint32_t x);
uint32_t __pony_ffs(uint32_t x);
uint32_t __pony_ffsll(uint64_t x);
uint32_t __pony_clzll(uint64_t x);
uint32_t __pony_popcount(uint32_t x);
