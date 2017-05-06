#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>


#define LOG_MODULE "thread"

#include "log.h"
#include "debug.h"
#include "dispatch.h"
#include "linkedlist.h"
#include "delegate.h"
#include "time_util.h"


#define N_THREADS (3)


LISTABLE_STRUCT(avbox_thread,
	int quit;
	int busy;
#ifndef NDEBUG
	int no;
	int64_t jobs;
#endif
	pthread_t thread;
	struct timespec start_time;
	struct timespec stop_time;
	struct avbox_dispatch_object *object;
);


static LIST threads;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;


/**
 * Thread message handler.
 */
static int
avbox_thread_msghandler(void * const context, struct avbox_message *msg)
{
	struct avbox_thread * const thread = context;

	switch (avbox_dispatch_getmsgtype(msg)) {
	case AVBOX_MESSAGETYPE_DELEGATE:
	{
		struct avbox_delegate * const del =
			avbox_dispatch_getmsgpayload(msg);
		(void) clock_gettime(CLOCK_MONOTONIC, &thread->start_time);
		avbox_delegate_execute(del);
		(void) clock_gettime(CLOCK_MONOTONIC, &thread->stop_time);

#ifndef NDEBUG
		thread->jobs++;
#endif

		break;
	}
	case AVBOX_MESSAGETYPE_QUIT:
	{
		struct avbox_thread * const thread =
			avbox_dispatch_getmsgpayload(msg);
		thread->quit = 1;
		break;
	}
	default:
		abort();
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Thread entry point.
 */
static void*
avbox_thread_run(void *arg)
{
	struct avbox_thread * const thread = (struct avbox_thread*) arg;
	struct avbox_message * msg;

	DEBUG_SET_THREAD_NAME("avbox-worker");
	DEBUG_VPRINT("thread", "Thread #%i starting", thread->no);

	/* initialize message dispatcher */
	if (avbox_dispatch_init() == -1) {
		LOG_VPRINT_ERROR("Could not initialize dispatch: %s",
			strerror(errno));
		goto end;
	}

	/* create dispatch object */
	if ((thread->object = avbox_dispatch_createobject(
		avbox_thread_msghandler, 0, thread)) == NULL) {
		LOG_VPRINT_ERROR("Could not create dispatch object: %s",
			strerror(errno));
		avbox_dispatch_shutdown();
		goto end;
	}

	/* initialize thread timers */
	clock_gettime(CLOCK_MONOTONIC, &thread->start_time);
	clock_gettime(CLOCK_MONOTONIC, &thread->stop_time);

	/* signal that we're up and running */
	pthread_mutex_lock(&lock);
	thread->quit = 0;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&lock);

	/* run the message loop */
	while (!thread->quit) {
		if ((msg = avbox_dispatch_getmsg()) == NULL) {
			switch (errno) {
			case EAGAIN: continue;
			default:
				DEBUG_VABORT("thread", "avbox_dispatch_getmsg() returned an unexpected error: %s (%i)",
					strerror(errno), errno);
			}
		}
		avbox_dispatch_dispatchmsg(msg);
	}

	/* cleanup */
	avbox_dispatch_destroyobject(thread->object);
	avbox_dispatch_shutdown();

	DEBUG_VPRINT("thread", "Thread #%i exited after %li jobs",
		thread->no, thread->jobs);

end:
	/* signal that we've exited */
	pthread_mutex_lock(&lock);
	assert(thread->quit == 1);
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&lock);

	return NULL;
}


/**
 * Create a new thread.
 */
static struct avbox_thread *
avbox_thread_new(void)
{
	struct avbox_thread *thread;
#ifndef NDEBUG
	static int thread_no = 0;
#endif

	/* allocate memory */
	if ((thread = malloc(sizeof(struct avbox_thread))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	/* start thread */
	pthread_mutex_lock(&lock);
#ifndef NDEBUG
	thread->no = thread_no++;
#endif
	thread->quit = 1;
	if (pthread_create(&thread->thread, NULL, avbox_thread_run, thread) != 0) {
		free(thread);
		return NULL;
	}
	pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);

	/* check that the thread started successfully */
	if (thread->quit) {
		free(thread);
		thread = NULL;
	} else {
		LIST_APPEND(&threads, thread);
	}

	DEBUG_VPRINT("thread", "Thread %p started", thread);

	return thread;
}


/**
 * Destroy a thread.
 */
static void
avbox_thread_destroy(struct avbox_thread * const thread)
{
	DEBUG_VPRINT("thread", "Shutting down thread #%i",
		thread->no);

	assert(thread != NULL);

	LIST_REMOVE(thread);

	/* Send the QUIT message to the thread */
	if (avbox_dispatch_sendmsg(-1, &thread->object,
		AVBOX_MESSAGETYPE_QUIT, AVBOX_DISPATCH_UNICAST, thread) == NULL) {
		LOG_PRINT_ERROR("Could not send QUIT message");
	}

	pthread_join(thread->thread, NULL);

	free(thread);
}


/**
 * Pick the least busy thread.
 */
static struct avbox_thread *
avbox_thread_pick(void)
{
	struct avbox_thread *thread;
	struct avbox_thread *best_thread = NULL;
	struct timespec best_time = { 0xFFFFFF, 999L * 1000L * 1000L };

	/* Pick the first free thread. If one cannot be found
	 * pick the one with the shortest runtime */
	LIST_FOREACH(struct avbox_thread*, thread, &threads) {
		struct timespec now, runtime;
		if (timegte(&thread->stop_time, &thread->start_time)) {
			best_thread = thread;
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		runtime = timediff(&now, &thread->start_time);
		if (timelt(&runtime, &best_time)) {
			best_time = runtime;
			best_thread = thread;
		}
	}

	assert(best_thread != NULL);

	return best_thread;
}


/**
 * Delegate a function call to a thread.
 */
struct avbox_delegate *
avbox_thread_delegate(avbox_delegate_func func, void * arg)
{
	struct avbox_delegate *del;
	struct avbox_thread *thread = NULL;

	pthread_mutex_lock(&lock);

	/* pick the best thread */
	if ((thread = avbox_thread_pick()) == NULL) {
		DEBUG_VABORT("thread", "Could not pick thread: %s",
			strerror(errno));
	}

	/* create a new delegate */
	if ((del = avbox_delegate_new(func, arg)) == NULL) {
		assert(errno == ENOMEM);
		goto end;
	}

	/* dispatch it to the chosen thread */
	if (avbox_dispatch_sendmsg(-1, &thread->object,
		AVBOX_MESSAGETYPE_DELEGATE, AVBOX_DISPATCH_UNICAST, del) == NULL) {
		free(del);
		del = NULL;
		goto end;
	}

end:
	pthread_mutex_unlock(&lock);
	return del;
}


/**
 * Initialize the thread pool.
 */
int
avbox_thread_init(void)
{
	int i;
	struct avbox_thread *thread;

	LIST_INIT(&threads);

	for (i = 0; i < N_THREADS; i++) {
		if ((thread = avbox_thread_new()) == NULL) {
			LIST_FOREACH_SAFE(struct avbox_thread*, thread, &threads, {
				avbox_thread_destroy(thread);
			});
			return -1;
		}
	}

	return 0;
}


/**
 * Shutdown the thread pool.
 */
void
avbox_thread_shutdown(void)
{
	struct avbox_thread *thread;

	DEBUG_PRINT("thread", "Shutting down thread pool");

	while ((thread = LIST_TAIL(struct avbox_thread*, &threads)) != NULL) {
		avbox_thread_destroy(thread);
	}
}
