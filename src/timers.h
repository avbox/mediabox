/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __TIMERS_H__
#define __TIMERS_H__

#include <time.h>


/**
 * Timer type enum
 */
enum avbox_timer_flags
{
	AVBOX_TIMER_TYPE_ONESHOT = 0,
	AVBOX_TIMER_TYPE_AUTORELOAD = 1,
	AVBOX_TIMER_MESSAGE = 2
};


struct avbox_timer_data
{
	int id;
	void *data;
};

/**
 * Timer callback result
 */
enum avbox_timer_result
{
	AVBOX_TIMER_CALLBACK_RESULT_CONTINUE,
	AVBOX_TIMER_CALLBACK_RESULT_STOP
};


/**
 * Timer callback function.
 */
typedef enum avbox_timer_result (*avbox_timer_callback)(int timer_id, void *data);


/**
 * Cancel a timer.
 */
int
avbox_timer_cancel(int timer_id);


/**
 * Register a timer.
 *
 * \param interval The interval at which the timer will fire
 * \param flags The timer flags
 * \param message_fd The message queue file descriptor where
 * 	the timeout messages will be sent to. This argument is ignored
 * 	unless the AVBOX_TIMER_MESSAGE flag is set.
 * \param func The callback function
 * \param data A pointer that will be passed to the callback function.
 */
int
avbox_timer_register(struct timespec *interval,
	enum avbox_timer_flags flags, int message_fd, avbox_timer_callback func, void *data);


/**
 * Initialize the timers system.
 */
int
avbox_timers_init(void);


/**
 * Shutdown the timers system.
 */
void
avbox_timers_shutdown(void);

#endif
