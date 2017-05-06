#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>

#define LOG_MODULE "dispatch"

#include "log.h"
#include "debug.h"
#include "linkedlist.h"
#include "queue.h"
#include "dispatch.h"
#include "compiler.h"


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
LISTABLE_STRUCT(avbox_dispatch_object,
	pthread_mutex_t lock;
	unsigned int refs;
	int destroyed;
	struct avbox_dispatch_queue *q;
	avbox_message_handler handler;
	void *context;
);


/**
 * Dispatch message structure.
 */
struct avbox_message
{
	struct avbox_dispatch_object** dest;
	int flags;
	int type;
	void *payload;
};


static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static int initialized = 0;
static LIST objects;
static LIST queues;


static pid_t
gettid(void)
{
	return syscall(__NR_gettid);
}


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


/**
 * Reference a dispatch object.
 */
static struct avbox_dispatch_object*
avbox_dispatch_refobject(struct avbox_dispatch_object *obj)
{
	assert(obj != NULL);
	ATOMIC_INC(&obj->refs);
	return obj;
}


/**
 * Unreference a dispatch object.
 */
static void
avbox_dispatch_unrefobject(struct avbox_dispatch_object *obj)
{
	ATOMIC_DEC(&obj->refs);
	if (obj->refs == 0) {
		pthread_mutex_lock(&obj->lock);
		assert(obj->refs == 0);
		if (obj->refs == 0) {
			assert(obj->destroyed == 1);
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
avbox_dispatch_freemsg(struct avbox_message *msg)
{
	assert(msg != NULL);
	if (msg->dest != NULL) {
		struct avbox_dispatch_object **dest = msg->dest;
		while (*dest != NULL) {
			avbox_dispatch_unrefobject(*dest++);
		}
		free(msg->dest);
	}
	free(msg);
}


/**
 * Swap two endpoints.
 */
void
avbox_dispatch_swapobject(struct avbox_dispatch_object *a,
	struct avbox_dispatch_object *b)
{
	pthread_mutex_lock(&lock);
	LIST_SWAP(a, b);
	pthread_mutex_unlock(&lock);
}


/**
 * Get the type of a message
 */
int
avbox_dispatch_getmsgtype(struct avbox_message *msg)
{
	assert(msg != NULL);
	return msg->type;
}


/**
 * Get the message payload.
 */
void *
avbox_dispatch_getmsgpayload(struct avbox_message *msg)
{
	assert(msg != NULL);
	return msg->payload;
}


/**
 * Gets a message from the queue.
 */
struct avbox_message*
avbox_dispatch_getmsg(void)
{
	struct avbox_message *msg;
	struct avbox_dispatch_queue *q;

	/* get the queue for this thread */
	if ((q = avbox_dispatch_getqueue(gettid())) == NULL) {
		return NULL;
	}

	assert(q != NULL);

	if ((msg = avbox_queue_get(q->queue)) == NULL) {
		switch (errno) {
		case EAGAIN:
		case ESHUTDOWN: 
			return NULL;
		default:
			LOG_VPRINT_ERROR("Unexpected queue error: %s",
				strerror(errno));
			abort();
		}
	}

	return msg;
}


/**
 * Peek the next message in the thread's queue.
 */
struct avbox_message *
avbox_dispatch_peekmsg(void)
{
	struct avbox_message *msg;
	struct avbox_dispatch_queue *q;

	/* get the queue for this thread */
	if ((q = avbox_dispatch_getqueue(gettid())) == NULL) {
		return NULL;
	}

	assert(q != NULL);

	if ((msg = avbox_queue_peek(q->queue, 0)) == NULL) {
		switch (errno) {
		case EAGAIN:
		case ESHUTDOWN: 
			return NULL;
		default:
			LOG_VPRINT_ERROR("Unexpected queue error: %s",
				strerror(errno));
			abort();
		}
	}

	return msg;

}


/**
 * Copy destination list.
 */
#if 0
static struct avbox_dispatch_object **
avbox_dispatch_destall()
{
	int i = 0;
	struct avbox_dispatch_object **out = NULL, *obj;
	pthread_mutex_lock(&lock);
	if ((out = malloc((LIST_SIZE(&objects) + 1) *
		sizeof(struct avbox_dispatch_object*))) == NULL) {
		assert(errno == ENOMEM);
		goto end;
	}
	LIST_FOREACH(struct avbox_dispatch_object*, obj, &objects) {
		out[i++] = obj;
	}
	out[i] = NULL;
end:
	pthread_mutex_unlock(&lock);
	return out;
}
#endif


/**
 * Clones the list of destination object and reference
 * each object on the list.
 */
static struct avbox_dispatch_object **
avbox_dispatch_destdup(struct avbox_dispatch_object **dest)
{
	int c = 0;
	struct avbox_dispatch_object **pdest = dest, **out;
	assert(dest != NULL);
	assert(*dest != NULL);

	/* count the objects in the array */
	while (*pdest != NULL) {
		pdest++;
		c++;
	}

	/* allocate memory */
	if ((out = malloc(++c * sizeof(struct avbox_dispatch_object*))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	c = 0;
	pdest = dest;
	while (*pdest != NULL) {
		out[c++] = avbox_dispatch_refobject(*pdest++);
	}
	out[c] = NULL;
	return out;
}


/**
 * Sends a message.
 */
struct avbox_message*
avbox_dispatch_sendmsg(const int tid, struct avbox_dispatch_object **dest,
	int type, int flags, void *payload)
{
	int cast;
	struct avbox_message *msg;
	struct avbox_dispatch_queue *q;

	/* allocate message */
	if ((msg = malloc(sizeof(struct avbox_message))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	/* initialize message */
	msg->dest = NULL;
	msg->flags = flags;
	msg->payload = payload;
	msg->type = type;

	cast = flags & (AVBOX_DISPATCH_UNICAST |
		AVBOX_DISPATCH_ANYCAST | AVBOX_DISPATCH_MULTICAST |
		AVBOX_DISPATCH_BROADCAST);

	switch (cast) {
	case AVBOX_DISPATCH_UNICAST:
	{
		if ((msg->dest = malloc(sizeof(struct avbox_dispatch_object*) * 2)) == NULL) {
			assert(errno == ENOMEM);
			free(msg);
			return NULL;
		}
		msg->dest[0] = avbox_dispatch_refobject(*dest);
		msg->dest[1] = NULL;
		q = (*dest)->q;
		break;
	}
	case AVBOX_DISPATCH_ANYCAST:
	case AVBOX_DISPATCH_MULTICAST:
		assert(dest != NULL);
		assert(tid == -1);
		if ((msg->dest = avbox_dispatch_destdup(dest)) == NULL) {
			assert(errno == ENOMEM);
			free(msg);
			return NULL;
		}
		assert(*msg->dest != NULL);
		q = (*msg->dest)->q;
		break;
	case AVBOX_DISPATCH_BROADCAST:
	{
		assert(dest == NULL);
		assert(tid != -1);

		/* get the queue for the target thread */
		if ((q = avbox_dispatch_getqueue(tid)) == NULL) {
			assert(errno == ENOENT);
			free(msg);
			return NULL;
		}

		break;
	}
	default:
		DEBUG_VPRINT("dispatch", "Invalid cast: %i",
			cast);
		errno = EINVAL;
		free(msg);
		return NULL;
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
struct avbox_dispatch_object*
avbox_dispatch_createobject(avbox_message_handler handler, int flags, void *context)
{
	struct avbox_dispatch_object *obj;
	struct avbox_dispatch_queue *q;
	pthread_mutexattr_t lockattr;

	if ((q = avbox_dispatch_getqueue(gettid())) == NULL) {
		assert(errno == EINVAL);
		return NULL;
	}

	if ((obj = malloc(sizeof(struct avbox_dispatch_object))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	/* the destructor for a self destructing object like the
	 * main window will be called in response to a message sent
	 * to the object so it will be locked at that time, therefore
	 * it needs to be recursive */
	pthread_mutexattr_init(&lockattr);
	pthread_mutexattr_settype(&lockattr, PTHREAD_MUTEX_RECURSIVE);
	if (pthread_mutex_init(&obj->lock, &lockattr) != 0) {
		return NULL;
	}

	obj->q = q;
	obj->handler = handler;
	obj->context = context;
	obj->refs = 1;
	obj->destroyed = 0;

	pthread_mutex_lock(&lock);
	LIST_ADD(&objects, obj);
	pthread_mutex_unlock(&lock);

	return obj;
}


/**
 * Destroy a dispatch object.
 */
void
avbox_dispatch_destroyobject(struct avbox_dispatch_object *obj)
{
	assert(obj != NULL);
	pthread_mutex_lock(&lock);
	obj->destroyed = 1;
	LIST_REMOVE(obj);
	pthread_mutex_unlock(&lock);
	avbox_dispatch_unrefobject(obj);
}


/**
 * Initialized a dispatch queue for the current thread.
 */
int
avbox_dispatch_init()
{
	struct avbox_dispatch_queue *q;

	if (!initialized) {
		LIST_INIT(&objects);
		LIST_INIT(&queues);
		initialized = 1;
	}


	/* if a queue for this thread already exists then
	 * abort() */
	if (avbox_dispatch_getqueue(gettid()) != NULL) {
		LOG_PRINT_ERROR("Queue for this thread already created!");
		errno = EALREADY;
		return -1;
	}

	/* allocate queue */
	if ((q = malloc(sizeof(struct avbox_dispatch_queue))) == NULL) {
		assert(errno == ENOMEM);
		return -1;
	}
	if ((q->queue = avbox_queue_new(0)) == NULL) {
		assert(errno == ENOMEM || errno == EPERM);
		free(q);
		return -1;
	}

	q->tid = gettid();

	pthread_mutex_lock(&queue_lock);
	LIST_ADD(&queues, q);
	pthread_mutex_unlock(&queue_lock);
	return 0;
}


/**
 * Destroys the queue for the current thread.
 */
void
avbox_dispatch_shutdown(void)
{
	struct avbox_dispatch_queue *q;

#ifndef NDEBUG
	if (!initialized) {
		DEBUG_PRINT("dispatch", "Dispatch not initialized!");
		abort();
	}
#endif

	/* get the thread's queue */
	if ((q = avbox_dispatch_getqueue(gettid())) == NULL) {
		LOG_PRINT_ERROR("Queue not initialized!");
		abort();
	}

	/* remove queue from list and free it */
	pthread_mutex_lock(&queue_lock);
	LIST_REMOVE(q);
	pthread_mutex_unlock(&queue_lock);
	free(q);
}


/**
 * Run the main dispatch loop
 */
void
avbox_dispatch_dispatchmsg(struct avbox_message *msg)
{
	const pid_t tid = gettid();
	assert(msg != NULL);

	const int cast = msg->flags & (AVBOX_DISPATCH_ANYCAST |
		AVBOX_DISPATCH_MULTICAST | AVBOX_DISPATCH_UNICAST |
		AVBOX_DISPATCH_BROADCAST);

	switch (cast) {
	case AVBOX_DISPATCH_ANYCAST:
	{
		struct avbox_dispatch_object **dest = msg->dest;
		assert(dest != NULL);
		while (*dest != NULL) {
			int res;
			DEBUG_VASSERT("dispatch", (*dest)->q->tid == tid,
				"Received message for thread %i on thread %i",
				(*dest)->q->tid, tid);

			pthread_mutex_lock(&(*dest)->lock);
			if (!(*dest)->destroyed) {
				if ((res = (*dest)->handler((*dest)->context, msg)) == AVBOX_DISPATCH_OK) {
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
		assert(msg->dest != NULL);
		assert(*msg->dest != NULL);
		pthread_mutex_lock(&(*msg->dest)->lock);
		if (!(*msg->dest)->destroyed) {
			(void) (*msg->dest)->handler((*msg->dest)->context, msg);
		} else {
			DEBUG_PRINT("dispatch", "Target has been destroyed!");
		}
		pthread_mutex_unlock(&(*msg->dest)->lock);
		avbox_dispatch_freemsg(msg);
		break;
	}
	case AVBOX_DISPATCH_MULTICAST:
	{
		struct avbox_dispatch_object **dest = msg->dest;
		while (*dest != NULL) {
			pthread_mutex_lock(&(*dest)->lock);
			if (!(*dest)->destroyed) {
				(void) (*dest)->handler((*dest)->context, msg);
			} else {
				DEBUG_PRINT("dispatch", "Target has been destroyed!");
			}
			pthread_mutex_unlock(&(*dest)->lock);
			dest++;
		}
		avbox_dispatch_freemsg(msg);
		break;
	}
	case AVBOX_DISPATCH_BROADCAST:
	{
		const pid_t tid = gettid();
		struct avbox_dispatch_object *obj;
		assert(msg->dest == NULL);
		assert((msg->flags & AVBOX_DISPATCH_BROADCAST) == 0);
		pthread_mutex_lock(&lock);
		LIST_FOREACH(struct avbox_dispatch_object*, obj, &objects) {
			DEBUG_VASSERT("dispatch", obj->q->tid == tid,
				"Received message for thread %i on thread %i",
				obj->q->tid, tid);
			(void) obj->handler(obj->context, msg);
		}
		pthread_mutex_unlock(&lock);
		avbox_dispatch_freemsg(msg);
		break;
	}
	default:
		DEBUG_ABORT("dispatch", "Invalid cast type!");
	}
}
