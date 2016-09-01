#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "timers.h"
#include "time_util.h"
#include "linkedlist.h"
#include "debug.h"


/**
 * Timer structure
 */
LISTABLE_TYPE(mb_timer_state,
	int id;
	struct timespec interval;
	struct timespec value;
	enum mbt_timer_flags flags;
	mbt_timer_callback callback;
	void *data;
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
mbt_timers_thread(void *arg)
{
	struct timespec last_sleep;
	struct timespec elapsed;
	struct timespec now;
	struct timespec sleeptime;
	struct timespec abs;
	mb_timer_state *tmr;
	enum mbt_result ret;

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
		LIST_FOREACH_SAFE(mb_timer_state*, tmr, &timers, {
			if (timelte(&tmr->value, &elapsed)) {
				/* the timer has elapsed so invoke the callback */
				if (tmr->callback != NULL) {
					ret = tmr->callback(tmr->id, tmr->data);
				} else {
					ret = MB_TIMER_CALLBACK_RESULT_CONTINUE;
				}
				if (tmr->flags & MB_TIMER_TYPE_AUTORELOAD) {
					/* if this is an autoreload timer reload it */
					if (ret == MB_TIMER_CALLBACK_RESULT_CONTINUE) {
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

		abs = abstime();
		sleeptime = timeadd(&abs, &sleeptime);
		last_sleep = now;
		pthread_cond_timedwait(&timers_signal, &timers_lock, &sleeptime);
	}

	pthread_mutex_unlock(&timers_lock);

	DEBUG_PRINT("timers", "Timers thread exiting");

	return NULL;
}


static int
mbt_getnextid(void)
{
	return nextid++;
}


/**
 * mbt_cancel() -- Cancel a timer.
 */
int
mbt_cancel(int timer_id)
{
	mb_timer_state *tmr;
	int ret = -1;

	DEBUG_VPRINT("timers", "Cancelling timer id %i", timer_id);

	pthread_mutex_lock(&timers_lock);

	LIST_FOREACH_SAFE(mb_timer_state*, tmr, &timers, {
		if (tmr->id == timer_id) {
			LIST_REMOVE(tmr);
			free(tmr);
			ret = 1;
			break;
		}
	});

	pthread_mutex_unlock(&timers_lock);

	return ret;
}


/**
 * mbt_register() -- Register a timer.
 */
int
mbt_register(struct timespec *interval,
	enum mbt_timer_flags flags, mbt_timer_callback func, void *data)
{
	mb_timer_state *timer;

	DEBUG_PRINT("timers", "Registering timer");

	/* allocate and initialize timer entry */
	if ((timer = malloc(sizeof(mb_timer_state))) == NULL) {
		fprintf(stderr, "timers: Could not allocate timer. Out of memory\n");
		return -1;
	}
	timer->interval = *interval;
	timer->value = *interval;
	timer->callback = func;
	timer->data = data;
	timer->flags = flags;

	DEBUG_VPRINT("timers", "Adding timer (%lis%linsecs)",
		timer->value.tv_sec, timer->value.tv_nsec);

	/* add entry to list */
	pthread_mutex_lock(&timers_lock);
	timer->id = mbt_getnextid();
	LIST_ADD(&timers, timer);
	pthread_mutex_unlock(&timers_lock);

	/* wake the timers thread */
	pthread_cond_signal(&timers_signal);

	return 0;
}


#if 0
static enum mbt_result
mbt_test_timer_handler(int id, void *data)
{
	(void) data;
	DEBUG_VPRINT("timers", "Test timer (id=%i) expired", id);
	return MB_TIMER_CALLBACK_RESULT_CONTINUE;
}
#endif


/**
 * mb_timers_init() -- Initialize the timers system.
 */
int
mbt_init(void)
{
	DEBUG_PRINT("timers", "Initializing timers system");

	LIST_INIT(&timers);

	if (pthread_create(&timers_thread, NULL, mbt_timers_thread, NULL) != 0) {
		fprintf(stderr, "timers: Could not start thread\n");
		return -1;
	}

	/* register a test timer */
	#if 0
	struct timespec tv;
	tv.tv_sec = 1;
	tv.tv_nsec = 0;
	if (mbt_register(&tv, MB_TIMER_TYPE_ONESHOT, &mbt_test_timer_handler, NULL) == -1) {
		DEBUG_PRINT("timers", "Could not register test timer");
	}
	#endif

	return 0;
}


/**
 * mbt_shutdown() -- Shutdown the timers system.
 */
void
mbt_shutdown(void)
{
	mb_timer_state *tmr;

	DEBUG_PRINT("timers", "Shutting down timers system");

	quit = 1;
	pthread_cond_signal(&timers_signal);
	pthread_join(timers_thread, NULL);

	LIST_FOREACH_SAFE(mb_timer_state*, tmr, &timers, {
		LIST_REMOVE(tmr);
		free(tmr);
	});
}

