#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "ponyassert.h"
#include "alloc.h"
#include <platform.h>
#include <string.h>
#include <stdio.h>

#ifdef PLATFORM_IS_POSIX_BASED
#include <sys/mman.h>
#endif

#if defined(PLATFORM_IS_MACOSX)
#include <mach/vm_statistics.h>
#endif

void* ponyint_virt_alloc(size_t bytes)
{
  void* p;
  bool ok = true;

#if defined(PLATFORM_IS_WINDOWS)
  p = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(p == NULL)
    ok = false;
#elif defined(PLATFORM_IS_POSIX_BASED)
#if defined(PLATFORM_IS_LINUX) || defined(PLATFORM_IS_EMSCRIPTEN)
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#elif defined(PLATFORM_IS_MACOSX)
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANON, -1, 0);
#elif defined(PLATFORM_IS_DRAGONFLY)
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANON, -1, 0);
#elif defined(PLATFORM_IS_OPENBSD)
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANON, -1, 0);
#elif defined(PLATFORM_IS_BSD)
#ifndef MAP_ALIGNED_SUPER
#define MAP_ALIGNED_SUPER 0
#endif
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANON | MAP_ALIGNED_SUPER, -1, 0);
#endif
  if(p == MAP_FAILED)
    ok = false;
#endif

  if(!ok)
  {
    perror("out of memory: ");
    abort();
  }

  return p;
}

#ifdef POOL_USE_MESSAGE_PASSING
void* ponyint_virt_alloc_aligned(size_t size)
{
  // size must be a multiple of POOL_MMAP.
  pony_assert((size & (POOL_MMAP - 1)) == 0);

  // NOTE: This can likely be made more efficient depending on the system  to 
  // avoid the extra allocation/freeing to get alingment.

  // overallocate to ensure we can align.
  size_t bytes = size + POOL_MMAP;

  void* p;
  bool ok = true;

#if defined(PLATFORM_IS_WINDOWS)
  // This is not supported on Windows at the moment.
  pony_assert(false);
  p = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(p == NULL)
    ok = false;
#elif defined(PLATFORM_IS_POSIX_BASED)
#if defined(PLATFORM_IS_LINUX) || defined(PLATFORM_IS_EMSCRIPTEN)
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#elif defined(PLATFORM_IS_MACOSX)
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANON, -1, 0);
#elif defined(PLATFORM_IS_DRAGONFLY)
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANON, -1, 0);
#elif defined(PLATFORM_IS_OPENBSD)
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANON, -1, 0);
#elif defined(PLATFORM_IS_BSD)
#ifndef MAP_ALIGNED_SUPER
#define MAP_ALIGNED_SUPER 0
#endif
  p = mmap(0, bytes, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANON | MAP_ALIGNED_SUPER, -1, 0);
#endif
  if(p == MAP_FAILED)
    ok = false;

  void* oldp = p;

  // Align the pointer to first POOL_MMAP multiple in allocation.
  p = (void*)(((uintptr_t)p + POOL_MMAP) & ~(POOL_MMAP - 1));

  // Free the memory before the aligned pointer.
  ponyint_virt_free(oldp, ((uintptr_t)p) - ((uintptr_t)oldp));

  // Free the memory after the aligned pointer region end.
  void* endp = (void*)((uintptr_t)p + size);
  ponyint_virt_free(endp, ((uintptr_t)oldp + bytes) - ((uintptr_t)endp));

  // ensure that the pointer is correctly aligned and the size is correct.
  pony_assert((((uintptr_t)endp) - ((uintptr_t)p)) == size);
  pony_assert((((uintptr_t)p) & (POOL_MMAP - 1)) == 0);
#endif

  if(!ok)
  {
    perror("out of memory: ");
    abort();
  }

  return p;
}
#endif

void ponyint_virt_free(void* p, size_t bytes)
{
#if defined(PLATFORM_IS_WINDOWS)
  VirtualFree(p, 0, MEM_RELEASE);
#elif defined(PLATFORM_IS_POSIX_BASED)
  if (0 == bytes)
    return;

  int r = munmap(p, bytes);
  if(0 != r)
  {
    perror("unable to free memory: ");
    abort();
  }
#endif
}
