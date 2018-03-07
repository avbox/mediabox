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


#ifndef __AVBOX_QUEUE_H__
#define __AVBOX_QUEUE_H__


/**
 * Represents a queue object.
 */
struct avbox_queue;


/**
 * Wake all threads waiting on queue.
 */
void
avbox_queue_wake(struct avbox_queue * inst);


/**
 * Wait for any IO events on the queue
 */
void
avbox_queue_wait(struct avbox_queue * const inst);


/**
 * Locks the queue.
 */
void
avbox_queue_lock(struct avbox_queue * inst);


/**
 * Unlocks the queue.
 */
void
avbox_queue_unlock(struct avbox_queue * inst);


/**
 * Gets the count of items in the queue.
 */
size_t
avbox_queue_count(struct avbox_queue * const inst);


/**
 * Returns the next item in the queue without dequeueing it.
 */
void *
avbox_queue_peek(struct avbox_queue * const inst, const int block);


/**
 * Peeks and block only for timeout micro seconds.
 */
void *
avbox_queue_timedpeek(struct avbox_queue * const inst, const int64_t timeout);


/**
 * Sets the queue size limit. NOTE: If the queue is currently
 * above the newly set limit all calls to avbox_queue_put() will
 * continue to block until the size is reduced bellow the new
 * size */
void
avbox_queue_setsize(struct avbox_queue * const inst, size_t sz);


/**
 * Places an item in the queue. Does not return until either
 * there is data on the queue or the queue is destroyed.
 */
void *
avbox_queue_get(struct avbox_queue *inst);


/**
 * Puts an item in the queue.
 */
int
avbox_queue_put(struct avbox_queue *inst, void *item);


/**
 * Check if the queue is closed.
 */
int
avbox_queue_isclosed(struct avbox_queue * const inst);


/**
 * Sets the queue name that is displayed in log warnings
 * and debug messages.
 */
int
avbox_queue_setname(struct avbox_queue * const inst, const char * const name);


/**
 * Creates a new queue object.
 */
struct avbox_queue *
avbox_queue_new(size_t sz);


/**
 * Close the queue. After the queue is closed all attempts to write
 * to it will return ESHUTDOWN. Any attempts to read from it will succeed
 * as long as there are items in the queue, otherwise they will fail
 * with ESHUTDOWN.
 */
void
avbox_queue_close(struct avbox_queue * inst);


/**
 * Destroys a queue object.
 */
void
avbox_queue_destroy(struct avbox_queue *inst);

#endif
