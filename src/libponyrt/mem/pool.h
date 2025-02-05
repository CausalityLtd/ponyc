#ifndef mem_pool_h
#define mem_pool_h

#include <stdbool.h>
#include <stddef.h>

#include <platform.h>

#if defined(USE_POOL_MEMALIGN)
#  define POOL_USE_MEMALIGN
#elif defined(USE_POOL_MESSAGE_PASSING)
#  define POOL_USE_MESSAGE_PASSING
#else
#  define POOL_USE_DEFAULT
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

PONY_EXTERN_C_BEGIN

/* Because of the way free memory is reused as its own linked list container,
 * the minimum allocation size is 32 bytes for the default pool implementation
 * and 16 bytes for the memalign and message passing pool implementations.
 */

#ifndef POOL_USE_DEFAULT
#define POOL_MIN_BITS 4
#else
#define POOL_MIN_BITS 5
#endif

#define POOL_MAX_BITS 20
#define POOL_ALIGN_BITS 10

#define POOL_MIN (1 << POOL_MIN_BITS)
#define POOL_MAX (1 << POOL_MAX_BITS)
#define POOL_ALIGN (1 << POOL_ALIGN_BITS)
#define POOL_COUNT (POOL_MAX_BITS - POOL_MIN_BITS + 1)

__pony_spec_malloc__(void* ponyint_pool_alloc(size_t index));
void ponyint_pool_free(size_t index, void* p);

__pony_spec_malloc__(void* ponyint_pool_alloc_size(size_t size));
void ponyint_pool_free_size(size_t size, void* p);

void* ponyint_pool_realloc_size(size_t old_size, size_t new_size, void* p);

void ponyint_pool_thread_cleanup();

size_t ponyint_pool_index(size_t size);

size_t ponyint_pool_used_size(size_t index);

size_t ponyint_pool_adjust_size(size_t size);

#ifdef POOL_USE_DEFAULT
#define POOL_INDEX(SIZE) \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 0)), 0, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 1)), 1, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 2)), 2, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 3)), 3, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 4)), 4, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 5)), 5, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 6)), 6, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 7)), 7, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 8)), 8, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 9)), 9, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 10)), 10, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 11)), 11, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 12)), 12, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 13)), 13, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 14)), 14, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 15)), 15, \
    EXPR_NONE \
    ))))))))))))))))
#else
#define POOL_INDEX(SIZE) \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 0)), 0, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 1)), 1, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 2)), 2, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 3)), 3, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 4)), 4, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 5)), 5, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 6)), 6, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 7)), 7, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 8)), 8, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 9)), 9, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 10)), 10, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 11)), 11, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 12)), 12, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 13)), 13, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 14)), 14, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 15)), 15, \
  __pony_choose_expr(SIZE <= (1 << (POOL_MIN_BITS + 16)), 16, \
    EXPR_NONE \
    )))))))))))))))))
#endif

#define POOL_ALLOC(TYPE) \
  (TYPE*) ponyint_pool_alloc(POOL_INDEX(sizeof(TYPE)))

#define POOL_FREE(TYPE, VALUE) \
  ponyint_pool_free(POOL_INDEX(sizeof(TYPE)), VALUE)

#define POOL_SIZE(INDEX) \
  ((size_t)1 << (POOL_MIN_BITS + INDEX))

#ifdef POOL_USE_MESSAGE_PASSING
typedef struct pool_remote_allocs_t pool_remote_allocs_t;
pool_remote_allocs_t* ponyint_initialize_pool_message_passing();
void ponyint_pool_gather_receive_remote_allocations(pool_remote_allocs_t* remote_allocs);
void ponyint_pool_receive_remote_allocations(pool_remote_allocs_t* remote_allocs);
#endif

#ifdef USE_RUNTIMESTATS
#define POOL_ALLOC_SIZE(TYPE) \
  POOL_SIZE(POOL_INDEX(sizeof(TYPE)))
#endif

PONY_EXTERN_C_END

#endif
