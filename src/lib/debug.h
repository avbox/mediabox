/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef __MB_DEBUG_H__
#define __MB_DEBUG_H__


#include <stdio.h>
#include "log.h"
#include "compiler.h"


/**
 * MB_DEBUG_SET_THREAD_NAME() -- Set the thread name as seen in debugger.
 */
#if defined(_GNU_SOURCE)
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
#define DEBUG_VABORT(module, fmt, ...) abort()
#endif

#define ABORT(str) \
do { \
	LOG_PRINT_ERROR(str); \
	abort(); \
} while (0)

#endif
