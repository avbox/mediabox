#ifndef __AVBOX_THREAD_H__
#define __AVBOX_THREAD_H__
#include "delegate.h"


/**
 * Delegate a function call to a thread.
 */
struct avbox_delegate*
avbox_thread_delegate(avbox_delegate_func func, void * arg);


/**
 * Initialize the thread pool.
 */
int
avbox_thread_init(void);


/**
 * Shutdown the thread pool.
 */
void
avbox_thread_shutdown(void);


#endif
