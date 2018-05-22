#ifndef __SYNCARGS__
#define __SYNCARGS__

#include <stdlib.h>
#include <pthread.h>


/**
 * Sync arg structure. All fields are private and should not
 * be accessed outside this file.
 */
struct avbox_syncarg
{
	pthread_mutex_t __mutex;
	pthread_cond_t __cond;
	void *__data;
};


/**
 * Initialize a syncarg structure.
 */
static inline void
avbox_syncarg_init(struct avbox_syncarg * const inst, void * const data)
{
	if (pthread_mutex_init(&inst->__mutex, NULL) != 0 ||
		pthread_cond_init(&inst->__cond, NULL) != 0) {
		abort();
	}
	inst->__data = data;
	pthread_mutex_lock(&inst->__mutex);
}


/**
 * Get the argument data.
 */
static inline void *
avbox_syncarg_data(struct avbox_syncarg * const inst)
{
	return inst->__data;
}


/**
 * Wait for a syncarg response.
 */
static inline void *
avbox_syncarg_wait(struct avbox_syncarg * const inst)
{
	void *result;
	pthread_cond_wait(&inst->__cond, &inst->__mutex);
	result = inst->__data;
	inst->__data = NULL;
	pthread_mutex_unlock(&inst->__mutex);
	return result;
}


/**
 * Wake the waiting thread and set the result.
 */
static inline void
avbox_syncarg_return(struct avbox_syncarg * const inst, void * const result)
{
	pthread_mutex_lock(&inst->__mutex);
	inst->__data = result;
	pthread_cond_signal(&inst->__cond);
	pthread_mutex_unlock(&inst->__mutex);
}


#endif
