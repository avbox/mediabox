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
