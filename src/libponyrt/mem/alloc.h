#ifndef mem_alloc_h
#define mem_alloc_h

#include "pool.h"

/**
 * Allocates memory in the virtual address space.
 */
void* ponyint_virt_alloc(size_t bytes);

#ifdef POOL_USE_MESSAGE_PASSING
/**
 * Allocates memory in the virtual address space aligned to POOL_MMAP.
 * All allocations are required to be a multiple of POOL_MMAP.
 */
void* ponyint_virt_alloc_aligned(size_t bytes);
#endif

/**
 * Deallocates a chunk of memory that was previously allocated with
 * ponyint_virt_alloc.
 */
void ponyint_virt_free(void* p, size_t bytes);

#endif
