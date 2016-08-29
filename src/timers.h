#ifndef __TIMERS_H__
#define __TIMERS_H__

#include <time.h>


/**
 * enum mbt_type -- Timer type enum
 */
enum mbt_timer_flags
{
	MB_TIMER_TYPE_ONESHOT = 0,
	MB_TIMER_TYPE_AUTORELOAD = 1,
	MB_TIMER_AUTOFREE = 2
};


/**
 * enum mbt_result -- Timer callback result
 */
enum mbt_result
{
	MB_TIMER_CALLBACK_RESULT_CONTINUE,
	MB_TIMER_CALLBACK_RESULT_STOP
};


/**
 * mbt_timer_callback -- Timer callback function.
 */
typedef enum mbt_result (*mbt_timer_callback)(int timer_id, void *data);


/**
 * mbt_cancel() -- Cancel a timer.
 */
int
mbt_cancel(int timer_id);


/**
 * mbt_register() -- Register a timer.
 */
int
mbt_register(struct timespec *interval,
	enum mbt_timer_flags flags, mbt_timer_callback func, void *data);


/**
 * mb_timers_init() -- Initialize the timers system.
 */
int
mbt_init(void);


/**
 * mbt_shutdown() -- Shutdown the timers system.
 */
void
mbt_shutdown(void);

#endif
