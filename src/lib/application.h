#ifndef __AVBOX_APPLICATION_H__
#define __AVBOX_APPLICATION_H__
#include "delegate.h"


/**
 * Delegate a function call to the application's thread.
 */
struct avbox_delegate*
avbox_application_delegate(avbox_delegate_func func, void *arg);


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
