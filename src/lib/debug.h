/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_DEBUG_H__
#define __MB_DEBUG_H__


#include <stdio.h>
#include "log.h"
#include "compiler.h"


/**
 * MB_DEBUG_SET_THREAD_NAME() -- Set the thread name as seen in debugger.
 */
#if !defined(NDEBUG) && defined(_GNU_SOURCE)
#define DEBUG_SET_THREAD_NAME(name) pthread_setname_np(pthread_self(), name)
#else
#define DEBUG_SET_THREAD_NAME(name) (void) name
#endif


#if !defined(NDEBUG) && defined(_GNU_SOURCE)
#define MB_DEBUG_SET_THREAD_NAME(name) pthread_setname_np(pthread_self(), name)
#else
#define MB_DEBUG_SET_THREAD_NAME(name) (void) name
#endif

/**
 * DEBUG_VPRINT() - Variadic debug print macro.
 */
#ifndef NDEBUG
#define DEBUG_VPRINT(module, fmt, ...) log_printf(module ": " fmt "\n", __VA_ARGS__)
#else
#define DEBUG_VPRINT(module, fmt, ...) (void) 0
#endif

/**
 * DEBUG_PRINT() -- Debug print macro.
 */
#ifndef NDEBUG
#define DEBUG_PRINT(module, str) log_printf(module ": " str "\n");
#else
#define DEBUG_PRINT(module, str) (void) 0
#endif

/**
 * Identical to the standard library's assert() but writes
 * to the debug stream rather than stdout.
 */
#ifndef NDEBUG
#define ASSERT(expr) \
do { \
	if (UNLIKELY(!(expr))) { \
		DEBUG_VPRINT("ASSERT", "(%s) failed at %s:%i", #expr,  __FILE__, __LINE__ ); \
		log_backtrace(); \
		abort(); \
	} \
} while (0)
#else
#define ASSERT(expr) (void) 0
#endif


/**
 * Combindes DEBUG_PRINT() and assert()
 */
#ifndef NDEBUG
#define DEBUG_ASSERT(module, cond, fmt) \
do { \
	if (UNLIKELY(!(cond))) { \
		DEBUG_PRINT(module, fmt); \
		abort(); \
	} \
} while (0)
#else
#define DEBUG_ASSERT(module, cond, fmt)
#endif
#ifndef NDEBUG
#define DEBUG_VASSERT(module, cond, fmt, ...) \
do { \
	if (UNLIKELY(!(cond))) { \
		DEBUG_VPRINT(module, fmt, __VA_ARGS__); \
		abort(); \
	} \
} while (0)
#else
#define DEBUG_VASSERT(module, cond, fmt, ...)
#endif

/**
 * Aborts only on debug mode.
 */
#ifndef NDEBUG
#define DEBUG_ABORT(module, fmt) \
do { \
	DEBUG_PRINT(module, fmt); \
	abort(); \
} while (0)
#else
#define DEBUG_ABORT(module, fmt) (void)0
#endif

#ifndef NDEBUG
#define DEBUG_VABORT(module, fmt, ...) \
do { \
	DEBUG_VPRINT(module, fmt, __VA_ARGS__); \
	abort(); \
} while (0)
#else
#define DEBUG_VABORT(module, fmt, ...) (void) 0
#endif


#endif
