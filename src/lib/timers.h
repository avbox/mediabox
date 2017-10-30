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


#ifndef __TIMERS_H__
#define __TIMERS_H__

#include <time.h>
#include "dispatch.h"


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
	enum avbox_timer_flags flags, struct avbox_object *obj, avbox_timer_callback func, void *data);


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
