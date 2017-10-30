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
