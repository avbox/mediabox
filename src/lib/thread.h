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
#include "dispatch.h"


#define AVBOX_THREAD_REALTIME	(0x01)


/**
 * Create a new thread.
 */
struct avbox_thread *
avbox_thread_new(avbox_message_handler handler, void * const context, int flags);


/**
 * Delegates a function call to a thread.
 */
struct avbox_delegate *
avbox_thread_delegate(struct avbox_thread * const thread,
	avbox_delegate_fn func, void * const arg);

/**
 * Get the underlying object.
 */
struct avbox_object *
avbox_thread_object(const struct avbox_thread * const thread);


/**
 * Destroy a thread.
 */
void
avbox_thread_destroy(struct avbox_thread * const thread);


/**
 * Delegate a function call to the work queue.
 */
struct avbox_delegate*
avbox_workqueue_delegate(avbox_delegate_fn func, void * arg);


/**
 * Initialize the thread pool.
 */
int
avbox_workqueue_init(void);


/**
 * Shutdown the thread pool.
 */
void
avbox_workqueue_shutdown(void);


#endif
