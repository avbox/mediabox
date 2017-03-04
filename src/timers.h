/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

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
	MB_TIMER_MESSAGE = 2
};


struct mbt_timer_data
{
	int id;
	void *data;
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
 * Register a timer.
 *
 * \param interval The interval at which the timer will fire
 * \param flags The timer flags
 * \param message_fd The message queue file descriptor where
 * 	the timeout messages will be sent to. This argument is ignored
 * 	unless the MB_TIMER_MESSAGE flag is set.
 * \param func The callback function
 * \param data A pointer that will be passed to the callback function.
 */
int
mbt_register(struct timespec *interval,
	enum mbt_timer_flags flags, int message_fd, mbt_timer_callback func, void *data);


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
