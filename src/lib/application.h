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
 * Dispatch the next message in the thread's
 * queue.
 */
int
avbox_application_doevents();


/**
 * Quit the application.
 */
int
avbox_application_quit(const int status);


#endif
