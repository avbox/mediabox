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


#ifndef __AVBOX_APPLICATION_H__
#define __AVBOX_APPLICATION_H__
#include "delegate.h"


/**
 * Application events.
 */
#define AVBOX_APPEVENT_NONE	(0)
#define AVBOX_APPEVENT_QUIT	(1)


/**
 * Function to handle application events.
 */
typedef int (*avbox_application_eventhandler)(void *context, int event);



/**
 * Delegate a function call to the application's thread.
 */
struct avbox_delegate*
avbox_application_delegate(avbox_delegate_fn func, void *arg);


/**
 * Subscribe to application events. This function can only
 * be called from the main thread.
 */
int
avbox_application_subscribe(avbox_application_eventhandler handler, void *context);


/**
 * Unsubscribe from application events. This function can only
 * be called from the application thread.
 */
int
avbox_application_unsubscribe(avbox_application_eventhandler handler, void *context);


/**
 * Gets the command line arguments
 */
const char **
avbox_application_args(int * const argc);


/**
 * Initialize application.
 */
int
avbox_application_init(int argc, char **argv, const char *logfile);


/**
 * Application main loop.
 */
int
avbox_application_run(void);

/**
 * Gets the application's main thread queue.
 */
struct avbox_queue*
avbox_application_queue();


/**
 * Get the main thread's object.
 */
struct avbox_object*
avbox_application_object();


/**
 * Quit the application.
 */
int
avbox_application_quit(const int status);


#endif
