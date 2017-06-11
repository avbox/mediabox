#ifndef __AVBOX_DELEGATE_H__
#define __AVBOX_DELEGATE_H__


typedef void* (*avbox_delegate_fn)(void *args);


struct avbox_delegate;


/**
 * Free a delegate. This only needs to be called when you need
 * to destroy a delegate before it is executed.
 */
void
avbox_delegate_destroy(struct avbox_delegate * const delegate);


/**
 * Dettach a delegate call.
 */
void
avbox_delegate_dettach(struct avbox_delegate *delegate);


/**
 * Check if the delegate has finished executing.
 */
int
avbox_delegate_finished(struct avbox_delegate * const delegate);


/**
 * Wait for a delegate call to complete and
 * get the result.
 */
int
avbox_delegate_wait(struct avbox_delegate *delegate, void **result);


/**
 * Create new delegate.
 */
struct avbox_delegate *
avbox_delegate_new(avbox_delegate_fn func, void *arg);


/**
 * Execute and free delegate.
 */
void
avbox_delegate_execute(struct avbox_delegate * const del);


#endif
