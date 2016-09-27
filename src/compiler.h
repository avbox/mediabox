#ifndef __MB_COMPILER__
#define __MB_COMPILER__

/* Macros for optimizing likely branches */
#define LIKELY(x)               (__builtin_expect(!!(x), 1))
#define UNLIKELY(x)             (__builtin_expect(!!(x), 0))


#define ATOMIC_INC(addr) (__sync_fetch_and_add(addr, 1))
#define ATOMIC_DEC(addr) (__sync_fetch_and_sub(addr, 1))

#endif
