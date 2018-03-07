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
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>

#define LOG_MODULE "timers"

#include "timers.h"
#include "time_util.h"
#include "linkedlist.h"
#include "debug.h"
#include "input.h"
#include "dispatch.h"


/**
 * Timer structure
 */
LISTABLE_STRUCT(avbox_timer_state,
	struct avbox_timer_data public;
	struct timespec interval;
	struct timespec value;
	enum avbox_timer_flags flags;
	struct avbox_object *message_object;
	avbox_timer_callback callback;
);


static LIST timers;
static int quit = 0;
static pthread_mutex_t timers_lock;
static pthread_cond_t timers_signal = PTHREAD_COND_INITIALIZER;
static pthread_t timers_thread;
static int nextid = 1;

static LIST timer_pool;
static LIST timer_data_pool;
static pthread_mutex_t timer_pool_lock;
static pthread_mutex_t timer_data_pool_lock;


static struct avbox_timer_state*
acquire_timer()
{
	struct avbox_timer_state *tmr;
	pthread_mutex_lock(&timer_pool_lock);
	tmr = LIST_TAIL(struct avbox_timer_state*, &timer_pool);
	if (UNLIKELY(tmr == NULL)) {
		if ((tmr = malloc(sizeof(struct avbox_timer_state))) == NULL) {
			ASSERT(errno == ENOMEM);
			pthread_mutex_unlock(&timer_pool_lock);
			return NULL;
		}
		LOG_PRINT_INFO("Allocated timer state!");
	} else {
		LIST_REMOVE(tmr);
	}
	pthread_mutex_unlock(&timer_pool_lock);
	return tmr;
}


static void
release_timer(struct avbox_timer_state * const tmr)
{
	pthread_mutex_lock(&timer_pool_lock);
	LIST_ADD(&timer_pool, tmr);
	pthread_mutex_unlock(&timer_pool_lock);
}


static struct avbox_timer_data*
acquire_payload()
{
	struct avbox_timer_data *td;
	pthread_mutex_lock(&timer_data_pool_lock);
	td = LIST_TAIL(struct avbox_timer_data*, &timer_data_pool);
	if (UNLIKELY(td == NULL)) {
		if ((td = malloc(sizeof(struct avbox_timer_data))) == NULL) {
			ASSERT(errno == ENOMEM);
			pthread_mutex_unlock(&timer_data_pool_lock);
			return NULL;
		}
		LOG_PRINT_INFO("Allocated AVPacket!");
	} else {
		LIST_REMOVE(td);
	}
	pthread_mutex_unlock(&timer_data_pool_lock);
	return td;
}


EXPORT void
avbox_timers_releasepayload(struct avbox_timer_data * const td)
{
	pthread_mutex_lock(&timer_data_pool_lock);
	LIST_ADD(&timer_data_pool, td);
	pthread_mutex_unlock(&timer_data_pool_lock);
}


/**
 * Waits until the next timer should elapsed,
 * processes it, and goes back to sleep
 */
static void *
avbox_timers_thread(void *arg)
{
	struct timespec last_sleep, elapsed, now, sleeptime;
	struct avbox_timer_state *tmr;
	enum avbox_timer_result ret;

	(void) arg;

	DEBUG_PRINT("timers", "Timers system running");
	DEBUG_SET_THREAD_NAME("avbox-timers");

#ifdef ENABLE_REALTIME
	struct sched_param parms;

	/* this needs to be above any CPU bound threads (ie. libtorrent)
	 * but bellow everything else as we depend on timers to get out
	 * from underrun so we can't allow them to be starved */
	parms.sched_priority = sched_get_priority_min(SCHED_FIFO) + 10;
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &parms) != 0) {
		LOG_PRINT_ERROR("Could not set main thread priority");
	}
#endif

	clock_gettime(CLOCK_MONOTONIC, &last_sleep);

	pthread_mutex_lock(&timers_lock);

	while (!quit) {

		/* this is the maximum time that we're allowed to sleep */
		sleeptime.tv_sec = 10;
		sleeptime.tv_nsec = 0;

		/* calculate time elapsed since last sleep */
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = timediff(&last_sleep, &now);

		/* iterate through all timers */
		LIST_FOREACH_SAFE(struct avbox_timer_state*, tmr, &timers, {
			if (timelte(&tmr->value, &elapsed)) {
				/* the timer has elapsed so invoke the callback */
				if (tmr->callback != NULL) {
					ret = tmr->callback(tmr->public.id, tmr->public.data);
				} else {
					ret = AVBOX_TIMER_CALLBACK_RESULT_CONTINUE;
				}
				if (tmr->flags & AVBOX_TIMER_MESSAGE) {
					if (tmr->message_object != NULL) {
						struct avbox_timer_data *payload;
						if ((payload = acquire_payload()) == NULL) {
							LOG_PRINT_ERROR("Could not send TIMER message: Out of memory");
						} else {
							memcpy(payload, &tmr->public, sizeof(struct avbox_timer_data));
							if (avbox_object_sendmsg(&tmr->message_object,
								AVBOX_MESSAGETYPE_TIMER, AVBOX_DISPATCH_UNICAST, payload) == NULL) {
								LOG_VPRINT_ERROR("Could not send notification message: %s",
									strerror(errno));
								avbox_timers_releasepayload(payload);
							}
						}
					}
				}
				if (tmr->flags & AVBOX_TIMER_TYPE_AUTORELOAD) {
					/* if this is an autoreload timer reload it */
					if (ret == AVBOX_TIMER_CALLBACK_RESULT_CONTINUE) {
						tmr->value = tmr->interval;
						if (timelt(&tmr->value, &sleeptime)) {
							sleeptime = tmr->interval;
						}
					} else {
						LIST_REMOVE(tmr);
						release_timer(tmr);
					}
				} else {
					/* remove the timer */
					LIST_REMOVE(tmr);
					release_timer(tmr);
				}
			} else {
				/* decrement the timer */
				tmr->value = timediff(&elapsed, &tmr->value);
				if (timelt(&tmr->value, &sleeptime)) {
					sleeptime = tmr->value;
				}
			}
		});

		delay2abstime(&sleeptime);
		last_sleep = now;
		pthread_cond_timedwait(&timers_signal, &timers_lock, &sleeptime);
	}

	pthread_mutex_unlock(&timers_lock);

	DEBUG_PRINT("timers", "Timers thread exiting");

	return NULL;
}


static int
avbox_timers_getnextid(void)
{
	return nextid++;
}


/**
 * Cancel a timer.
 */
EXPORT int
avbox_timer_cancel(int timer_id)
{
	struct avbox_timer_state *tmr;
	int ret = -1;

	/* DEBUG_VPRINT("timers", "Cancelling timer id %i", timer_id); */

	pthread_mutex_lock(&timers_lock);

	LIST_FOREACH_SAFE(struct avbox_timer_state*, tmr, &timers, {
		if (tmr->public.id == timer_id) {
			LIST_REMOVE(tmr);
			release_timer(tmr);
			ret = 0;
			break;
		}
	});

	pthread_mutex_unlock(&timers_lock);

	return ret;
}


/**
 * Register a timer.
 */
EXPORT int
avbox_timer_register(struct timespec *interval,
	enum avbox_timer_flags flags, struct avbox_object *msgobj, avbox_timer_callback func, void *data)
{
	int ret = -1;
	struct avbox_timer_state *timer;

	/* DEBUG_PRINT("timers", "Registering timer"); */

	/* allocate and initialize timer entry */
	if ((timer = acquire_timer()) == NULL) {
		LOG_PRINT_ERROR("Could not acquire timer. Out of memory!");
		return -1;
	}
	memset(timer, 0, sizeof(struct avbox_timer_state));
	timer->interval = *interval;
	timer->value = *interval;
	timer->message_object = msgobj;
	timer->callback = func;
	timer->public.data = data;
	timer->flags = flags;

	/* DEBUG_VPRINT("timers", "Adding timer (%lis%linsecs)",
		timer->value.tv_sec, timer->value.tv_nsec); */

	/* add entry to list */
	pthread_mutex_lock(&timers_lock);
	ret = timer->public.id = avbox_timers_getnextid();
	LIST_ADD(&timers, timer);
	pthread_mutex_unlock(&timers_lock);

	/* wake the timers thread */
	pthread_cond_signal(&timers_signal);

	return ret;
}


/**
 * Initialize the timers system.
 */
INTERNAL int
avbox_timers_init(void)
{
	pthread_mutexattr_t prio_inherit;

	DEBUG_PRINT("timers", "Initializing timers system");

	LIST_INIT(&timers);
	LIST_INIT(&timer_pool);
	LIST_INIT(&timer_data_pool);

	pthread_mutexattr_init(&prio_inherit);
	pthread_mutexattr_setprotocol(&prio_inherit, PTHREAD_PRIO_INHERIT);
	if (pthread_mutex_init(&timers_lock, &prio_inherit) != 0 ||
		pthread_mutex_init(&timer_pool_lock, &prio_inherit) != 0 ||
		pthread_mutex_init(&timer_data_pool_lock, &prio_inherit) != 0) {
		ABORT("Could not initialize mutexes!");
	}
	pthread_mutexattr_destroy(&prio_inherit);

	if (pthread_create(&timers_thread, NULL, avbox_timers_thread, NULL) != 0) {
		fprintf(stderr, "timers: Could not start thread\n");
		return -1;
	}

	return 0;
}


/**
 * Shutdown the timers system.
 */
INTERNAL void
avbox_timers_shutdown(void)
{
	struct avbox_timer_state *tmr;

	DEBUG_PRINT("timers", "Shutting down timers system");

	quit = 1;
	pthread_cond_signal(&timers_signal);
	pthread_join(timers_thread, NULL);

	LIST_FOREACH_SAFE(struct avbox_timer_state*, tmr, &timers, {
		LIST_REMOVE(tmr);
		release_timer(tmr);
	});
}
