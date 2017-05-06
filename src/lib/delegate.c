#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include "delegate.h"

/**
 * Delegate call.
 */
struct avbox_delegate
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	avbox_delegate_func func;
	void *arg;
	void *result;
	int finished;
	int dettached;
};



/**
 * Dettach a delegate call.
 */
void
avbox_delegate_dettach(struct avbox_delegate *delegate)
{
	assert(delegate != NULL);
	pthread_mutex_lock(&delegate->lock);
	if (delegate->finished) {
		free(delegate);
		return;
	} else {
		delegate->dettached = 1;
	}
	pthread_mutex_unlock(&delegate->lock);
}


/**
 * Check if the delegate has finished executing.
 */
int
avbox_delegate_finished(struct avbox_delegate * const delegate)
{
	assert(delegate != NULL);
	return delegate->finished;
}


/**
 * Wait for a delegate call to complete and
 * get the result.
 */
int
avbox_delegate_wait(struct avbox_delegate *delegate, void **result)
{
	assert(delegate != NULL);
	pthread_mutex_lock(&delegate->lock);
	assert(!delegate->dettached);
	if (!delegate->finished) {
		pthread_cond_wait(&delegate->cond, &delegate->lock);
	}
	pthread_mutex_unlock(&delegate->lock);
	if (result != NULL) {
		*result = delegate->result;
	}
	free(delegate);
	return 0;
}


/**
 * Dispatch a function call to the main thread.
 */
struct avbox_delegate*
avbox_delegate_new(avbox_delegate_func func, void *arg)
{
	struct avbox_delegate * del;

	/* allocate memory */
	if ((del = malloc(sizeof(struct avbox_delegate))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	/* initialize locking primitives */
	if (pthread_mutex_init(&del->lock, NULL) != 0 ||
		pthread_cond_init(&del->cond, NULL) != 0) {
		errno = EINVAL;
		free(del);
		return NULL;
	}

	/* initialize call */
	del->func = func;
	del->arg = arg;
	del->result = NULL;
	del->finished = 0;
	del->dettached = 0;

	return del;
}


/**
 * Execute delegate call.
 */
void
avbox_delegate_execute(struct avbox_delegate * const del)
{
	/* run delegated function */
	del->result = del->func(del->arg);

	/* update state and notify waiter */
	pthread_mutex_lock(&del->lock);
	del->finished = 1;
	if (del->dettached) {
		pthread_mutex_unlock(&del->lock);
		free(del);
	} else {
		pthread_cond_signal(&del->cond);
		pthread_mutex_unlock(&del->lock);
	}
}


