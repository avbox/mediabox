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
#include "thread.h"


#define N_THREADS (3)


LISTABLE_STRUCT(avbox_thread,
	int running;
	int busy;
	int flags;
#ifndef NDEBUG
	int no;
	int64_t jobs;
#endif
	pthread_t thread;
	struct timespec start_time;
	struct timespec stop_time;
	struct avbox_object *object;
	avbox_message_handler handler;
	void *context;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
);


static LIST workqueue_threads;
static pthread_mutex_t workqueue_mutex = PTHREAD_MUTEX_INITIALIZER;


/**
 * Thread message handler.
 */
static int
avbox_thread_msghandler(void * const context, struct avbox_message *msg)
{
	struct avbox_thread * const thread = context;

	/* if this is a realtime thread set it's priority */
	#if 0
	if (thread->flags & AVBOX_THREAD_REALTIME) {
		struct sched_param parms;
		parms.sched_priority = sched_get_priority_min(SCHED_FIFO);
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &parms) != 0) {
			LOG_PRINT_ERROR("Could not set the priority of realtime thread!");
		}
	}
	#endif

	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_DELEGATE:
	{
		struct avbox_delegate * const del =
			avbox_message_payload(msg);
		(void) clock_gettime(CLOCK_MONOTONIC, &thread->start_time);
		avbox_delegate_execute(del);
		(void) clock_gettime(CLOCK_MONOTONIC, &thread->stop_time);

#ifndef NDEBUG
		thread->jobs++;
#endif

		break;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		if (thread->handler != NULL) {
			return thread->handler(thread->context, msg);
		}
		break;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		avbox_dispatch_close();
		if (thread->handler != NULL) {
			return thread->handler(thread->context, msg);
		}
		break;
	}
	default:
		if (thread->handler != NULL) {
			return thread->handler(thread->context, msg);
		}
		LOG_VPRINT_ERROR("Unhandled message: 0x%x",
			avbox_message_id(msg));
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
	int quit = 0;
	struct avbox_thread * const thread = (struct avbox_thread*) arg;
	struct avbox_message * msg;

	/* initialize message dispatcher */
	if (avbox_dispatch_init() == -1) {
		LOG_VPRINT_ERROR("Could not initialize dispatch: %s",
			strerror(errno));
		goto end;
	}

	/* create dispatch object */
	if ((thread->object = avbox_object_new(
		avbox_thread_msghandler, thread)) == NULL) {
		LOG_VPRINT_ERROR("Could not create dispatch object: %s",
			strerror(errno));
		avbox_dispatch_shutdown();
		goto end;
	}

	/* initialize thread timers */
	clock_gettime(CLOCK_MONOTONIC, &thread->start_time);
	clock_gettime(CLOCK_MONOTONIC, &thread->stop_time);

	/* signal that we're up and running */
	pthread_mutex_lock(&thread->mutex);
	thread->running = 1;
	pthread_cond_signal(&thread->cond);
	pthread_mutex_unlock(&thread->mutex);

	/* run the message loop */
	while (!quit) {
		if ((msg = avbox_dispatch_getmsg()) == NULL) {
			switch (errno) {
			case EAGAIN: continue;
			case ESHUTDOWN:
				quit = 1;
				continue;
			default:
				DEBUG_VABORT("thread", "avbox_dispatch_getmsg() returned an unexpected error: %s (%i)",
					strerror(errno), errno);
			}
		}
		avbox_message_dispatch(msg);
	}

	/* cleanup */
	avbox_dispatch_shutdown();

	DEBUG_VPRINT("thread", "Thread #%i exited after %li jobs",
		thread->no, thread->jobs);

end:
	/* signal that we've exited */
	pthread_mutex_lock(&thread->mutex);
	pthread_cond_signal(&thread->cond);
	pthread_mutex_unlock(&thread->mutex);

	return NULL;
}


/**
 * Create a new thread.
 */
struct avbox_thread *
avbox_thread_new(avbox_message_handler handler, void * const context, int flags)
{
	struct avbox_thread *thread;

	/* allocate memory */
	if ((thread = malloc(sizeof(struct avbox_thread))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	if (pthread_mutex_init(&thread->mutex, NULL) != 0 ||
		pthread_cond_init(&thread->cond, NULL) != 0) {
		abort();
	}

	pthread_mutex_lock(&thread->mutex);

#ifndef NDEBUG
	thread->no = -1;
	thread->jobs = 0;
#endif
	thread->handler = handler;
	thread->context = context;
	thread->flags = flags;
	thread->running = 0;
	if (pthread_create(&thread->thread, NULL, avbox_thread_run, thread) != 0) {
		free(thread);
		return NULL;
	}
	pthread_cond_wait(&thread->cond, &thread->mutex);
	pthread_mutex_unlock(&thread->mutex);

	/* check that the thread started successfully */
	if (!thread->running) {
		free(thread);
		thread = NULL;
	} else {
	}

	DEBUG_VPRINT("thread", "Thread %p started", thread);

	return thread;
}


/**
 * Delegates a function call to a thread.
 */
struct avbox_delegate *
avbox_thread_delegate(struct avbox_thread * const thread,
	avbox_delegate_fn func, void * const arg)
{
	struct avbox_delegate *del = NULL;

	if ((del = avbox_delegate_new(func, arg)) == NULL) {
		assert(errno == ENOMEM);
		goto end;
	}

	/* dispatch it to the chosen thread */
	if (avbox_object_sendmsg(&thread->object,
		AVBOX_MESSAGETYPE_DELEGATE, AVBOX_DISPATCH_UNICAST, del) == NULL) {
		free(del);
		del = NULL;
		goto end;
	}
end:
	return del;
}


/**
 * Get the underlying object.
 */
struct avbox_object *
avbox_thread_object(const struct avbox_thread * const thread)
{
	ASSERT(thread != NULL);
	return thread->object;
}


/**
 * Destroy a thread.
 */
void
avbox_thread_destroy(struct avbox_thread * const thread)
{
	DEBUG_VPRINT("thread", "Shutting down thread #%i",
		thread->no);

	assert(thread != NULL);


	/* Destroy the thread object and wait for it
	 * to exit */
	avbox_object_destroy(thread->object);
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
	LIST_FOREACH(struct avbox_thread*, thread, &workqueue_threads) {
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
 * Delegate a function call to the work queue.
 */
struct avbox_delegate *
avbox_workqueue_delegate(avbox_delegate_fn func, void * arg)
{
	struct avbox_delegate *del;
	struct avbox_thread *thread = NULL;

	pthread_mutex_lock(&workqueue_mutex);

	/* pick the best thread */
	if ((thread = avbox_thread_pick()) == NULL) {
		DEBUG_VABORT("thread", "Could not pick thread: %s",
			strerror(errno));
	}

	/* create a new delegate */
	if ((del = avbox_thread_delegate(thread, func, arg)) == NULL) {
		goto end;
	}
end:
	pthread_mutex_unlock(&workqueue_mutex);
	return del;
}


/**
 * Initialize a workqueue thread.
 */
static void *
avbox_workqueue_thread_init(void *arg)
{
	struct avbox_thread * const thread = arg;
#ifndef NDEBUG
	DEBUG_SET_THREAD_NAME("avbox-worker");
	DEBUG_VPRINT("thread", "Thread #%i started", thread->no);
	static int thread_no = 0;
	thread->no = thread_no++;
#endif
	return thread;
}


/**
 * Initialize the thread pool.
 */
int
avbox_workqueue_init(void)
{
	int i;
	struct avbox_thread *thread, *thread_check;
	struct avbox_delegate *initfunc;
	LIST_INIT(&workqueue_threads);

	for (i = 0; i < N_THREADS; i++) {
		if ((thread = avbox_thread_new(NULL, NULL, 0)) == NULL ||
			(initfunc = avbox_thread_delegate(thread, avbox_workqueue_thread_init, thread)) == NULL) {
			if (thread != NULL) {
				avbox_thread_destroy(thread);
			}
			LIST_FOREACH_SAFE(struct avbox_thread*, thread, &workqueue_threads, {
				avbox_thread_destroy(thread);
			});
			return -1;
		}
		avbox_delegate_wait(initfunc, (void**) &thread_check);
		if (thread_check != thread) {
			abort();
		}
		LIST_APPEND(&workqueue_threads, thread);
	}

	return 0;
}


/**
 * Shutdown the thread pool.
 */
void
avbox_workqueue_shutdown(void)
{
	struct avbox_thread *thread;

	DEBUG_PRINT("thread", "Shutting down thread pool");

	while ((thread = LIST_TAIL(struct avbox_thread*, &workqueue_threads)) != NULL) {
		LIST_REMOVE(thread);
		avbox_thread_destroy(thread);
	}
}
