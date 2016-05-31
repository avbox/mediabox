#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/signal.h>

#include "input.h"
#include "input-directfb.h"
#include "input-tcp.h"
#include "linkedlist.h"

LISTABLE_TYPE(mbi_sink,
	int readfd;
	int writefd;
);

static int readfd;
static int writefd;
static pthread_t input_loop_thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sinks_lock = PTHREAD_MUTEX_INITIALIZER;

LIST_DECLARE_STATIC(sinks);

static void*
mbi_loop(void *arg)
{
	mbi_event e;
	mbi_sink *sink;
	int runloop = 1, dispatched;

	(void) arg;

	while (runloop) {
		dispatched = 0;

		/* read the next event */
		read_or_die(readfd, &e, sizeof(mbi_event));

		if (e == MBI_EVENT_EXIT) {
			fprintf(stderr, "mbi: EXIT command received\n");
			break;
		}

		do {
			int ret;
			sink = LIST_TAIL(mbi_sink*, &sinks);
			if (sink == NULL) {
				fprintf(stderr, "mbi: input event dropped\n");
				dispatched = 1;

			} else if ((ret = write_or_epipe(sink->writefd, &e, sizeof(mbi_event))) == 0) {
				pthread_mutex_lock(&sinks_lock);
				LIST_REMOVE(sink);
				pthread_mutex_unlock(&sinks_lock);

				close(sink->writefd);
				free(sink);

			} else {
				//fprintf(stderr, "write_or_epipe() returned %i\n", ret);
				dispatched = 1;
			}
		}
		while (!dispatched);
	}
	fprintf(stderr, "mbi: Input loop exiting\n");
	return 0;
}

/**
 * mbi_grab_input() -- Returns a file descriptor to a pipe where
 * all input events will be sent until the file descriptor is closed,
 * in which case the prior descriptor is closed or until mbi_grab_input()
 * is called again
 */
int
mbi_grab_input(void)
{
	int inputfd[2];
	mbi_sink *input_sink;

	input_sink = malloc(sizeof(mbi_sink));
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
	pthread_mutex_lock(&sinks_lock);
	LIST_APPEND(&sinks, input_sink);
	pthread_mutex_unlock(&sinks_lock);

	return input_sink->readfd;
}

void
mbi_event_send(mbi_event e)
{
        pthread_mutex_lock(&lock);
        write_or_die(writefd, &e, sizeof(mbi_event));
        pthread_mutex_unlock(&lock);

}

/**
 * mbi_init() -- Initialize input subsystem
 */
int
mbi_init(void)
{
	int event_pipe[2];

	/* ignore SIGPIPE */
	signal(SIGPIPE, SIG_IGN);

	/* create event pipes */
	if (pipe(event_pipe) == -1) {
		fprintf(stderr, "mbi_init() -- pipe() failed\n");
		return -1;
	}

	/* save pipe endpoints */
	readfd = event_pipe[0];
	writefd = event_pipe[1];

	/* initialize sinks stack */
	LIST_INIT(&sinks);

	/* initialize directfb input provider */
	if (mbi_directfb_init() == -1) {
		fprintf(stderr, "!!! mbi_directfb_init() failed\n");
	}

	/* initialize the tcp remote input provider */
	if (mbi_tcp_init() == -1) {
		fprintf(stderr, "!!! mbi_tcp_init() failed\n");
	}

	if (pthread_create(&input_loop_thread, NULL, mbi_loop, NULL) != 0) {
		fprintf(stderr, "pthread_create() failed\n");
		return -1;
	}	

	return 0;
}

void
mbi_destroy(void)
{
	mbi_directfb_destroy();
	mbi_tcp_destroy();
	/* mbi_tcp_destroy(); */
	mbi_event_send(MBI_EVENT_EXIT);
	pthread_join(input_loop_thread, NULL);
	close(writefd);
	close(readfd);
}

