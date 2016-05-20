#ifndef __MB_INPUT_H__
#define __MB_INPUT_H__

#include <pthread.h>

#include "pipe_util.h"
#include "linkedlist.h"

#define mbi_event_send(handle, e) \
{ \
	const mbi_event __e = e; \
	pthread_mutex_lock(&handle->lock); \
	write_or_die(handle->writefd, &__e, sizeof(mbi_event)); \
	pthread_mutex_unlock(&handle->lock); \
}

typedef enum
{
	MBI_EVENT_PLAY,
	MBI_EVENT_STOP,
	MBI_EVENT_ENTER,
	MBI_EVENT_EXIT
}
mbi_event;

struct mbi
{
	int readfd;
	int writefd;
	pthread_mutex_t lock;
	pthread_mutex_t sinks_lock;
	LIST_DECLARE(sinks);
};

int
mbi_loop(struct mbi *inst);

/**
 * mbi_grab_input() -- Returns a file descriptor to a pipe where
 * all input events will be sent until the file descriptor is closed,
 * in which case the prior descriptor is closed or until mbi_grab_input()
 * is called again
 */
int
mbi_grab_input(struct mbi *inst);

struct mbi*
mbi_init();

void
mbi_destroy();

#endif

