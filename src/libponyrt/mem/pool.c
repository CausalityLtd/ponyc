#define PONY_WANT_ATOMIC_DEFS

#include "pool.h"
#include "alloc.h"
#include "../ds/fun.h"
#include "../sched/cpu.h"
#include "ponyassert.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <platform.h>
#include <pony/detail/atomics.h>

/// Allocations this size and above are aligned on this size. This is needed
/// so that the pagemap for the heap is aligned.
#define POOL_ALIGN_INDEX (POOL_ALIGN_BITS - POOL_MIN_BITS)
#define POOL_ALIGN_MASK (POOL_ALIGN - 1)

#ifdef POOL_USE_DEFAULT

#ifdef USE_VALGRIND
#include <valgrind/valgrind.h>
#include <valgrind/helgrind.h>
#endif

/// When we mmap, pull at least this many bytes.
#ifdef PLATFORM_IS_ILP32
#  define POOL_MMAP (16 * 1024 * 1024) // 16 MB
#else
#  ifdef PLATFORM_IS_WINDOWS
#    define POOL_MMAP (16 * 1024 * 1024) // 16 MB
#  else
#    define POOL_MMAP (128 * 1024 * 1024) // 128 MB
#  endif
#endif

/// An item on a per-size thread-local free list.
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

/// A free list on a per-size global list of free lists.
typedef struct pool_central_t
{
  pool_item_t* next;
  uintptr_t length;
  struct pool_central_t* central;
} pool_central_t;

PONY_ABA_PROTECTED_PTR_DECLARE(pool_central_t)

/// A per-size global list of free lists header.
typedef struct pool_global_t
{
  size_t size;
  size_t count;
  PONY_ATOMIC_ABA_PROTECTED_PTR(pool_central_t) central;
} pool_global_t;

/// An item on an either thread-local or global list of free blocks.
typedef struct pool_block_t
{
  struct pool_block_t* prev;
  union
  {
    struct pool_block_t* next;
    PONY_ATOMIC(struct pool_block_t*) global;
  };
  size_t size;
  PONY_ATOMIC(bool) acquired;

#if defined(_MSC_VER)
  pool_block_t() { }
#endif
} pool_block_t;

/// A thread local list of free blocks header.
typedef struct pool_block_header_t
{
  pool_block_t* head;
  size_t total_size;
  size_t largest_size;
} pool_block_header_t;

#define POOL_CENTRAL_INIT {{NULL, 0}}

static pool_global_t pool_global[POOL_COUNT] =
{
  {POOL_MIN << 0, POOL_MAX / (POOL_MIN << 0), POOL_CENTRAL_INIT},
  {POOL_MIN << 1, POOL_MAX / (POOL_MIN << 1), POOL_CENTRAL_INIT},
  {POOL_MIN << 2, POOL_MAX / (POOL_MIN << 2), POOL_CENTRAL_INIT},
  {POOL_MIN << 3, POOL_MAX / (POOL_MIN << 3), POOL_CENTRAL_INIT},
  {POOL_MIN << 4, POOL_MAX / (POOL_MIN << 4), POOL_CENTRAL_INIT},
  {POOL_MIN << 5, POOL_MAX / (POOL_MIN << 5), POOL_CENTRAL_INIT},
  {POOL_MIN << 6, POOL_MAX / (POOL_MIN << 6), POOL_CENTRAL_INIT},
  {POOL_MIN << 7, POOL_MAX / (POOL_MIN << 7), POOL_CENTRAL_INIT},
  {POOL_MIN << 8, POOL_MAX / (POOL_MIN << 8), POOL_CENTRAL_INIT},
  {POOL_MIN << 9, POOL_MAX / (POOL_MIN << 9), POOL_CENTRAL_INIT},
  {POOL_MIN << 10, POOL_MAX / (POOL_MIN << 10), POOL_CENTRAL_INIT},
  {POOL_MIN << 11, POOL_MAX / (POOL_MIN << 11), POOL_CENTRAL_INIT},
  {POOL_MIN << 12, POOL_MAX / (POOL_MIN << 12), POOL_CENTRAL_INIT},
  {POOL_MIN << 13, POOL_MAX / (POOL_MIN << 13), POOL_CENTRAL_INIT},
  {POOL_MIN << 14, POOL_MAX / (POOL_MIN << 14), POOL_CENTRAL_INIT},
  {POOL_MIN << 15, POOL_MAX / (POOL_MIN << 15), POOL_CENTRAL_INIT},
};

static pool_block_t pool_block_global;
static PONY_ATOMIC(size_t) in_pool_block_global;

static __pony_thread_local pool_local_t pool_local[POOL_COUNT];
static __pony_thread_local pool_block_header_t pool_block_header;

#ifdef USE_POOLTRACK
#include "../ds/stack.h"
#include "../sched/cpu.h"

#define POOL_TRACK_FREE ((void*)0)
#define POOL_TRACK_ALLOC ((void*)1)
#define POOL_TRACK_PUSH ((void*)2)
#define POOL_TRACK_PULL ((void*)3)
#define POOL_TRACK_PUSH_LIST ((void*)4)
#define POOL_TRACK_PULL_LIST ((void*)5)
#define POOL_TRACK_MAX_THREADS 64

DECLARE_STACK(pool_track, pool_track_t, void);
DEFINE_STACK(pool_track, pool_track_t, void);

typedef struct
{
  bool init;
  bool internal;
  int thread_id;
  pool_track_t* stack;
} pool_track_info_t;

static __pony_thread_local pool_track_info_t track;
static PONY_ATOMIC(int) track_global_thread_id;
static pool_track_info_t* track_global_info[POOL_TRACK_MAX_THREADS];

static void pool_event_print(int thread, void* op, size_t event, size_t tsc,
  void* addr, size_t size)
{
  if(op == POOL_TRACK_ALLOC)
    printf("%d ALLOC "__zu" ("__zu"): %p, "__zu"\n",
      thread, event, tsc, addr, size);
  else if(op == POOL_TRACK_FREE)
    printf("%d FREE "__zu" ("__zu"): %p, "__zu"\n",
      thread, event, tsc, addr, size);
  else if(op == POOL_TRACK_PUSH)
    printf("%d PUSH "__zu" ("__zu"): %p, "__zu"\n",
      thread, event, tsc, addr, size);
  else if(op == POOL_TRACK_PULL)
    printf("%d PULL "__zu" ("__zu"): %p, "__zu"\n",
      thread, event, tsc, addr, size);
  else if(op == POOL_TRACK_PUSH_LIST)
    printf("%d PUSH LIST "__zu" ("__zu"): "__zu", "__zu"\n",
      thread, event, tsc, (size_t)addr, size);
  else if(op == POOL_TRACK_PULL_LIST)
    printf("%d PULL LIST "__zu" ("__zu"): "__zu", "__zu"\n",
      thread, event, tsc, (size_t)addr, size);
}

static void pool_track(int thread_filter, void* addr_filter, size_t op_filter,
  size_t event_filter)
{
  for(int i = 0; i < POOL_TRACK_MAX_THREADS; i++)
  {
    if((thread_filter != -1) && (thread_filter != i))
      continue;

    pool_track_info_t* track = track_global_info[i];

    if(track == NULL)
      continue;

    Stack* t = (Stack*)track->stack;
    size_t event = 0;

    int state = 0;
    void* op;
    void* addr;
    size_t size;
    size_t tsc;

    while(t != NULL)
    {
      for(int j = t->index - 1; j >= 0; j--)
      {
        switch(state)
        {
          case 0:
            tsc = (size_t)t->data[j];
            state = 1;
            break;

          case 1:
            size = (size_t)t->data[j];
            state = 2;
            break;

          case 2:
            addr = t->data[j];
            state = 3;
            break;

          case 3:
          {
            bool print = true;
            op = t->data[j];
            state = 0;

            if((op_filter != (size_t)-1) && (op_filter != (size_t)op))
              print = false;

            if((event_filter != (size_t)-1) && (event_filter != event))
              print = false;

            if((addr_filter != NULL) &&
              ((addr > addr_filter) || ((addr + size) <= addr_filter)))
            {
              print = false;
            }

            if(print)
            {
              pool_event_print(i, op, event, tsc, addr, size);

              if(event_filter != (size_t)-1)
                return;
            }

            event++;
            break;
          }

          default: {}
        }
      }

      t = t->prev;
    }
  }
}

static void track_init()
{
  if(track.init)
    return;

  track.init = true;
  track.thread_id = atomic_fetch_add_explicit(&track_global_thread_id, 1,
    memory_order_seq_cst);
  track_global_info[track.thread_id] = &track;

  // Force the symbol to be linked.
  pool_track(track.thread_id, NULL, -1, 0);
}

static void track_alloc(void* p, size_t size)
{
  track_init();

  if(track.internal)
    return;

  track.internal = true;

  track.stack = pool_track_push(track.stack, POOL_TRACK_ALLOC);
  track.stack = pool_track_push(track.stack, p);
  track.stack = pool_track_push(track.stack, (void*)size);
  track.stack = pool_track_push(track.stack, (void*)ponyint_cpu_tick());

  track.internal = false;
}

static void track_free(void* p, size_t size)
{
  track_init();
  pony_assert(!track.internal);

  track.internal = true;

  track.stack = pool_track_push(track.stack, POOL_TRACK_FREE);
  track.stack = pool_track_push(track.stack, p);
  track.stack = pool_track_push(track.stack, (void*)size);
  track.stack = pool_track_push(track.stack, (void*)ponyint_cpu_tick());

  track.internal = false;
}

static void track_push(pool_item_t* p, size_t length, size_t size)
{
  track_init();
  pony_assert(!track.internal);

  track.internal = true;
  uint64_t tsc = ponyint_cpu_tick();

  track.stack = pool_track_push(track.stack, POOL_TRACK_PUSH_LIST);
  track.stack = pool_track_push(track.stack, (void*)length);
  track.stack = pool_track_push(track.stack, (void*)size);
  track.stack = pool_track_push(track.stack, (void*)tsc);

  while(p != NULL)
  {
    track.stack = pool_track_push(track.stack, POOL_TRACK_PUSH);
    track.stack = pool_track_push(track.stack, p);
    track.stack = pool_track_push(track.stack, (void*)size);
    track.stack = pool_track_push(track.stack, (void*)tsc);
    p = p->next;
  }

  track.internal = false;
}

static void track_pull(pool_item_t* p, size_t length, size_t size)
{
  track_init();
  pony_assert(!track.internal);

  track.internal = true;
  uint64_t tsc = ponyint_cpu_tick();

  track.stack = pool_track_push(track.stack, POOL_TRACK_PULL_LIST);
  track.stack = pool_track_push(track.stack, (void*)length);
  track.stack = pool_track_push(track.stack, (void*)size);
  track.stack = pool_track_push(track.stack, (void*)tsc);

  while(p != NULL)
  {
    track.stack = pool_track_push(track.stack, POOL_TRACK_PULL);
    track.stack = pool_track_push(track.stack, p);
    track.stack = pool_track_push(track.stack, (void*)size);
    track.stack = pool_track_push(track.stack, (void*)tsc);
    p = p->next;
  }

  track.internal = false;
}

#define TRACK_ALLOC(PTR, SIZE) track_alloc(PTR, SIZE)
#define TRACK_FREE(PTR, SIZE) track_free(PTR, SIZE)
#define TRACK_PUSH(PTR, LEN, SIZE) track_push(PTR, LEN, SIZE)
#define TRACK_PULL(PTR, LEN, SIZE) track_pull(PTR, LEN, SIZE)
#define TRACK_EXTERNAL() (!track.internal)

#else

#define TRACK_ALLOC(PTR, SIZE)
#define TRACK_FREE(PTR, SIZE)
#define TRACK_PUSH(PTR, LEN, SIZE)
#define TRACK_PULL(PTR, LEN, SIZE)
#define TRACK_EXTERNAL() (true)

#endif

static void pool_block_remove(pool_block_t* block)
{
  pool_block_t* prev = block->prev;
  pool_block_t* next = block->next;

  if(prev != NULL)
    prev->next = next;
  else
    pool_block_header.head = next;

  if(next != NULL)
    next->prev = prev;
}

static void pool_block_insert(pool_block_t* block)
{
  pool_block_t* next = pool_block_header.head;
  pool_block_t* prev = NULL;

  while(next != NULL)
  {
    if(block->size <= next->size)
      break;

    prev = next;
    next = next->next;
  }

  block->prev = prev;
  block->next = next;

  if(prev != NULL)
    prev->next = block;
  else
    pool_block_header.head = block;

  if(next != NULL)
    next->prev = block;
}

static void pool_block_push(pool_block_t* block)
{
  atomic_store_explicit(&block->acquired, false, memory_order_relaxed);
  atomic_fetch_add_explicit(&in_pool_block_global, 1, memory_order_acquire);
#ifdef USE_VALGRIND
  ANNOTATE_HAPPENS_AFTER(&in_pool_block_global);
#endif

  while(true)
  {
    pool_block_t* pos = atomic_load_explicit(&pool_block_global.global,
      memory_order_acquire);
#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_AFTER(&pool_block_global.global);
#endif

    // Find an insertion position. The list is sorted and stays sorted after an
    // insertion.
    pool_block_t* prev = &pool_block_global;
    while((pos != NULL) && (block->size > pos->size))
    {
      prev = pos;
      pos = atomic_load_explicit(&pos->global, memory_order_acquire);
#ifdef USE_VALGRIND
      ANNOTATE_HAPPENS_AFTER(&pos->global);
#endif
    }

    if(atomic_exchange_explicit(&prev->acquired, true, memory_order_acquire))
      continue;

#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_AFTER(&prev->acquired);
#endif

    pool_block_t* check_pos = atomic_load_explicit(&prev->global,
      memory_order_relaxed);

    if(pos != check_pos)
    {
      atomic_store_explicit(&prev->acquired, false, memory_order_relaxed);
      continue;
    }

    atomic_store_explicit(&block->global, pos, memory_order_relaxed);
#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_BEFORE(&prev->global);
#endif
    atomic_store_explicit(&prev->global, block, memory_order_release);
#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_BEFORE(&prev->acquired);
#endif
    atomic_store_explicit(&prev->acquired, false, memory_order_release);

    break;
  }

#ifdef USE_VALGRIND
  ANNOTATE_HAPPENS_BEFORE(&in_pool_block_global);
#endif
  atomic_fetch_sub_explicit(&in_pool_block_global, 1, memory_order_release);
}

static pool_block_t* pool_block_pull( size_t size)
{
  pool_block_t* block = atomic_load_explicit(&pool_block_global.global,
    memory_order_relaxed);

  if(block == NULL)
    return NULL;

  atomic_fetch_add_explicit(&in_pool_block_global, 1, memory_order_acquire);
#ifdef USE_VALGRIND
  ANNOTATE_HAPPENS_AFTER(&in_pool_block_global);
#endif

  while(true)
  {
    block = atomic_load_explicit(&pool_block_global.global,
      memory_order_relaxed);

    if(block == NULL)
    {
      atomic_fetch_sub_explicit(&in_pool_block_global, 1, memory_order_relaxed);
      return NULL;
    }

    atomic_thread_fence(memory_order_acquire);
#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_AFTER(&pool_block_global.global);
#endif

    pool_block_t* prev = &pool_block_global;

    // Find a big enough block. The list is sorted.
    while((block != NULL) && (size > block->size))
    {
      prev = block;
      block = atomic_load_explicit(&block->global, memory_order_acquire);
#ifdef USE_VALGRIND
      ANNOTATE_HAPPENS_AFTER(&block->global);
#endif
    }

    // No suitable block.
    if(block == NULL)
    {
      atomic_fetch_sub_explicit(&in_pool_block_global, 1, memory_order_relaxed);
      return NULL;
    }

    if(atomic_exchange_explicit(&prev->acquired, true, memory_order_acquire))
      continue;

#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_AFTER(&prev->acquired);
#endif

    pool_block_t* check_block = atomic_load_explicit(&prev->global,
      memory_order_relaxed);

    if((block != check_block) ||
      atomic_exchange_explicit(&block->acquired, true, memory_order_relaxed))
    {
      atomic_store_explicit(&prev->acquired, false, memory_order_relaxed);
      continue;
    }

    atomic_thread_fence(memory_order_acquire);
#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_AFTER(&block->acquired);
#endif

    pool_block_t* next = atomic_load_explicit(&block->global,
      memory_order_relaxed);
    atomic_store_explicit(&prev->global, next, memory_order_relaxed);

    // Don't release block.
#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_BEFORE(&prev->acquired);
#endif
    atomic_store_explicit(&prev->acquired, false, memory_order_release);

    break;
  }

#ifdef USE_VALGRIND
  ANNOTATE_HAPPENS_BEFORE(&in_pool_block_global);
#endif
  atomic_fetch_sub_explicit(&in_pool_block_global, 1, memory_order_release);

  // We can't modify block until we're sure no other thread will try to read
  // from it (e.g. to check if it can be popped from the global list). To do
  // this, we wait until nobody is trying to either push or pull.
  while(atomic_load_explicit(&in_pool_block_global, memory_order_relaxed) != 0)
    ponyint_cpu_relax();

  atomic_thread_fence(memory_order_acquire);
#ifdef USE_VALGRIND
  ANNOTATE_HAPPENS_AFTER(&in_pool_block_global);
#endif

  pony_assert(size <= block->size);

#ifdef USE_VALGRIND
  ANNOTATE_HAPPENS_BEFORE_FORGET_ALL(&block->global);
  ANNOTATE_HAPPENS_BEFORE_FORGET_ALL(&block->acquired);
#endif

  return block;
}

static void* pool_block_get(size_t size)
{
  if(pool_block_header.largest_size >= size)
  {
    pool_block_t* block = pool_block_header.head;

    while(block != NULL)
    {
      if(block->size > size)
      {
        // Use size bytes from the end of the block. This allows us to keep the
        // block info inside the block instead of using another data structure.
        size_t rem = block->size - size;
        block->size = rem;
        pool_block_header.total_size -= size;

        if((block->prev != NULL) && (block->prev->size > block->size))
        {
          // If we are now smaller than the previous block, move us forward in
          // the list.
          if(block->next == NULL)
            pool_block_header.largest_size = block->prev->size;

          pool_block_remove(block);
          pool_block_insert(block);
        } else if(block->next == NULL) {
          pool_block_header.largest_size = rem;
        }

        return (char*)block + rem;
      } else if(block->size == size) {
        if(block->next == NULL)
        {
          pool_block_header.largest_size =
            (block->prev == NULL) ? 0 : block->prev->size;
        }

        // Remove the block from the list.
        pool_block_remove(block);

        // Return the block pointer itself.
        pool_block_header.total_size -= size;
        return block;
      }

      block = block->next;
    }

    // If we didn't find any suitable block, something has gone really wrong.
    pony_assert(false);
  }

  pool_block_t* block = pool_block_pull(size);

  if(block == NULL)
    return NULL;

  if(size == block->size)
    return block;

  size_t rem = block->size - size;
  block->size = rem;
  pool_block_insert(block);
  pool_block_header.total_size += rem;

  if(pool_block_header.largest_size < rem)
    pool_block_header.largest_size = rem;

  return (char*)block + rem;
}

static void* pool_alloc_pages(size_t size)
{
  void* p = pool_block_get(size);

  if(p != NULL)
    return p;

  // We have no free blocks big enough.
  if(size >= POOL_MMAP)
    return ponyint_virt_alloc(size);

  pool_block_t* block = (pool_block_t*)ponyint_virt_alloc(POOL_MMAP);
  size_t rem = POOL_MMAP - size;

  block->size = rem;
  block->next = NULL;
  block->prev = NULL;
  pool_block_insert(block);
  pool_block_header.total_size += rem;
  if(pool_block_header.largest_size < rem)
    pool_block_header.largest_size = rem;

  return (char*)block + rem;
}

static void pool_free_pages(void* p, size_t size)
{
  if(pool_block_header.total_size >= POOL_MMAP)
  {
    // TODO: ???
  }

  pool_block_t* block = (pool_block_t*)p;
  block->prev = NULL;
  block->next = NULL;
  block->size = size;

  pool_block_insert(block);
  pool_block_header.total_size += size;
  if(pool_block_header.largest_size < size)
    pool_block_header.largest_size = size;
}

static void pool_push(pool_local_t* thread, pool_global_t* global)
{
  pool_central_t* p = (pool_central_t*)thread->pool;
  p->length = thread->length;

  thread->pool = NULL;
  thread->length = 0;

  pony_assert((p->length > 0) && (p->length <= global->count));
  TRACK_PUSH((pool_item_t*)p, p->length, global->size);

  pool_central_t* top;
  PONY_ABA_PROTECTED_PTR(pool_central_t) cmp;
  PONY_ABA_PROTECTED_PTR(pool_central_t) xchg;
  cmp.object = global->central.object;
  cmp.counter = global->central.counter;

  xchg.object = p;

  do
  {
    top = cmp.object;
    xchg.counter = cmp.counter + 1;
    p->central = top;

#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_BEFORE(&global->central);
#endif
  }
  while(!bigatomic_compare_exchange_strong_explicit(&global->central, &cmp,
    xchg, memory_order_acq_rel, memory_order_acquire));
}

static pool_item_t* pool_pull(pool_local_t* thread, pool_global_t* global)
{
  pool_central_t* top;
  PONY_ABA_PROTECTED_PTR(pool_central_t) cmp;
  PONY_ABA_PROTECTED_PTR(pool_central_t) xchg;
  cmp.object = global->central.object;
  cmp.counter = global->central.counter;
  pool_central_t* next;

  do
  {
    top = cmp.object;
    if(top == NULL)
      return NULL;

    atomic_thread_fence(memory_order_acquire);
#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_AFTER(&global->central);
#endif
    next = top->central;
    xchg.object = next;
    xchg.counter = cmp.counter + 1;
  }
  while(!bigatomic_compare_exchange_strong_explicit(&global->central, &cmp,
    xchg, memory_order_acq_rel, memory_order_acquire));

  // We need to synchronise twice on global->central to make sure we see every
  // previous write to the memory we're going to use before we use it.
#ifdef USE_VALGRIND
  ANNOTATE_HAPPENS_AFTER(&global->central);
#endif

  pony_assert((top->length > 0) && (top->length <= global->count));
  TRACK_PULL((pool_item_t*)top, top->length, global->size);

  thread->pool = top->next;
  thread->length = top->length - 1;

  return (pool_item_t*)top;
}

static void* pool_get(pool_local_t* pool, size_t index)
{
  // Try per-size thread-local free list first.
  pool_local_t* thread = &pool[index];
  pool_global_t* global = &pool_global[index];
  pool_item_t* p = thread->pool;

  if(p != NULL)
  {
    thread->pool = p->next;
    thread->length--;
    return p;
  }

  if(TRACK_EXTERNAL())
  {
    // Try to get a new free list from the per-size global list of free lists.
    p = pool_pull(thread, global);

    if(p != NULL)
      return p;
  }

  if(global->size < POOL_ALIGN)
  {
    // Check our per-size thread-local free block.
    if(thread->start < thread->end)
    {
      void* p = thread->start;
      thread->start += global->size;
      return p;
    }

    // Use the pool allocator to get a block POOL_ALIGN bytes in size
    // and treat it as a free block.
    char* mem = (char*)pool_get(pool, POOL_ALIGN_INDEX);
    thread->start = mem + global->size;
    thread->end = mem + POOL_ALIGN;
    return mem;
  }

  // Pull size bytes from the list of free blocks. Don't use a size-specific
  // free block.
  return pool_alloc_pages(global->size);
}

void* ponyint_pool_alloc(size_t index)
{
#ifdef USE_VALGRIND
  VALGRIND_DISABLE_ERROR_REPORTING;
#endif

  pool_local_t* pool = pool_local;
  void* p = pool_get(pool, index);
  TRACK_ALLOC(p, POOL_MIN << index);

#ifdef USE_VALGRIND
  VALGRIND_ENABLE_ERROR_REPORTING;
  VALGRIND_HG_CLEAN_MEMORY(p, POOL_SIZE(index));
  VALGRIND_MALLOCLIKE_BLOCK(p, POOL_SIZE(index), 0, 0);
#endif

  return p;
}

void ponyint_pool_free(size_t index, void* p)
{
#ifdef USE_VALGRIND
  VALGRIND_HG_CLEAN_MEMORY(p, POOL_SIZE(index));
  VALGRIND_DISABLE_ERROR_REPORTING;
#endif

  pony_assert(index < POOL_COUNT);
  TRACK_FREE(p, POOL_MIN << index);

  pool_local_t* thread = &pool_local[index];
  pool_global_t* global = &pool_global[index];

  if(thread->length == global->count)
    pool_push(thread, global);

  pony_assert(thread->length < global->count);
  pool_item_t* lp = (pool_item_t*)p;
  lp->next = thread->pool;
  thread->pool = lp;
  thread->length++;

#ifdef USE_VALGRIND
  VALGRIND_ENABLE_ERROR_REPORTING;
  VALGRIND_FREELIKE_BLOCK(p, 0);
#endif
}

static void* pool_alloc_size(size_t size)
{
#ifdef USE_VALGRIND
  VALGRIND_DISABLE_ERROR_REPORTING;
#endif

  void* p = pool_alloc_pages(size);
  TRACK_ALLOC(p, size);

#ifdef USE_VALGRIND
  VALGRIND_ENABLE_ERROR_REPORTING;
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

  return p;
}

static void pool_free_size(size_t size, void* p)
{
#ifdef USE_VALGRIND
  VALGRIND_HG_CLEAN_MEMORY(p, size);
  VALGRIND_DISABLE_ERROR_REPORTING;
#endif

  TRACK_FREE(p, size);
  pool_free_pages(p, size);

#ifdef USE_VALGRIND
  VALGRIND_ENABLE_ERROR_REPORTING;
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

  if(p == NULL)
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
  for(size_t index = 0; index < POOL_COUNT; index++)
  {
    pool_local_t* thread = &pool_local[index];
    pool_global_t* global = &pool_global[index];

    while(thread->start < thread->end)
    {
      if(thread->length == global->count)
        pool_push(thread, global);

      pool_item_t* item = (pool_item_t*)thread->start;
      thread->start += global->size;
      item->next = thread->pool;
      thread->pool = item;
      thread->length++;
    }

    if(thread->length > 0)
      pool_push(thread, global);
  }

  pool_block_t* block = pool_block_header.head;
  while(block != NULL)
  {
    pool_block_t* next = block->next;
    pool_block_remove(block);
    pool_block_push(block);
    block = next;
  }

  pool_block_header.total_size = 0;
  pool_block_header.largest_size = 0;
}

#endif

size_t ponyint_pool_index(size_t size)
{
#ifdef PLATFORM_IS_ILP32
#define BITS (32 - POOL_MIN_BITS)
#else
#define BITS (64 - POOL_MIN_BITS)
#endif

  // The condition is in that order for better branch prediction: non-zero pool
  // indices are more likely than zero pool indices.
  if(size > POOL_MIN)
    return (size_t)BITS - __pony_clzzu(size) - (!(size & (size - 1)));

  return 0;
}

size_t ponyint_pool_used_size(size_t size)
{
  size_t index = ponyint_pool_index(size);

  if(index < POOL_COUNT)
    return POOL_SIZE(index);

  return ponyint_pool_adjust_size(size);
}

size_t ponyint_pool_adjust_size(size_t size)
{
  if((size & POOL_ALIGN_MASK) != 0)
    size = (size & ~POOL_ALIGN_MASK) + POOL_ALIGN;

  // we've overflowed the `size_t` datatype
  if(size == 0)
    size = size - 1;

  return size;
}
