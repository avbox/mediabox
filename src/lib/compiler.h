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


#ifndef __MB_COMPILER__
#define __MB_COMPILER__

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include <unistd.h>
#include <sys/syscall.h>


/* Macros for optimizing likely branches */
/* NOTE: Be very careful enabling this. I think there are
 * broken if statements (not parenthesized correctly) and this
 * will break things. */
#if 1 || defined(ENABLE_BRANCH_OPTIMIZATION)
#define LIKELY(x)               (__builtin_expect(!!(x), 1))
#define UNLIKELY(x)             (__builtin_expect(!!(x), 0))
#else
#define LIKELY(x)		(x)
#define UNLIKELY(x)		(x)
#endif

#define avbox_gettid()		syscall(__NR_gettid)


#define ATOMIC_INC(addr) (__sync_fetch_and_add(addr, 1))
#define ATOMIC_DEC(addr) (__sync_fetch_and_sub(addr, 1))
#define MEMORY_BARRIER() __sync_synchronize()

/*
 * Access modifiers.
 *
 * STATIC - directly accessible only in file scope
 * INTERNAL - directly accessible to other objects at link-time but not exported
 * EXPORT - exported and can be overriden by LD_PRELOAD
 * PROTECTED - exported and cannot be overriden.
 */
#define STATIC		static
#define INTERNAL	__attribute__ ((visibility("hidden")))
#define PROTECTED	__attribute__ ((visibility("protected")))
#ifdef __cplusplus
#define EXPORT		extern "C" __attribute__ ((visibility("default")))
#else
#define EXPORT		__attribute__ ((visibility("default")))
#endif


#endif
