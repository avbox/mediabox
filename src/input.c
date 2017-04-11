/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/signal.h>

#define LOG_MODULE "input"

#include "input.h"
#include "input-directfb.h"
#include "input-libinput.h"
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


static void *
malloc_safe(size_t sz)
{
	void *buf;
	while ((buf = malloc(sz)) == NULL) {
		usleep(500 * 1000L);
	}
	return buf;
}


static void *
realloc_safe(void *buf, size_t sz)
{
	void *newbuf;
	while ((newbuf = realloc(buf, sz)) == NULL) {
		usleep(500 * 1000);
	}
	return newbuf;
}


/**
 * mbi_dispatchmessage() -- Dispatch a message.
 */
static int
mbi_dispatchmessage(struct mb_message *msg)
{
	int ret;
	int dispatched = 0;
	mbi_sink *sink;

	pthread_mutex_lock(&dispatch_lock);

	/* if the message specifies a recipient sent it to the recipient's
	 * queue and return success if the message is successfully queued */
	if (msg->recipient != MBI_RECIPIENT_ANY) {
		LIST_FOREACH_SAFE(mbi_sink*, sink, &sinks, {
			if (sink->readfd == msg->recipient) {
				if (write_or_epipe(sink->writefd, msg, sizeof(struct mb_message) + msg->size) == -1) {
					pthread_mutex_lock(&sinks_lock);
					LIST_REMOVE(sink);
					pthread_mutex_unlock(&sinks_lock);
					free(sink);
				}
				dispatched = 0;
				goto end;
			}
		});
		errno = ENOENT;
		dispatched = -1;
		goto end;
	}

	/* first send the message to all nonblocking sinks */
	LIST_FOREACH_SAFE(mbi_sink*, sink, &nonblock_sinks, {
		if (write_or_epipe(sink->writefd, msg, sizeof(struct mb_message) + msg->size) == -1) {
			pthread_mutex_lock(&sinks_lock);
			LIST_REMOVE(sink);
			pthread_mutex_unlock(&sinks_lock);
			free(sink);
		}
	});

	sink = LIST_TAIL(mbi_sink*, &sinks);
	if (sink == NULL) {
		LOG_PRINT(MB_LOGLEVEL_INFO, "input", "Input event dropped. No sinks");

	} else if ((ret = write_or_epipe(sink->writefd, msg, sizeof(struct mb_message) + msg->size)) == 0) {
		pthread_mutex_lock(&sinks_lock);
		LIST_REMOVE(sink);
		pthread_mutex_unlock(&sinks_lock);

		close(sink->writefd);
		free(sink);
		dispatched = -1;

	} else if (ret < 0) {
		LOG_VPRINT(MB_LOGLEVEL_ERROR, "input",
			"write_or_epipe returned %i", ret);
	}

end:
	pthread_mutex_unlock(&dispatch_lock);

	return dispatched;
}


/**
 * mbi_dispatch_event() -- Sends a message without data to the thread that currently
 * receives input messages
 */
int
mbi_dispatchevent(enum mbi_event e)
{
	struct mb_message msg;
	msg.msg = e;
	msg.recipient = -1;
	msg.size = 0;
	return mbi_dispatchmessage(&msg);
}


/**
 * mbi_getmessage() -- Gets the next message at the queue specified
 * by the file descriptor.
 */
struct mb_message *
mbi_getmessage(int fd)
{
	struct mb_message *msg;

	msg = malloc_safe(sizeof(struct mb_message));

	/* read the next message */
	if (read_or_eof(fd, msg, sizeof(struct mb_message)) == -1) {
		free(msg);
		return NULL;
	}

	/* if the message has data read it */
	if (msg->size > 0) {
		msg = realloc_safe(msg, sizeof(struct mb_message) + msg->size);
		read_or_die(fd, ((uint8_t*) msg) + sizeof(struct mb_message), msg->size);
	}
	return msg;
}


/**
 * mbi_getevent() -- Gets the event code for the next message in the
 * queue and discards the rest of the message.
 */
int
mbi_getevent(int fd, enum mbi_event *e)
{
	struct mb_message *msg;
	if ((msg = mbi_getmessage(fd)) == NULL) {
		return -1;
	}
	*e = msg->msg;
	free(msg);
	return 0;
}


/**
 * mbi_loop() -- Runs on the background receiving and dispatching
 * messages.
 */
static void*
mbi_loop(void *arg)
{
	struct mb_message *msg;
	size_t msgsz;

	(void) arg;

	MB_DEBUG_SET_THREAD_NAME("input");
	DEBUG_PRINT("input", "Starting input dispatcher thread");

	msgsz = 0;
	msg = malloc_safe(sizeof(struct mb_message));

	while (1) {

		/* read the next event */
		read_or_die(readfd, msg, sizeof(struct mb_message));

		/* if the message has data read it */
		if (msg->size != 0) {
			if (msg->size > msgsz) {
				msg = realloc_safe(msg, sizeof(struct mb_message) + msg->size);
				msgsz = msg->size;
			}
			read_or_die(readfd, ((uint8_t*) msg) + sizeof(struct mb_message), msg->size);
		}

		/* if this is the special exit message then quit */
		if (msg->msg == MBI_EVENT_EXIT) {
			DEBUG_PRINT("input", "EXIT command received");
			break;
		}

		while (mbi_dispatchmessage(msg) == -1);
	}

	DEBUG_PRINT("input", "Input dispatcher thread exiting");

	free(msg);

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


/**
 * mbi_sendmessage() -- Sends a message.
 */
void
mbi_sendmessage(int recipient, enum mbi_event e, void *data, size_t sz)
{
	struct mb_message msg;
	msg.msg = e;
	msg.recipient = recipient;
	msg.size = sz;

	pthread_mutex_lock(&lock);
	write_or_die(writefd, &msg, sizeof(struct mb_message));
	if (sz > 0) {
		write_or_die(writefd, data, sz);
	}
        pthread_mutex_unlock(&lock);
}


void
mbi_event_send(enum mbi_event e)
{
	mbi_sendmessage(MBI_RECIPIENT_ANY, e, NULL, 0);
}


/**
 * mbi_init() -- Initialize input subsystem
 */
int
mbi_init(void)
{
	int event_pipe[2];
	int got_keyboard = 0;

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
	} else {
		got_keyboard = 1;
	}

	/* initialize libinput driver */
	if (!got_keyboard && mbi_libinput_init() == -1) {
		LOG_PRINT_ERROR("Could not initialize libinput driver");
	} else {
		got_keyboard = 1;
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

