#ifndef __AVBOX_QUEUE_H__
#define __AVBOX_QUEUE_H__


/**
 * Represents a queue object.
 */
struct avbox_queue;


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
 * Creates a new queue object.
 */
struct avbox_queue *
avbox_queue_new();


/**
 * Destroys a queue object.
 */
void
avbox_queue_destroy(struct avbox_queue *inst);

#endif
