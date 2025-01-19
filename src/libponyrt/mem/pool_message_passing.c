#include "pool.h"
#include "alloc.h"
#include "../ds/fun.h"
#include "../ds/hash.h"
#include "../sched/cpu.h"
#include "ponyassert.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <platform.h>

/// Allocations this size and above are aligned on this size. This is needed
/// so that the pagemap for the heap is aligned.
#define POOL_ALIGN_INDEX (POOL_ALIGN_BITS - POOL_MIN_BITS)
#define POOL_MMAP_MASK (~(POOL_MMAP - 1))
#define POOL_MMAP_ALIGN(x) ((void*)(((uintptr_t)x) & POOL_MMAP_MASK))

#ifdef POOL_USE_MESSAGE_PASSING

#if defined(PLATFORM_IS_WINDOWS)
pony_static_assert(false, "pool message passing is not currently implemented for windows!");
#endif

#ifdef USE_ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#endif

#ifdef USE_VALGRIND
#include <valgrind/valgrind.h>
#include <valgrind/helgrind.h>
#endif


static size_t memallocmap_hash(void* alloc)
{
  return ponyint_hash_ptr(alloc);
}

static bool memallocmap_cmp(void* a, void* b)
{
  return a == b;
}

DECLARE_HASHMAP(memallocmap, memallocmap_t, void);

DEFINE_HASHMAP(memallocmap, memallocmap_t, void, memallocmap_hash,
  memallocmap_cmp, NULL);

/// An item on a per-size free list.
typedef struct pool_item_t
{
  struct pool_item_t* next;
} pool_item_t;

/// A per-size thread-local free list header.
typedef struct pool_local_t
{
  pool_item_t* pool;
  size_t length;
  char* start;
  char* end;
} pool_local_t;

/// An item on a list of free blocks.
typedef struct pool_block_t
{
  struct pool_block_t* next;
  size_t size;
} pool_block_t;

/// A list of free blocks header.
typedef struct pool_block_header_t
{
  pool_block_t* head;
  size_t total_size;
  size_t largest_size;
} pool_block_header_t;

#ifndef PONY_NDEBUG
// only used as part of a pony_assert
static size_t pool_sizeclasses[POOL_COUNT] =
{
  POOL_MIN << 0,
  POOL_MIN << 1,
  POOL_MIN << 2,
  POOL_MIN << 3,
  POOL_MIN << 4,
  POOL_MIN << 5,
  POOL_MIN << 6,
  POOL_MIN << 7,
  POOL_MIN << 8,
  POOL_MIN << 9,
  POOL_MIN << 10,
  POOL_MIN << 11,
  POOL_MIN << 12,
  POOL_MIN << 13,
  POOL_MIN << 14,
  POOL_MIN << 15,
  POOL_MIN << 16,
};
#endif

typedef struct pool_remote_allocs_t
{
  pool_item_t* pool_remote[POOL_COUNT];
  pool_block_t* pool_remote_blocks;
} pool_remote_allocs_t;

static __pony_thread_local pool_local_t pool_local[POOL_COUNT];
static __pony_thread_local pool_block_header_t pool_local_block_header;
static __pony_thread_local memallocmap_t memallocmap;
static __pony_thread_local void* low_virt_alloc = (void*)-1;
static __pony_thread_local void* high_virt_alloc = NULL;

// include it after defining `pool_item_t`/`pool_block_t`
#include "pool_sorting.h"

// check if the pointer is within the virtual allocation range or not
// if it is, it's possibly a local allocation, if not, it's definitely a remote allocation
// this is likely to be a less helpful check in non-numa environments
static bool possibly_a_local_allocation(void* p)
{
  return (p >= low_virt_alloc) && (p < high_virt_alloc);
}

static bool is_a_local_allocation(void* p, bool* prev_alloc_pool_addr_is_local, void** prev_alloc_pool_addr)
{
  // round down to the nearest POOL_MMAP boundary
  void* curr_alloc_pool_addr = POOL_MMAP_ALIGN(p);

  // fast path
  // it's a local allocation because the previous allocation was in the same mmap'd range
  if (*prev_alloc_pool_addr_is_local && *prev_alloc_pool_addr == curr_alloc_pool_addr)
  {
    *prev_alloc_pool_addr = curr_alloc_pool_addr;
    *prev_alloc_pool_addr_is_local = true;
    return true;
  }

  // fast path
  // it's a remote allocation because the pointer is definitely not a local allocation
  if (!possibly_a_local_allocation(curr_alloc_pool_addr))
  {
    *prev_alloc_pool_addr = curr_alloc_pool_addr;
    *prev_alloc_pool_addr_is_local = false;
    return false;
  }

  // slow path
  // it may or may not be a local allocation; need to check the memalloc map to be sure

  // check if it's in the memallocmap to confirm if it's a local allocation or not
  size_t index = -1;
  bool is_local = (NULL != memallocmap_get(&memallocmap, curr_alloc_pool_addr, &index));

  *prev_alloc_pool_addr = curr_alloc_pool_addr;
  *prev_alloc_pool_addr_is_local = is_local;

  return is_local;
}

static void pool_block_insert(pool_block_t* block)
{
  pool_block_t* next = pool_local_block_header.head;
  pool_block_t* prev = NULL;

  while(NULL != next)
  {
    if(block->size <= next->size)
      break;

    prev = next;
    next = next->next;
  }

  block->next = next;

  if(NULL != prev)
    prev->next = block;
  else
    pool_local_block_header.head = block;
}

static void* pool_block_get(size_t size)
{
  pool_block_t* head_block = pool_local_block_header.head;
  if(pool_local_block_header.largest_size >= size)
  {
    pool_block_t* block = head_block;
    pool_block_t* prev = NULL;

    while(NULL != block)
    {
      if(block->size > size)
      {
        // Use size bytes from the end of the block. This allows us to keep the
        // block info inside the block instead of using another data structure.
        size_t rem = block->size - size;
        block->size = rem;
        pool_local_block_header.total_size -= size;

        if((NULL != prev) && (prev->size > block->size))
        {
          // If we are now smaller than the previous block, move us forward in
          // the list.
          if(NULL == block->next)
            pool_local_block_header.largest_size = prev->size;

          // Remove the block from the list.
          if(NULL != prev)
            prev->next = block->next;
          else
            pool_local_block_header.head = block->next;

          pool_block_insert(block);
        } else if(NULL == block->next) {
          pool_local_block_header.largest_size = rem;
        }

        return (char*)block + rem;
      } else if(block->size == size) {
        if(NULL == block->next)
        {
          pool_local_block_header.largest_size =
            (NULL == prev) ? 0 : prev->size;
        }

        // Remove the block from the list.
        if(NULL != prev)
          prev->next = block->next;
        else
          pool_local_block_header.head = block->next;

        // Return the block pointer itself.
        pool_local_block_header.total_size -= size;
        return block;
      }

      prev = block;
      block = block->next;
    }

    // If we didn't find any suitable block, something has gone really wrong.
    pony_assert(false);
  }

  // no block big enough available
  return NULL;
}

static void add_alloc_to_memallocmap(void* p, size_t size)
{
  // chop up the allocation into POOL_MMAP sized blocks and put them into the map
  // this is so that we can keep track of the allocations and free them later
  while (size > 0)
  {
    memallocmap_put(&memallocmap, p);
    p = (char*)p + POOL_MMAP;
    size -= POOL_MMAP;
  }
}

static void* pool_alloc_pages(size_t size)
{
  // Try to get a block from the pool_local_block_header
  void* p = pool_block_get(size);

  if(NULL != p)
    return p;

  // We have no free blocks big enough.
  // make sure we allocate enough memory to satisfy the request rounded up to the next POOL_MMAP boundary
  size_t alloc_size = (size + POOL_MMAP) & POOL_MMAP_MASK;

  pool_block_t* block = (pool_block_t*)ponyint_virt_alloc_aligned(alloc_size);

#ifdef USE_ADDRESS_SANITIZER
  ASAN_POISON_MEMORY_REGION(block, alloc_size);
#endif

  // track the virtual allocation range
  if ((void*)block < low_virt_alloc)
    low_virt_alloc = block;

  if ((void*)block + alloc_size > high_virt_alloc)
    high_virt_alloc = (void*)block + alloc_size;

#ifdef USE_ADDRESS_SANITIZER
  ASAN_UNPOISON_MEMORY_REGION(block, sizeof(pool_block_t));
#endif

  // add the whole virtual allocation into the pool_local_block_header
  block->size = alloc_size;
  block->next = NULL;
  pool_block_insert(block);
  pool_local_block_header.total_size += alloc_size;
  if(pool_local_block_header.largest_size < alloc_size)
    pool_local_block_header.largest_size = alloc_size;

  // put the allocation into the map (this will initialize it or potentially
  // resize it and allocate from the block)
  add_alloc_to_memallocmap(block, alloc_size);

  p = pool_block_get(size);

  pony_assert(NULL != p);

  return p;
}

static void pool_free_pages(void* p, size_t size)
{
#ifdef USE_ADDRESS_SANITIZER
  ASAN_UNPOISON_MEMORY_REGION(p, sizeof(pool_block_t));
#endif

  pool_block_t* block = (pool_block_t*)p;
  block->next = NULL;
  block->size = size;

  pool_block_insert(block);

  pool_local_block_header.total_size += size;
  if(pool_local_block_header.largest_size < size)
    pool_local_block_header.largest_size = size;
}

static void* pool_get(size_t index)
{
  // Try per-size thread-local free list first.
  pool_local_t* local = &pool_local[index];
  pool_item_t* p = local->pool;

  if(NULL != p)
  {
    local->pool = p->next;
    local->length--;
    return p;
  }

  size_t sizeclass = POOL_MIN << index;

  pony_assert(sizeclass == pool_sizeclasses[index]);

  if(sizeclass < POOL_ALIGN)
  {
    // Check our per-size local-local free block.
    if(local->start < local->end)
    {
      void* p = local->start;
      local->start += sizeclass;
      return p;
    }

    // Use the pool allocator to get a block POOL_ALIGN bytes in size
    // and treat it as a free block.
    // ?? do we want to take the block from the pool_local_block_header only when we're planning on splitting it into tiny pieces?
    char* mem = (char*)pool_get(POOL_ALIGN_INDEX);
    local->start = mem + sizeclass;
    local->end = mem + POOL_ALIGN;
    return mem;
  }

  // Pull size bytes from the list of free blocks. Don't use a size-specific
  // free block.
  return pool_alloc_pages(sizeclass);
}

void* ponyint_pool_alloc(size_t index)
{
  void* p = pool_get(index);

#ifdef USE_ADDRESS_SANITIZER
  // TODO: if we know the original size requested, we can only unpoison that
  // instead of the whole sizeclass block size
  ASAN_UNPOISON_MEMORY_REGION(p, POOL_MIN << index);
#endif

#ifdef USE_VALGRIND
  VALGRIND_HG_CLEAN_MEMORY(p, POOL_SIZE(index));
  VALGRIND_MALLOCLIKE_BLOCK(p, POOL_SIZE(index), 0, 0);
#endif

  return p;
}

void ponyint_pool_free(size_t index, void* p)
{
#ifdef USE_VALGRIND
  VALGRIND_HG_CLEAN_MEMORY(p, POOL_SIZE(index));
#endif

  pony_assert(index < POOL_COUNT);

#ifdef USE_ADDRESS_SANITIZER
  ASAN_POISON_MEMORY_REGION(p, POOL_MIN << index);
  ASAN_UNPOISON_MEMORY_REGION(p, sizeof(pool_item_t));
#endif

  pool_item_t* lp = (pool_item_t*)p;
  pool_local_t* local = &pool_local[index];
  lp->next = local->pool;
  local->pool = lp;
  local->length++;

#ifdef USE_VALGRIND
  VALGRIND_FREELIKE_BLOCK(p, 0);
#endif
}

static void* pool_alloc_size(size_t size)
{
  void* p = pool_alloc_pages(size);

#ifdef USE_VALGRIND
  VALGRIND_HG_CLEAN_MEMORY(p, size);
  VALGRIND_MALLOCLIKE_BLOCK(p, size, 0, 0);
#endif

  return p;
}

void* ponyint_pool_alloc_size(size_t size)
{
  size_t index = ponyint_pool_index(size);

  if(index < POOL_COUNT)
    return ponyint_pool_alloc(index);

  size = ponyint_pool_adjust_size(size);
  void* p = pool_alloc_size(size);

#ifdef USE_ADDRESS_SANITIZER
  // TODO: if we know the original size requested, we can only unpoison that
  // instead of the whole sizeclass block size
  ASAN_UNPOISON_MEMORY_REGION(p, size);
#endif

  return p;
}

static void pool_free_size(size_t size, void* p)
{
#ifdef USE_VALGRIND
  VALGRIND_HG_CLEAN_MEMORY(p, size);
#endif

#ifdef USE_ADDRESS_SANITIZER
  ASAN_POISON_MEMORY_REGION(p, size);
#endif

  pool_free_pages(p, size);


#ifdef USE_VALGRIND
  VALGRIND_FREELIKE_BLOCK(p, 0);
#endif
}

void ponyint_pool_free_size(size_t size, void* p)
{
  size_t index = ponyint_pool_index(size);

  if(index < POOL_COUNT)
    return ponyint_pool_free(index, p);

  size = ponyint_pool_adjust_size(size);
  pool_free_size(size, p);
}

void* ponyint_pool_realloc_size(size_t old_size, size_t new_size, void* p)
{
  // Can only reuse the old pointer if the old index/adjusted size is equal to
  // the new one, not greater.

  if(NULL == p)
    return ponyint_pool_alloc_size(new_size);

  size_t old_index = ponyint_pool_index(old_size);
  size_t new_index = ponyint_pool_index(new_size);
  size_t old_adj_size = 0;

  void* new_p;

  if(new_index < POOL_COUNT)
  {
    if(old_index == new_index)
      return p;

    new_p = ponyint_pool_alloc(new_index);
  } else {
    size_t new_adj_size = ponyint_pool_adjust_size(new_size);

    if(old_index >= POOL_COUNT)
    {
      old_adj_size = ponyint_pool_adjust_size(old_size);

      if(old_adj_size == new_adj_size)
        return p;
    }

    new_p = pool_alloc_size(new_adj_size);

#ifdef USE_ADDRESS_SANITIZER
  // TODO: if we know the original size requested, we can only unpoison that
  // instead of the whole sizeclass block size
    ASAN_UNPOISON_MEMORY_REGION(new_p, new_adj_size);
#endif
  }

  memcpy(new_p, p, old_size < new_size ? old_size : new_size);

  if(old_index < POOL_COUNT)
    ponyint_pool_free(old_index, p);
  else
    pool_free_size(old_adj_size, p);

  return new_p;
}

void ponyint_pool_thread_cleanup()
{
  // TODO: in theory we should be able to free all the virtual allocations we've made
  // to reset everything for a clean shutdown
}

static pool_item_t* gather_remote_allocs_to_send(pool_local_t* local)
{
  pool_item_t* p = local->pool;
  pool_item_t* next = NULL;
  pool_item_t* prev = NULL;
  pool_item_t* allocs_to_send = NULL;
  void* prev_alloc_pool_addr = NULL;
  bool prev_alloc_pool_addr_is_local = false;

  while (NULL != p)
  {
    next = p->next;

    if (is_a_local_allocation(p, &prev_alloc_pool_addr_is_local, &prev_alloc_pool_addr))
    {
      prev = p;
    } else {
      if (NULL == prev)
      {
        local->pool = next;
      } else {
        prev->next = next;
      }

      // add the allocation to the allocs_to_send
      p->next = allocs_to_send;
      allocs_to_send = p;
    }

    p = next;
  }
  return allocs_to_send;
}

static pool_block_t* gather_remote_blocks_to_send()
{
  pool_block_t* block = pool_local_block_header.head;
  pool_block_t* next = NULL;
  pool_block_t* prev = NULL;
  pool_block_t* blocks_to_send = NULL;
  void* prev_alloc_pool_addr = NULL;
  bool prev_alloc_pool_addr_is_local = false;

  while(NULL != block)
  {
    next = block->next;

    if (is_a_local_allocation(block, &prev_alloc_pool_addr_is_local, &prev_alloc_pool_addr))
    {
      prev = block;
    } else {
      if(NULL == block->next)
      {
        pool_local_block_header.largest_size =
          (NULL == prev) ? 0 : prev->size;
      }

      pool_local_block_header.total_size -= block->size;

      // Remove the block from the list.
      if(NULL != prev)
        prev->next = next;
      else
        pool_local_block_header.head = next;

      block->next = blocks_to_send;
      blocks_to_send = block;
    }

    block = next;
  }

  return blocks_to_send;
}

void ponyint_pool_receive_remote_allocations(pool_remote_allocs_t* remote_allocs)
{
  for (int i = 0; i < POOL_COUNT; i++)
  {
    pool_item_t* p = remote_allocs->pool_remote[i];

    if (NULL == p)
      continue;

    while (NULL != p && NULL != p->next)
    {
      p = p->next;
    }

    pool_local_t* local = &pool_local[i];
    p->next = local->pool;
    local->pool = remote_allocs->pool_remote[i];

    remote_allocs->pool_remote[i] = NULL;
  }

  pool_block_t* block = remote_allocs->pool_remote_blocks;
  pool_block_t* next = NULL;
  void* prev_alloc_pool_addr = NULL;
  void* temp_alloc_pool_addr = NULL;
  bool prev_alloc_pool_addr_is_local = false;
  bool temp_alloc_pool_addr_is_local = false;

  while(NULL != block)
  {
    if (is_a_local_allocation(block, &prev_alloc_pool_addr_is_local, &prev_alloc_pool_addr))
    {
      temp_alloc_pool_addr = prev_alloc_pool_addr;
      temp_alloc_pool_addr_is_local = prev_alloc_pool_addr_is_local;

      if(block->size + block == block->next && is_a_local_allocation(block->next, &temp_alloc_pool_addr_is_local, &temp_alloc_pool_addr))
      {
        // coalesce the blocks if they're adjacent to each other and both local allocations
        block->size += block->next->size;
        block->next = block->next->next;
      } else {
        // insert the block if they cannot be coalesced
        next = block->next;

        pool_block_insert(block);

        pool_local_block_header.total_size += block->size;
        if(pool_local_block_header.largest_size < block->size)
          pool_local_block_header.largest_size = block->size;

        block = next;
      }
    } else {
      // don't coalesce blocks that don't belong to this scheduler but insert them into the pool for use
      next = block->next;

      pool_block_insert(block);

      pool_local_block_header.total_size += block->size;
      if(pool_local_block_header.largest_size < block->size)
        pool_local_block_header.largest_size = block->size;

      block = next;
    }
  }

  remote_allocs->pool_remote_blocks = NULL;
}

void ponyint_pool_gather_receive_remote_allocations(pool_remote_allocs_t* remote_allocs)
{
  // scan the remote allocations and add them to the local pool if they're in the virtual allocation range for this thread
  for (int i = 0; i < POOL_COUNT; i++)
  {
    pool_local_t* local = &pool_local[i];

    // get local ones that need sending
    pool_item_t* allocs_to_send = gather_remote_allocs_to_send(local);

    pool_item_t* p = remote_allocs->pool_remote[i];
    pool_item_t* next = NULL;
    pool_item_t* prev = NULL;
    void* prev_alloc_pool_addr = NULL;
    bool prev_alloc_pool_addr_is_local = false;

    while (NULL != p)
    {
      next = p->next;

      if (is_a_local_allocation(p, &prev_alloc_pool_addr_is_local, &prev_alloc_pool_addr))
      {
        if (NULL == prev)
        {
          remote_allocs->pool_remote[i] = next;
        } else {
          prev->next = next;
        }

        p->next = local->pool;
        local->pool = p;
        local->length++;
      } else {
        prev = p;
      }

      p = next;
    }

    // append allocs to send to the end of the remote_allocs list
    if (NULL == prev)
    {
      remote_allocs->pool_remote[i] = allocs_to_send;
    } else {
      prev->next = allocs_to_send;
    }

    // sort by address before sending so that it's more efficient to process them on the receiving side
    remote_allocs->pool_remote[i] = sort_pool_item_list_by_address(remote_allocs->pool_remote[i]);
  }

  pool_block_t* remote_blocks = gather_remote_blocks_to_send();

  pool_block_t* block = remote_allocs->pool_remote_blocks;
  pool_block_t* next = NULL;
  pool_block_t* prev = NULL;
  void* prev_alloc_pool_addr = NULL;
  void* temp_alloc_pool_addr = NULL;
  bool prev_alloc_pool_addr_is_local = false;
  bool temp_alloc_pool_addr_is_local = false;

  while(NULL != block)
  {
    if (is_a_local_allocation(block, &prev_alloc_pool_addr_is_local, &prev_alloc_pool_addr))
    {
      temp_alloc_pool_addr = prev_alloc_pool_addr;
      temp_alloc_pool_addr_is_local = prev_alloc_pool_addr_is_local;

      if(block->size + block == block->next && is_a_local_allocation(block->next, &temp_alloc_pool_addr_is_local, &temp_alloc_pool_addr))
      {
        // coalesce the blocks if they're adjacent to each other and both local allocations
        block->size += block->next->size;
        block->next = block->next->next;
      } else {
        // insert the block if they cannot be coalesced
        next = block->next;

        if(NULL == prev)
        {
          remote_allocs->pool_remote_blocks = next;
        } else {
          prev->next = next;
        }

        pool_block_insert(block);

        pool_local_block_header.total_size += block->size;
        if(pool_local_block_header.largest_size < block->size)
          pool_local_block_header.largest_size = block->size;

        block = next;
      }
    } else {
      next = block->next;
      prev = block;
      block = next;
    }
  }

  // append allocs to send to the end of the remote_allocs list
  if (NULL == prev)
  {
    remote_allocs->pool_remote_blocks = remote_blocks;
  } else {
    prev->next = remote_blocks;
  }

  // sort by address before sending so that it's more efficient to process them on the receiving side
  remote_allocs->pool_remote_blocks = sort_pool_block_list_by_address(remote_allocs->pool_remote_blocks);
}

pool_remote_allocs_t* ponyint_initialize_pool_message_passing()
{
  return POOL_ALLOC(pool_remote_allocs_t);
}

#endif
