#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>

#define LOG_MODULE "queue"

#include "log.h"
#include "debug.h"
#include "linkedlist.h"


/**
 * Represents a queue object.
 */
struct avbox_queue
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int destroyed;
	unsigned int waiters;
	LIST items;
};


/**
 * Queue item.
 */
LISTABLE_STRUCT(avbox_queue_node,
	void *value;
);


/**
 * Places an item in the queue. Does not return until either
 * there is data on the queue or the queue is destroyed.
 */
void *
avbox_queue_get(struct avbox_queue *inst)
{
	void *ret = NULL;
	struct avbox_queue_node *node = NULL;
	assert(inst != NULL);

	pthread_mutex_lock(&inst->lock);
	inst->waiters++;

#ifndef NDEBUG
	if (inst->destroyed) {
		goto end;
	}
#endif

	/* get the next item on the queue. if the queue is empty
	 * go to sleep */
	while ((node = LIST_TAIL(struct avbox_queue_node*, &inst->items)) == NULL) {
		pthread_cond_wait(&inst->cond, &inst->lock);
		if (inst->destroyed) {
			goto end;
		}
	}

	/* dequeue the item and return it */
	assert(node != NULL);
	LIST_REMOVE(node);
	ret = node->value;
end:
	inst->waiters--;
	pthread_mutex_unlock(&inst->lock);
	if (node != NULL) {
		free(node);
	}
	return ret;
}


/**
 * Puts an item in the queue.
 */
int
avbox_queue_put(struct avbox_queue *inst, void *item)
{
	struct avbox_queue_node *node;
	if ((node = malloc(sizeof(struct avbox_queue_node))) == NULL) {
		LOG_VPRINT_ERROR("Could not allocate node: %s",
			strerror(errno));
		return -1;
	}

	/* add item to queue and signal any threads waiting
	 * for it */
	pthread_mutex_lock(&inst->lock);
#ifndef NDEBUG
	if (inst->destroyed) {
		LOG_PRINT_ERROR("Added node to destroyed queue! Aborting.");
		abort();
	}
#endif
	LIST_ADD(&inst->items, item);
	pthread_cond_signal(&inst->cond);
	pthread_mutex_unlock(&inst->lock);
	return 0;
}


/**
 * Creates a new queue object.
 */
struct avbox_queue *
avbox_queue_new()
{
	struct avbox_queue *inst;

	/* allocate memory for queue */
	if ((inst = malloc(sizeof(struct avbox_queue))) == NULL) {
		LOG_VPRINT_ERROR("Could not create queue: %s",
			strerror(errno));
		return NULL;
	}

	if (pthread_mutex_init(&inst->lock, NULL) != 0) {
		LOG_VPRINT_ERROR("Could not initialize queue mutex: %s!",
			strerror(errno));
		return NULL;
	}

	LIST_INIT(&inst->items);
	inst->destroyed = 0;
	inst->waiters = 0;
	return inst;
}


/**
 * Destroys a queue object.
 */
void
avbox_queue_destroy(struct avbox_queue *inst)
{
	struct avbox_queue_node *node;

	/* wake any threads waiting on queue */
	pthread_mutex_lock(&inst->lock);
	inst->destroyed = 1;
	while (inst->waiters > 0) {
		pthread_cond_broadcast(&inst->cond);
		pthread_mutex_unlock(&inst->lock);
		pthread_mutex_lock(&inst->lock);
	}

	/* if the queue still has any items in it
	 * print a warning */
	if (LIST_SIZE(&inst->items) > 0) {
		LOG_VPRINT_ERROR("LEAK!: Destroying queue with %d items!",
			LIST_SIZE(&inst->items));
	}

	/* free all nodes */
	LIST_FOREACH_SAFE(struct avbox_queue_node*, node, &inst->items, {
		LIST_REMOVE(node);
		free(node);
	});

	pthread_mutex_unlock(&inst->lock);

	/* free queue */
	free(inst);
}
