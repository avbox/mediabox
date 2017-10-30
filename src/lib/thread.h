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


#ifndef __AVBOX_THREAD_H__
#define __AVBOX_THREAD_H__
#include "delegate.h"


/**
 * Delegate a function call to a thread.
 */
struct avbox_delegate*
avbox_thread_delegate(avbox_delegate_fn func, void * arg);


/**
 * Initialize the thread pool.
 */
int
avbox_thread_init(void);


/**
 * Shutdown the thread pool.
 */
void
avbox_thread_shutdown(void);


#endif
