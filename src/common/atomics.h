#ifndef PLATFORM_ATOMICS_H
#define PLATFORM_ATOMICS_H

/** Intrinsics for atomic memory operations.
 *
 * This file provides cross-platform atomic instructions. It is based on the
 * FreeBSD stdatomic.h, the Clang stdatomic.h, and the GCC stdatomic.h. It
 * does not replicate the correct stdatomic.h interface because it doesn't
 * appear to be possible to do so when using MSVC on Windows.
 *
 * The Clang __c11_atomic_* operations are not used, because there are cases
 * (such as pagemap) where some operations on a type need to be atomic and
 * some non-atomic. Requiring an _Atomic(T) type prevents this.
 */

#if defined(PLATFORM_IS_CLANG_OR_GCC)
  #if defined(__clang__)
    #define PONY_CLANG (__clang_major__ * 100) + __clang_minor__
    #if defined(__apple_build_version__) && (PONY_CLANG >= 601)
      #define __GNUC_ATOMICS
    #elif !defined(__apple_build_version__) && (PONY_CLANG >= 306)
      #define __GNUC_ATOMICS
    #else
      #define __SYNC_ATOMICS
    #endif
  #elif __GNUC_PREREQ(4, 7)
    #define __GNUC_ATOMICS
  #elif defined(__GNUC__)
    #define __SYNC_ATOMICS
  #else
    #error "Please use clang >= 3.5 or gcc >= 4.7"
  #endif
#elif defined(PLATFORM_IS_VISUAL_STUDIO)
  #define __MSVC_ATOMICS
#endif

#ifdef __CLANG_ATOMICS

#define _atomic_load(PTR, ORDER) \
  __c11_atomic_load(PTR, ORDER)

#define _atomic_store(PTR, VAL, ORDER) \
  __c11_atomic_store(PTR, VAL, ORDER)

#define _atomic_exchange(PTR, VAL, ORDER) \
  __c11_atomic_exchange(PTR, VAL, ORDER)

#define _atomic_cas(PTR, EXPP, VAL, SUCC, FAIL) \
  __c11_atomic_compare_exchange_strong(PTR, EXPP, VAL, SUCC, FAIL)

#define _atomic_dwcas(PTR, EXPP, VAL, SUCC, FAIL) \
  __c11_atomic_compare_exchange_strong(PTR, EXPP, VAL, SUCC, FAIL)

#define _atomic_add(PTR, VAL, ORDER) \
  (__c11_atomic_fetch_add(PTR, VAL, ORDER))

#endif

#ifdef __GNUC_ATOMICS

#define _atomic_load(PTR, ORDER) \
  __atomic_load_n(PTR, ORDER)

#define _atomic_store(PTR, VAL, ORDER) \
  __atomic_store_n(PTR, VAL, ORDER)

#define _atomic_exchange(PTR, VAL, ORDER) \
  __atomic_exchange_n(PTR, VAL, ORDER)

#define _atomic_cas(PTR, EXPP, VAL, SUCC, FAIL) \
  __atomic_compare_exchange_n(PTR, EXPP, VAL, 0, SUCC, FAIL)

#define _atomic_dwcas(PTR, EXPP, VAL, SUCC, FAIL) \
  __atomic_compare_exchange_n(PTR, EXPP, VAL, 0, SUCC, FAIL)

#define _atomic_add(PTR, VAL, ORDER) \
  (__atomic_fetch_add(PTR, VAL, ORDER))

#endif

#ifdef __SYNC_ATOMICS

#define _atomic_load(PTR, ORDER) \
  (*(PTR))

#define _atomic_store(PTR, VAL, ORDER) \
  (*(PTR) = VAL)

#define _atomic_exchange(PTR, VAL, ORDER) \
  __sync_lock_test_and_set(PTR, VAL);

#define _atomic_cas(PTR, EXPP, VAL, SUCC, FAIL) \
  (*(EXPP) == \
    (*(EXPP) = __sync_val_compare_and_swap(PTR, *(EXPP), VAL)))

#define _atomic_dwcas(PTR, EXPP, VAL, SUCC, FAIL) \
  (*(EXPP) == \
    (*(EXPP) = __sync_val_compare_and_swap(PTR, *(EXPP), VAL)))

#define _atomic_add(PTR, VAL, ORDER) \
  (__sync_fetch_and_add(PTR, VAL))

#endif

#ifdef __MSVC_ATOMICS

#pragma intrinsic(_InterlockedExchangePointer)
#pragma intrinsic(_InterlockedCompareExchangePointer)
#pragma intrinsic(_InterlockedCompareExchange128)

#define _atomic_load(PTR, ORDER) \
  (*(PTR))

#define _atomic_store(PTR, VAL, ORDER) \
  (*(PTR) = VAL)

#define _atomic_exchange(PTR, VAL, ORDER) \
  (_InterlockedExchangePointer((PVOID volatile*)PTR, VAL))

#define _atomic_cas(PTR, EXPP, VAL, SUCC, FAIL) \
  (*(EXPP) == \
    (*((PVOID*)(EXPP)) = \
      _InterlockedCompareExchangePointer( \
        (PVOID volatile*)PTR, VAL, *(EXPP))))

#define _atomic_dwcas(PTR, EXPP, VAL, SUCC, FAIL) \
  (_InterlockedCompareExchange128( \
    (LONGLONG volatile*)PTR, VAL.high, VAL.low, (LONGLONG*)EXPP))

#define _atomic_add(PTR, VAL, ORDER) \
  (InterlockedAdd64((LONGLONG volatile*)PTR, VAL) - VAL)

#endif

#endif
