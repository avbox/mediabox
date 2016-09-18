#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/signal.h>

#include "input.h"
#include "input-directfb.h"
#include "input-tcp.h"
#include "input-bluetooth.h"
#include "linkedlist.h"
#include "debug.h"
#include "log.h"


LISTABLE_TYPE(mbi_sink,
	int readfd;
	int writefd;
);


static int readfd;
static int writefd;
static pthread_t input_loop_thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sinks_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t dispatch_lock = PTHREAD_MUTEX_INITIALIZER;


LIST_DECLARE_STATIC(sinks);
LIST_DECLARE_STATIC(nonblock_sinks);


int
mbi_dispatchevent(mbi_event e)
{
	int ret;
	int dispatched = 1;
	mbi_sink *sink;

	pthread_mutex_lock(&dispatch_lock);

	/* first send the message to all nonblocking sinks */
	LIST_FOREACH_SAFE(mbi_sink*, sink, &nonblock_sinks, {
		if (write_or_epipe(sink->writefd, &e, sizeof(mbi_event)) == -1) {
			pthread_mutex_lock(&sinks_lock);
			LIST_REMOVE(sink);
			pthread_mutex_unlock(&sinks_lock);
			free(sink);
		}
	});

	sink = LIST_TAIL(mbi_sink*, &sinks);
	if (sink == NULL) {
		LOG_PRINT(MB_LOGLEVEL_INFO, "input", "Input event dropped. No sinks");

	} else if ((ret = write_or_epipe(sink->writefd, &e, sizeof(mbi_event))) == 0) {
		pthread_mutex_lock(&sinks_lock);
		LIST_REMOVE(sink);
		pthread_mutex_unlock(&sinks_lock);

		close(sink->writefd);
		free(sink);
		dispatched = 0;

	} else {
		LOG_VPRINT(MB_LOGLEVEL_ERROR, "input",
			"write_or_epipe returned %i", ret);
	}

	pthread_mutex_unlock(&dispatch_lock);

	return dispatched;
}


static void*
mbi_loop(void *arg)
{
	mbi_event e;

	(void) arg;

	MB_DEBUG_SET_THREAD_NAME("input");
	DEBUG_PRINT("input", "Starting input dispatcher thread");

	while (1) {

		/* read the next event */
		read_or_die(readfd, &e, sizeof(mbi_event));

		if (e == MBI_EVENT_EXIT) {
			DEBUG_PRINT("input", "EXIT command received");
			break;
		}

		while (!mbi_dispatchevent(e));
	}

	DEBUG_PRINT("input", "Input dispatcher thread exiting");

	return 0;
}


/**
 * mbi_grab_input() -- Returns a file descriptor to a pipe where
 * all input events will be sent until the file descriptor is closed,
 * in which case the prior descriptor is closed or until mbi_grab_input()
 * is called again
 */
static int
mbi_grab_input_internal(int block)
{
	int inputfd[2];
	mbi_sink *input_sink;

	input_sink = malloc(sizeof(mbi_sink));
	if (input_sink == NULL) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "mbi_grab_input(): Out of memory");
		return -1;
	}

	if (pipe(inputfd) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "mbi_grab_input(): pipe() failed");
		free(input_sink);
		return -1;
	}

	/* save file descriptors */
	input_sink->readfd = inputfd[0];
	input_sink->writefd = inputfd[1];

	/* add sink to stack */
	if (block) {
		pthread_mutex_lock(&sinks_lock);
		LIST_APPEND(&sinks, input_sink);
		pthread_mutex_unlock(&sinks_lock);
	} else {
		pthread_mutex_lock(&sinks_lock);
		LIST_APPEND(&nonblock_sinks, input_sink);
		pthread_mutex_unlock(&sinks_lock);

	}

	return input_sink->readfd;
}


int
mbi_grab_input(void)
{
	return mbi_grab_input_internal(1);
}


int
mbi_grab_input_nonblock(void)
{
	return mbi_grab_input_internal(0);
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

	DEBUG_PRINT("input", "Starting input dispatcher");

	/* ignore SIGPIPE */
	signal(SIGPIPE, SIG_IGN);

	/* create event pipes */
	if (pipe(event_pipe) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Cannot initialize: pipe() failed");
		return -1;
	}

	/* save pipe endpoints */
	readfd = event_pipe[0];
	writefd = event_pipe[1];

	/* initialize sinks stack */
	LIST_INIT(&sinks);
	LIST_INIT(&nonblock_sinks);

	/* initialize directfb input provider */
	if (mbi_directfb_init() == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Could not start DirectFB provider");
	}

	/* initialize the tcp remote input provider */
	if (mbi_tcp_init() == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Could not start TCP provider");
	}

#ifdef ENABLE_BLUETOOTH
	/* initialize the bluetooth input provider */
	if (mbi_bluetooth_init() == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Could not start Bluetooth provider");
	}
#endif

	if (pthread_create(&input_loop_thread, NULL, mbi_loop, NULL) != 0) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input",
			"Could not start input dispatch thread");
		return -1;
	}	

	return 0;
}


void
mbi_destroy(void)
{
	mbi_directfb_destroy();
	mbi_tcp_destroy();
#ifdef ENABLE_BLUETOOTH
	mbi_bluetooth_destroy();
#endif
	mbi_event_send(MBI_EVENT_EXIT);
	pthread_join(input_loop_thread, NULL);
	close(writefd);
	close(readfd);
}

