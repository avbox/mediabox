/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_COMPILER__
#define __MB_COMPILER__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Macros for optimizing likely branches */
#ifdef ENABLE_BRANCH_OPTIMIZATION
#define LIKELY(x)               (__builtin_expect(!!(x), 1))
#define UNLIKELY(x)             (__builtin_expect(!!(x), 0))
#else
#define LIKELY(x)		(x)
#define UNLIKELY(x)		(x)
#endif


#define ATOMIC_INC(addr) (__sync_fetch_and_add(addr, 1))
#define ATOMIC_DEC(addr) (__sync_fetch_and_sub(addr, 1))

#endif
