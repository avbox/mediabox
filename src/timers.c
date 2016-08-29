#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "timers.h"
#include "linkedlist.h"
#include "debug.h"


/**
 * Timer structure
 */
LISTABLE_TYPE(mb_timer_state,
	struct timespec interval;
	struct timespec start_time;
	enum mbt_timer_flags flags;
	mbt_timer_callback callback;
	void *data;
);


LIST_DECLARE_STATIC(timers);


/**
 * mbt_cancel() -- Cancel a timer.
 */
int
mbt_cancel(int timer_id)
{
	DEBUG_PRINT("timers", "Called unimplemented cancel() function");
	return -ENOSYS;
}


/**
 * mbt_register() -- Register a timer.
 */
int
mbt_register(struct timespec *interval,
	enum mbt_timer_flags flags, mbt_timer_callback func, void *data)
{
	DEBUG_PRINT("timers", "Called unimplemented register() function");
	return -ENOSYS;
}


/**
 * mb_timers_init() -- Initialize the timers system.
 */
int
mbt_init(void)
{
	DEBUG_PRINT("timers", "Initializing timers system");
	LIST_INIT(&timers);
	return 0;
}


/**
 * mbt_shutdown() -- Shutdown the timers system.
 */
void
mbt_shutdown(void)
{
	DEBUG_PRINT("timers", "Shutting down timers system");
}

