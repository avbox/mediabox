/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

#include "timers.h"
#include "time_util.h"
#include "linkedlist.h"
#include "debug.h"
#include "input.h"


/**
 * Timer structure
 */
LISTABLE_TYPE(avbox_timer_state,
	struct avbox_timer_data public;
	struct timespec interval;
	struct timespec value;
	enum avbox_timer_flags flags;
	int message_fd;
	avbox_timer_callback callback;
);


LIST_DECLARE_STATIC(timers);
static int quit = 0;
static pthread_mutex_t timers_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timers_signal = PTHREAD_COND_INITIALIZER;
static pthread_t timers_thread;
static int nextid = 1;


/**
 * mbt_timers_thread() -- Waits until the next timer should elapsed,
 * processes it, and goes back to sleep
 */
void *
avbox_timers_thread(void *arg)
{
	struct timespec last_sleep, elapsed, now, sleeptime;
	avbox_timer_state *tmr;
	enum avbox_timer_result ret;

	(void) arg;

	DEBUG_PRINT("timers", "Timers system running");
	MB_DEBUG_SET_THREAD_NAME("timers");

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
		LIST_FOREACH_SAFE(avbox_timer_state*, tmr, &timers, {
			if (timelte(&tmr->value, &elapsed)) {
				/* the timer has elapsed so invoke the callback */
				if (tmr->callback != NULL) {
					ret = tmr->callback(tmr->public.id, tmr->public.data);
				} else {
					ret = AVBOX_TIMER_CALLBACK_RESULT_CONTINUE;
				}
				if (tmr->flags & AVBOX_TIMER_MESSAGE) {
					if (tmr->message_fd != -1) {
						avbox_input_sendmessage(tmr->message_fd, MBI_EVENT_TIMER,
							&tmr->public, sizeof(struct avbox_timer_data));
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
						free(tmr);
					}
				} else {
					/* remove the timer */
					LIST_REMOVE(tmr);
					free(tmr);
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
int
avbox_timer_cancel(int timer_id)
{
	avbox_timer_state *tmr;
	int ret = -1;

	DEBUG_VPRINT("timers", "Cancelling timer id %i", timer_id);

	pthread_mutex_lock(&timers_lock);

	LIST_FOREACH_SAFE(avbox_timer_state*, tmr, &timers, {
		if (tmr->public.id == timer_id) {
			LIST_REMOVE(tmr);
			free(tmr);
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
int
avbox_timer_register(struct timespec *interval,
	enum avbox_timer_flags flags, int message_fd, avbox_timer_callback func, void *data)
{
	avbox_timer_state *timer;

	DEBUG_PRINT("timers", "Registering timer");

	assert(message_fd == -1 || message_fd > 2);

	/* allocate and initialize timer entry */
	if ((timer = malloc(sizeof(avbox_timer_state))) == NULL) {
		fprintf(stderr, "timers: Could not allocate timer. Out of memory\n");
		return -1;
	}
	timer->interval = *interval;
	timer->value = *interval;
	timer->message_fd = message_fd;
	timer->callback = func;
	timer->public.data = data;
	timer->flags = flags;

	DEBUG_VPRINT("timers", "Adding timer (%lis%linsecs)",
		timer->value.tv_sec, timer->value.tv_nsec);

	/* add entry to list */
	pthread_mutex_lock(&timers_lock);
	timer->public.id = avbox_timers_getnextid();
	LIST_ADD(&timers, timer);
	pthread_mutex_unlock(&timers_lock);

	/* wake the timers thread */
	pthread_cond_signal(&timers_signal);

	return timer->public.id;
}


/**
 * Initialize the timers system.
 */
int
avbox_timers_init(void)
{
	DEBUG_PRINT("timers", "Initializing timers system");

	LIST_INIT(&timers);

	if (pthread_create(&timers_thread, NULL, avbox_timers_thread, NULL) != 0) {
		fprintf(stderr, "timers: Could not start thread\n");
		return -1;
	}

	return 0;
}


/**
 * Shutdown the timers system.
 */
void
avbox_timers_shutdown(void)
{
	avbox_timer_state *tmr;

	DEBUG_PRINT("timers", "Shutting down timers system");

	quit = 1;
	pthread_cond_signal(&timers_signal);
	pthread_join(timers_thread, NULL);

	LIST_FOREACH_SAFE(avbox_timer_state*, tmr, &timers, {
		LIST_REMOVE(tmr);
		free(tmr);
	});
}

