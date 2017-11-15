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


#ifdef HAVE_CONFIG_H
#	include "../config.h"
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
#include "time_util.h"


/**
 * Represents a queue object.
 */
struct avbox_queue
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int closed;
	size_t cnt;
	size_t sz;
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
 * Wake all threads waiting on queue.
 */
void
avbox_queue_wake(struct avbox_queue * inst)
{
	pthread_mutex_lock(&inst->lock);
	pthread_cond_broadcast(&inst->cond);
	pthread_mutex_unlock(&inst->lock);
}


/**
 * Locks the queue.
 */
void
avbox_queue_lock(struct avbox_queue * inst)
{
	assert(inst != NULL);
	pthread_mutex_lock(&inst->lock);
}


/**
 * Unlocks the queue.
 */
void
avbox_queue_unlock(struct avbox_queue * inst)
{
	assert(inst != NULL);
	pthread_mutex_unlock(&inst->lock);
}


/**
 * Gets the count of items in the queue.
 */
size_t
avbox_queue_count(struct avbox_queue * const inst)
{
	assert(inst != NULL);
	return inst->cnt;
}


/**
 * Gets the next node on the queue.
 */
static struct avbox_queue_node *
avbox_queue_getnode(struct avbox_queue * const inst, const int block, const int64_t timeout)
{
	struct avbox_queue_node *node = NULL;
	assert(inst != NULL);

	inst->waiters++;

	/* get the next item on the queue. if the queue is empty
	 * go to sleep */
	if ((node = LIST_TAIL(struct avbox_queue_node*, &inst->items)) == NULL) {
		if (inst->closed) {
			errno = ESHUTDOWN;
			goto end;
		}
		if (!block) {
			errno = EAGAIN;
			goto end;
		}
		if (timeout == 0) {
			pthread_cond_wait(&inst->cond, &inst->lock);
		} else {
			struct timespec tv;
			tv.tv_sec = 0;
			tv.tv_nsec = timeout * 1000L;
			delay2abstime(&tv);
			pthread_cond_timedwait(&inst->cond, &inst->lock, &tv);
		}
		if ((node = LIST_TAIL(struct avbox_queue_node*, &inst->items)) == NULL) {
			errno = EAGAIN;
			goto end;
		}
	}

	/* dequeue the item and return it */
	assert(node != NULL);
end:
	inst->waiters--;
	return node;
}


/**
 * Returns the next item in the queue without dequeueing it.
 */
void *
avbox_queue_peek(struct avbox_queue * const inst, const int block)
{
	void *ret = NULL;
	struct avbox_queue_node *node;
	pthread_mutex_lock(&inst->lock);
	if ((node = avbox_queue_getnode(inst, block, 0)) == NULL) {
		goto end;
	}
	ret = node->value;
end:
	pthread_mutex_unlock(&inst->lock);
	return ret;
}


/**
 * Peeks and block only for timeout micro seconds.
 */
void *
avbox_queue_timedpeek(struct avbox_queue * const inst, const int64_t timeout)
{
	void *ret = NULL;
	struct avbox_queue_node *node;
	pthread_mutex_lock(&inst->lock);
	if ((node = avbox_queue_getnode(inst, 1, timeout)) == NULL) {
		goto end;
	}
	ret = node->value;
end:
	pthread_mutex_unlock(&inst->lock);
	return ret;
}


/**
 * Wait for any IO events on the queue
 */
void
avbox_queue_wait(struct avbox_queue * const inst)
{
	pthread_mutex_lock(&inst->lock);
	pthread_cond_wait(&inst->cond, &inst->lock);
	pthread_mutex_unlock(&inst->lock);
}


/**
 * Places an item in the queue. Does not return until either
 * there is data on the queue or the queue is destroyed.
 */
void *
avbox_queue_get(struct avbox_queue *inst)
{
	void *ret = NULL;
	struct avbox_queue_node *node;
	assert(inst != NULL);

	pthread_mutex_lock(&inst->lock);
	if ((node = avbox_queue_getnode(inst, 1, 0)) == NULL) {
		goto end;
	}

	/* dequeue the item and return it */
	assert(node != NULL);
	ret = node->value;
	LIST_REMOVE(node);
	free(node);
	inst->cnt--;
	assert(ret != NULL);
end:
	pthread_cond_broadcast(&inst->cond);
	pthread_mutex_unlock(&inst->lock);
	return ret;
}


/**
 * Puts an item in the queue.
 *
 * Returns -1 on error and sets errno to ENOMEM or EGAIN.
 */
int
avbox_queue_put(struct avbox_queue *inst, void *item)
{
	int ret = -1;
	struct avbox_queue_node *node;

	assert(inst != NULL);
	assert(item != NULL);

	/* allocate memory for a queue node */
	if ((node = malloc(sizeof(struct avbox_queue_node))) == NULL) {
		LOG_VPRINT_ERROR("Could not allocate node: %s",
			strerror(errno));
		/* errno set to ENOMEM */
		return -1;
	}

	/* add item to queue and signal any threads waiting
	 * for it */
	pthread_mutex_lock(&inst->lock);
	inst->waiters++;

	/* if the queue is full we must wait. if it's still full when
	 * we wake return EAGAIN */
	if (inst->sz > 0 && inst->cnt >= inst->sz) {
		if (inst->closed) {
			errno = ESHUTDOWN;
			free(node);
			goto end;
		}
		pthread_cond_wait(&inst->cond, &inst->lock);	
		if (inst->cnt >= inst->sz) {
			errno = EAGAIN;
			free(node);
			goto end;
		}
	}

	node->value = item;
	LIST_ADD(&inst->items, node);
	inst->cnt++;
	ret = 0;
end:
	inst->waiters--;
	pthread_cond_broadcast(&inst->cond);
	pthread_mutex_unlock(&inst->lock);
	return ret;
}


/**
 * Check if the queue is closed.
 */
int
avbox_queue_isclosed(struct avbox_queue * const inst)
{
	assert(inst != NULL);
	return inst->closed;
}


/**
 * Sets the queue size limit. NOTE: If the queue is currently
 * above the newly set limit all calls to avbox_queue_put() will
 * continue to block until the size is reduced bellow the new
 * size */
void
avbox_queue_setsize(struct avbox_queue * const inst, size_t sz)
{
	inst->sz = sz;
}


/**
 * Creates a new queue object.
 */
struct avbox_queue *
avbox_queue_new(const size_t sz)
{
	int res;
	struct avbox_queue *inst;

	/* allocate memory for queue */
	if ((inst = malloc(sizeof(struct avbox_queue))) == NULL) {
		LOG_VPRINT_ERROR("Could not create queue: %s",
			strerror(errno));
		assert(errno == ENOMEM);
		return NULL;
	}

	/* initialize pthread primitives */
	if ((res = pthread_mutex_init(&inst->lock, NULL)) != 0 ||
		(res = pthread_cond_init(&inst->cond, NULL)) != 0) {
		LOG_VPRINT_ERROR("Could not initialize queue mutex: %s!",
			strerror(errno));
		assert(res == ENOMEM || res == EPERM);
		errno = res;
		free(inst);
		return NULL;
	}


	inst->closed = 0;
	inst->waiters = 0;
	inst->cnt = 0;
	inst->sz = sz;
	LIST_INIT(&inst->items);

	return inst;
}


/**
 * Close the queue. After the queue is closed all attempts to write
 * to it will return ESHUTDOWN. Any attempts to read from it will succeed
 * as long as there are items in the queue, otherwise they will fail
 * with ESHUTDOWN.
 */
void
avbox_queue_close(struct avbox_queue * inst)
{
	assert(inst != NULL);
	inst->closed = 1;
	avbox_queue_wake(inst);
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
	inst->closed = 1;
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
