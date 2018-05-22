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
#       include <libavbox/config.h>
#endif
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sched.h>

#define LOG_MODULE "dispatch"

#include <libavbox/avbox.h>


#define AVBOX_MESSAGE_POOL_SIZE		(10)
#define AVBOX_STACK_TOUCH_BYTES		(4096)

/**
 * Represents a dispatch queue.
 */
LISTABLE_STRUCT(avbox_dispatch_queue,
	pid_t tid;
	struct avbox_queue *queue;
);


/**
 * Represents a dispatch object.
 */
LISTABLE_STRUCT(avbox_object,
	pthread_mutex_t lock;
	unsigned int refs;
	int destroyed;
	int destroy_timer_id;
	struct avbox_dispatch_queue *q;
	avbox_message_handler handler;
	void *context;
);


/**
 * Dispatch message structure.
 */
LISTABLE_STRUCT(avbox_message,
	int id;
	int flags;
	int must_free_dest;
	void *dest;
	void *payload;
);


LISTABLE_STRUCT(avbox_dest_header,
	struct avbox_object *object;
);


static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static int initialized = 0;
static LIST queues;

static LIST message_pool;
static pthread_mutex_t message_pool_lock;
static int pools_primed = 0;
#if 0
static LIST dest_pool;
static pthread_mutex_t dest_pool_lock;;
#endif


static struct avbox_message *
acquire_message()
{
	static int allocs = 0;
	struct avbox_message *msg;
	pthread_mutex_lock(&message_pool_lock);
	msg = LIST_TAIL(struct avbox_message*, &message_pool);
	if (UNLIKELY(msg == NULL)) {
		if ((msg = malloc(sizeof(struct avbox_message))) == NULL) {
			ASSERT(errno == ENOMEM);
			pthread_mutex_unlock(&message_pool_lock);
			return NULL;
		}
		allocs++;
		if (pools_primed) {
			LOG_VPRINT_INFO("Allocated message structure (total_allocs=%i)",
				allocs);
		}
	} else {
		LIST_REMOVE(msg);
	}
	pthread_mutex_unlock(&message_pool_lock);
	return msg;
}


static void
release_message(struct avbox_message * const msg)
{
	pthread_mutex_lock(&message_pool_lock);
	LIST_ADD(&message_pool, msg);
	pthread_mutex_unlock(&message_pool_lock);
}


#if 0
static struct avbox_dest_header*
acquire_dest_header()
{
	struct avbox_dest_header *hdr;
	pthread_mutex_lock(&dest_pool_lock);
	hdr = LIST_TAIL(struct avbox_dest_header*, &dest_pool);
	if (UNLIKELY(hdr == NULL)) {
		if ((hdr = malloc(sizeof(struct avbox_dest_header))) == NULL) {
			ASSERT(errno == ENOMEM);
			pthread_mutex_unlock(&dest_pool_lock);
			return NULL;
		}
		LOG_PRINT_INFO("Allocated dest* structure");
	} else {
		LIST_REMOVE(hdr);
	}
	pthread_mutex_unlock(&dest_pool_lock);
	return hdr;
}


static void
release_dest_header(struct avbox_dest_header * const hdr)
{
	pthread_mutex_lock(&dest_pool_lock);
	LIST_ADD(&dest_pool, hdr);
	pthread_mutex_unlock(&dest_pool_lock);
}
#endif


/**
 * Get the queue for a thread.
 */
static struct avbox_dispatch_queue *
avbox_dispatch_getqueue(const pid_t tid)
{
	pid_t _tid;
	struct avbox_dispatch_queue *q = NULL, *queue;
	if ((_tid = tid) == -1) {
		_tid = getpid();
	}
	pthread_mutex_lock(&queue_lock);
	LIST_FOREACH(struct avbox_dispatch_queue*, queue, &queues) {
		if (queue->tid == _tid) {
			q = queue;
			break;
		}
	}
	pthread_mutex_unlock(&queue_lock);
	if (q == NULL) {
		errno = ENOENT;
	}
	return q;
}


struct avbox_object*
avbox_object_ref(struct avbox_object * const obj)
{
	ASSERT(obj != NULL);
	ATOMIC_INC(&obj->refs);
	return obj;
}


EXPORT void
avbox_object_unref(struct avbox_object * const obj)
{
	ATOMIC_DEC(&obj->refs);

	if (UNLIKELY(obj->refs == 0)) {
		pthread_mutex_lock(&obj->lock);
		ASSERT(obj->refs == 0);
		if (obj->refs == 0) {
			ASSERT(obj->destroyed == 1);
			ASSERT(obj->destroy_timer_id = -1);
			pthread_mutex_unlock(&obj->lock);
			free(obj);
			return;
		}
		pthread_mutex_unlock(&obj->lock);
	}
}


/**
 * Decrease reference counter and free
 * the message once the counter reaches zero.
 */
static void
avbox_dispatch_freemsg(struct avbox_message * const msg)
{
	ASSERT(msg != NULL);
	if (LIKELY(!msg->must_free_dest /*msg->flags & AVBOX_DISPATCH_UNICAST */)) {
		avbox_object_unref(msg->dest);
	} else {
		if (msg->dest != NULL) {
			struct avbox_object * const * dest = msg->dest;
			while (*dest != NULL) {
				avbox_object_unref(*dest++);
			}
			free(msg->dest);
		}
	}
	release_message(msg);
}


/**
 * Message handler
 */
static int
avbox_object_handler(struct avbox_object * const object, struct avbox_message * const msg)
{
	switch (msg->id) {
	case AVBOX_MESSAGETYPE_TIMER:
	{
		struct avbox_timer_data * const timer_data =
			avbox_message_payload(msg);
		if (timer_data->id == object->destroy_timer_id) {
			/* resend the DESTROY message */
			if (avbox_object_sendmsg(&object,
				AVBOX_MESSAGETYPE_DESTROY, AVBOX_DISPATCH_UNICAST, NULL) == NULL) {
				LOG_VPRINT_ERROR("Could not send CLEANUP message: %s",
					strerror(errno));
			}
			avbox_timers_releasepayload(timer_data);
			object->destroy_timer_id = -1;
			return AVBOX_DISPATCH_OK;
		} else {
			return object->handler(object->context, msg);
		}
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		int ret;

		ASSERT(object != NULL);
		ASSERT(object->destroy_timer_id == -1);

		if ((ret = object->handler(object->context, msg)) == AVBOX_DISPATCH_OK) {
			/* destroy object so it won't receive
			 * more messages */
			object->destroyed = 1;
			if (avbox_object_sendmsg(&object,
				AVBOX_MESSAGETYPE_CLEANUP, AVBOX_DISPATCH_UNICAST, NULL) == NULL) {
				LOG_VPRINT_ERROR("Could not send CLEANUP message: %s",
					strerror(errno));
			}
		} else {
			struct timespec tv;
			ASSERT(ret == AVBOX_DISPATCH_CONTINUE);
			(void) ret;

			/* Set a timer to invoke the destructor again after
			 * a delay */
			tv.tv_sec = 0;
			tv.tv_nsec = 100LL * 1000LL * 1000LL;
			object->destroy_timer_id = avbox_timer_register(&tv,
				AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
				object, NULL, NULL);

			/* if the timer fails for any reason (it shouldn't) then
			 * we need to send the DESTROY message again immediately */
			if (object->destroy_timer_id == -1) {
				LOG_VPRINT_ERROR("Could not register destructor timer: %s",
					strerror(errno));
				if (avbox_object_sendmsg(&object,
					AVBOX_MESSAGETYPE_DESTROY, AVBOX_DISPATCH_UNICAST, NULL) == NULL) {
					LOG_VPRINT_ERROR("Could not send CLEANUP message: %s",
						strerror(errno));
				}
				/* yield the CPU so the other threads can do their work */
				sched_yield();
			}
		}

		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		ASSERT(object != NULL);
		(void) object->handler(object->context, msg);
		avbox_object_unref(object);
		return AVBOX_DISPATCH_OK;
	}
	default:
		return object->handler(object->context, msg);
	}
}


/**
 * Get the type of a message
 */
EXPORT int
avbox_message_id(const struct avbox_message * const msg)
{
	ASSERT(msg != NULL);
	return msg->id;
}


/**
 * Get the message payload.
 */
EXPORT void *
avbox_message_payload(const struct avbox_message * const msg)
{
	ASSERT(msg != NULL);
	return msg->payload;
}


/**
 * Clones the list of destination object and reference
 * each object on the list.
 */
static struct avbox_object **
avbox_dispatch_destdup(struct avbox_object * const * const dest)
{
	int c = 0;
	struct avbox_object * const *pdest = dest, **out;

	ASSERT(dest != NULL);
	ASSERT(*dest != NULL);

	/* count the objects in the array */
	while (*pdest != NULL) {
		pdest++;
		c++;
	}

	/* allocate memory */
	if ((out = malloc(++c * sizeof(struct avbox_object*))) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}

	c = 0;
	pdest = dest;
	while (*pdest != NULL) {
		if ((*pdest)->destroyed) {
			DEBUG_PRINT("dispatch", "Sending message to destroyed object!!");
		}
		out[c++] = avbox_object_ref(*pdest++);
	}
	out[c] = NULL;
	return out;
}


/**
 * Sends a message.
 */
EXPORT struct avbox_message*
avbox_object_sendmsg(struct avbox_object * const * const dest,
	const int id, const int flags, void * const payload)
{
	struct avbox_dispatch_queue *q;
	const int cast = flags & (AVBOX_DISPATCH_UNICAST |
		AVBOX_DISPATCH_ANYCAST | AVBOX_DISPATCH_MULTICAST |
		AVBOX_DISPATCH_BROADCAST);

	/* allocate message */
	struct avbox_message * const msg = acquire_message();
	if (UNLIKELY(msg == NULL)) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}

	/* initialize message */
	msg->dest = NULL;
	msg->must_free_dest = 1;
	msg->flags = flags;
	msg->payload = payload;
	msg->id = id;

	switch (cast) {
	case AVBOX_DISPATCH_UNICAST:
	{
		msg->dest = avbox_object_ref(*dest);
		msg->must_free_dest = 0;
		q = (*dest)->q;
		break;
	}
	case AVBOX_DISPATCH_ANYCAST:
	case AVBOX_DISPATCH_MULTICAST:
	{
		struct avbox_object **dest_copy;
		ASSERT(dest != NULL);
		if ((dest_copy = avbox_dispatch_destdup(dest)) == NULL) {
			assert(errno == ENOMEM);
			release_message(msg);
			return NULL;
		}
		assert(*dest_copy != NULL);
		msg->dest = dest_copy;
		q = (*dest_copy)->q;
		break;
	}
	default:
		DEBUG_VABORT("dispatch", "Invalid cast: %i", cast);
	}

	/* put the message on the target thread's queue */
	if (avbox_queue_put(q->queue, msg) == -1) {
		assert(errno == ENOMEM || errno == EAGAIN || errno == ESHUTDOWN);
		avbox_dispatch_freemsg(msg);
		return NULL;
	}

	return msg;
}


/**
 * Create a dispatch object.
 *
 * Returns NULL on failure and sets errno to:
 * EINVAL - The queue for this thread has not been initialized.
 * ENOMEM - Out of memory.
 */
EXPORT struct avbox_object*
avbox_object_new(avbox_message_handler handler, void * const context)
{
	struct avbox_object *obj;
	struct avbox_dispatch_queue *q;
	pthread_mutexattr_t lockattr;

	if ((q = avbox_dispatch_getqueue(avbox_gettid())) == NULL) {
		assert(errno == ENOENT);
		return NULL;
	}

	if ((obj = malloc(sizeof(struct avbox_object))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	/* the destructor for a self destructing object like the
	 * main window will be called in response to a message sent
	 * to the object so it will be locked at that time, therefore
	 * it needs to be recursive */
	pthread_mutexattr_init(&lockattr);
	pthread_mutexattr_settype(&lockattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutexattr_setprotocol(&lockattr, PTHREAD_PRIO_INHERIT);
	if (pthread_mutex_init(&obj->lock, &lockattr) != 0) {
		return NULL;
	}
	pthread_mutexattr_destroy(&lockattr);

	obj->q = q;
	obj->handler = handler;
	obj->context = context;
	obj->refs = 1;
	obj->destroyed = 0;
	obj->destroy_timer_id = -1;

	return obj;
}


/**
 * Destroy a dispatch object.
 */
EXPORT void
avbox_object_destroy(struct avbox_object * const object)
{
	ASSERT(object != NULL);
	if (avbox_object_sendmsg(&object,
		AVBOX_MESSAGETYPE_DESTROY, AVBOX_DISPATCH_UNICAST, NULL) == NULL) {
		LOG_VPRINT_ERROR("Could not send DESTROY message: %s",
			strerror(errno));
	}
}


/**
 * Initialized a dispatch queue for the current thread.
 */
INTERNAL struct avbox_queue*
avbox_dispatch_init()
{
	int i;
	char qname[256];
	void *pointers[AVBOX_STACK_TOUCH_BYTES];
	struct avbox_dispatch_queue *q;

	if (!initialized) {
		pthread_mutexattr_t prio_inherit;
		pthread_mutexattr_init(&prio_inherit);
		pthread_mutexattr_setprotocol(&prio_inherit, PTHREAD_PRIO_INHERIT);
		if (pthread_mutex_init(&message_pool_lock, &prio_inherit) != 0) {
			ABORT("Could not initialize mutex");
		}
		pthread_mutexattr_destroy(&prio_inherit);

		LIST_INIT(&queues);
		LIST_INIT(&message_pool);
		initialized = 1;
	}


	/* if a queue for this thread already exists then
	 * abort() */
	if (avbox_dispatch_getqueue(avbox_gettid()) != NULL) {
		LOG_PRINT_ERROR("Queue for this thread already created!");
		errno = EALREADY;
		return NULL;
	}

	/* allocate queue */
	if ((q = malloc(sizeof(struct avbox_dispatch_queue))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}
	if ((q->queue = avbox_queue_new(10)) == NULL) {
		assert(errno == ENOMEM || errno == EPERM);
		free(q);
		return NULL;
	}

	q->tid = avbox_gettid();

	/* set the queue name and unbind it's size */
	snprintf(qname, sizeof(qname) - 1, "thread-%i", q->tid);
	avbox_queue_setname(q->queue, qname);
	avbox_queue_setsize(q->queue, 0);

	pthread_mutex_lock(&queue_lock);
	LIST_ADD(&queues, q);
	pthread_mutex_unlock(&queue_lock);

	/* touch the stack and prime the message pool */
	memset(pointers, 0, sizeof(pointers));
	for (i = 0; i < AVBOX_MESSAGE_POOL_SIZE; i++) {
		pointers[i] = acquire_message();
	}
	for (i = 0; i < AVBOX_MESSAGE_POOL_SIZE; i++) {
		release_message(pointers[i]);
	}

	return q->queue;
}


/**
 * Close the dispatch queue.
 */
INTERNAL void
avbox_dispatch_close(void)
{
	struct avbox_dispatch_queue *q;
	/* get the thread's queue */
	if ((q = avbox_dispatch_getqueue(avbox_gettid())) == NULL) {
		LOG_PRINT_ERROR("Queue not initialized!");
		abort();
	}
	avbox_queue_close(q->queue);
}


/**
 * Destroys the queue for the current thread.
 */
INTERNAL void
avbox_dispatch_shutdown(void)
{
	struct avbox_dispatch_queue *q;
	struct avbox_message *msg;

#ifndef NDEBUG
	if (!initialized) {
		DEBUG_PRINT("dispatch", "Dispatch not initialized!");
		abort();
	}
#endif

	/* get the thread's queue */
	if ((q = avbox_dispatch_getqueue(avbox_gettid())) == NULL) {
		LOG_PRINT_ERROR("Queue not initialized!");
		abort();
	}

	/* flush and destroy the thread's queue */
	/* TODO: Implement a message destructor for freeing
	 * the payload of flushed messages */
	avbox_queue_close(q->queue);
	while ((msg = avbox_queue_get(q->queue)) != NULL) {
		DEBUG_VPRINT("dispatch", "LEAK: Leftover message (id=0x%02x)",
			msg->id);
		avbox_dispatch_freemsg(msg);
	}
	avbox_queue_destroy(q->queue);

	/* remove queue from list and free it */
	pthread_mutex_lock(&queue_lock);
	LIST_REMOVE(q);
	pthread_mutex_unlock(&queue_lock);
	free(q);

	LIST_FOREACH_SAFE(struct avbox_message*, msg, &message_pool, {
		LIST_REMOVE(msg);
		free(msg);
	});
}


/**
 * Run the main dispatch loop
 */
INTERNAL void
avbox_message_dispatch(struct avbox_message * const msg)
{
	assert(msg != NULL);

	const int cast = msg->flags & (AVBOX_DISPATCH_ANYCAST |
		AVBOX_DISPATCH_MULTICAST | AVBOX_DISPATCH_UNICAST |
		AVBOX_DISPATCH_BROADCAST);

	switch (cast) {
	case AVBOX_DISPATCH_ANYCAST:
	{
		struct avbox_object **dest = msg->dest;
		assert(dest != NULL);
		while (LIKELY(*dest != NULL)) {
			int res;

			pthread_mutex_lock(&(*dest)->lock);
			if (!(*dest)->destroyed || msg->id == AVBOX_MESSAGETYPE_CLEANUP) {
				if ((res = avbox_object_handler(*dest, msg)) == AVBOX_DISPATCH_OK) {
					pthread_mutex_unlock(&(*dest)->lock);
					break;
				}
				DEBUG_VASSERT("dispatch", (res == AVBOX_DISPATCH_CONTINUE),
					"Handler returned invalid code: %i", res);
			} else {
				DEBUG_PRINT("dispatch", "Target has been destroyed!");
			}
			pthread_mutex_unlock(&(*dest)->lock);
			dest++;
		}
		avbox_dispatch_freemsg(msg);
		break;
	}
	case AVBOX_DISPATCH_UNICAST:
	{
		struct avbox_object * const dest = msg->dest;
		ASSERT(dest != NULL);
		ASSERT(msg->must_free_dest == 0);
		pthread_mutex_lock(&dest->lock);
		if (LIKELY(!dest->destroyed || msg->id == AVBOX_MESSAGETYPE_CLEANUP)) {
			(void) avbox_object_handler(dest, msg);
		} else {
			DEBUG_PRINT("dispatch", "Target has been destroyed!");
		}
		pthread_mutex_unlock(&dest->lock);
		avbox_dispatch_freemsg(msg);
		break;
	}
	case AVBOX_DISPATCH_MULTICAST:
	{
		struct avbox_object **dest = msg->dest;
		while (*dest != NULL) {
			pthread_mutex_lock(&(*dest)->lock);
			if (!(*dest)->destroyed || msg->id == AVBOX_MESSAGETYPE_CLEANUP) {
				(void) avbox_object_handler(*dest, msg);
			} else {
				DEBUG_PRINT("dispatch", "Target has been destroyed!");
			}
			pthread_mutex_unlock(&(*dest)->lock);
			dest++;
		}
		avbox_dispatch_freemsg(msg);
		break;
	}
	default:
		DEBUG_ABORT("dispatch", "Invalid cast type!");
	}
}
