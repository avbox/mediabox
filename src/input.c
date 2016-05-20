#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#include "input.h"
#include "input-directfb.h"
#include "input-tcp.h"
#include "linkedlist.h"

LISTABLE_TYPE(mpi_sink,
	int readfd;
	int writefd;
);

int
mbi_loop(struct mbi *inst)
{
	int runloop = 1;
	mbi_event e;

	while (runloop) {
		/* read the next event */
		read_or_die(inst->readfd, &e, sizeof(mbi_event));

		/* process event */
		switch (e) {
		case MBI_EVENT_PLAY:
			fprintf(stderr, "play event received\n");
			break;
		case MBI_EVENT_STOP:
		case MBI_EVENT_ENTER:
			break;
		case MBI_EVENT_EXIT:
			runloop = 0;
			break;
		default:
			abort();
			break;
		}
	}
	return 0;
}

/**
 * mbi_grab_input() -- Returns a file descriptor to a pipe where
 * all input events will be sent until the file descriptor is closed,
 * in which case the prior descriptor is closed or until mbi_grab_input()
 * is called again
 */
int
mbi_grab_input(struct mbi *inst)
{
	int inputfd[2];
	mpi_sink *input_sink;

	input_sink = malloc(sizeof(mpi_sink));
	if (input_sink == NULL) {
		fprintf(stderr, "mbi_grab_input() -- out of memory\n");
		return -1;
	}

	if (pipe(inputfd) == -1) {
		fprintf(stderr, "mbi_grab_input() -- pipe() failed\n");
		free(input_sink);
		return -1;
	}

	/* save file descriptors */
	input_sink->readfd = inputfd[0];
	input_sink->writefd = inputfd[1];

	/* add sink to stack */
	pthread_mutex_lock(&inst->sinks_lock);
	LIST_APPEND(&inst->sinks, input_sink);
	pthread_mutex_unlock(&inst->sinks_lock);

	return input_sink->readfd;
}

/**
 * mbi_init() -- Initialize input subsystem
 */
struct mbi*
mbi_init()
{
	struct mbi *inst;
	int event_pipe[2];

	inst = malloc(sizeof(struct mbi));
	if (inst == NULL) {
		fprintf(stderr, "mbi_init() -- out of memory\n");
		return NULL;
	}

	/* initialize mutex */
	if (pthread_mutex_init(&inst->lock, NULL) != 0) {
		fprintf(stderr, "mbi_init() -- pthread_mutex_init() failed.\n");
		goto err;
	}

	/* create event pipes */
	if (pipe(event_pipe) == -1) {
		fprintf(stderr, "mbi_init() -- pipe() failed\n");
		goto err;
	}

	/* save pipe endpoints */
	inst->readfd = event_pipe[0];
	inst->writefd = event_pipe[1];

	/* initialize sinks stack */
	LIST_INIT(&inst->sinks);

	/* initialize directfb input driver */
	if (mbi_directfb_init(inst) == -1) {
		fprintf(stderr, "mbi_directfb_init() failed\n");
		goto err;
	}

	/* initialize the tcp remote input driver */
	if (mbi_tcp_init(inst) == -1) {
		fprintf(stderr, "mbi_tcp_init() failed\n");
		goto err;
	}

	return inst;
err:
	free(inst);
	return NULL;
}

void
mbi_destroy(struct mbi *inst)
{
	assert(inst != NULL);
	close(inst->writefd);
	close(inst->readfd);
	free(inst);
}

