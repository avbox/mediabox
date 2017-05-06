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
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/signal.h>

#define LOG_MODULE "input"

#include "input.h"
#include "input-directfb.h"
#include "input-libinput.h"
#include "input-tcp.h"
#include "input-bluetooth.h"
#include "../linkedlist.h"
#include "../debug.h"
#include "../log.h"
#include "../dispatch.h"


LISTABLE_STRUCT(avbox_input_endpoint,
	struct avbox_dispatch_object *object;
);


static pthread_mutex_t endpoints_lock = PTHREAD_MUTEX_INITIALIZER;
static LIST endpoints;
static int using_directfb = 0;
static int using_libinput = 0;
static int using_tcp = 0;
#ifdef ENABLE_BLUETOOTH
static int using_bluetooth = 0;
#endif


#if 0
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
#endif


static struct avbox_dispatch_object**
avbox_input_getendpoints()
{
	int i = 0;
	struct avbox_dispatch_object** objlist = NULL;
	struct avbox_input_endpoint *ep;

	pthread_mutex_lock(&endpoints_lock);

	/* allocate memory for endpoints array */
	if ((objlist = malloc((LIST_SIZE(&endpoints) + 1) *
		sizeof(struct avbox_dispatch_object*))) == NULL) {
		assert(errno == ENOMEM);
		goto end;
	}

	/* copy all endpoint pointers to array */
	LIST_FOREACH(struct avbox_input_endpoint*, ep, &endpoints) {
		objlist[i++] = ep->object;
	}
	objlist[i] = NULL;
end:
	pthread_mutex_unlock(&endpoints_lock);
	return objlist;
}


static struct avbox_input_endpoint*
avbox_input_getendpoint(struct avbox_dispatch_object *obj)
{
	struct avbox_input_endpoint *endpoint, *out = NULL;
	pthread_mutex_lock(&endpoints_lock);
	LIST_FOREACH(struct avbox_input_endpoint*, endpoint, &endpoints) {
		if (endpoint->object == obj) {
			out = endpoint;
			break;
		}
	}
	pthread_mutex_unlock(&endpoints_lock);
	return out;
}


void
avbox_input_eventfree(struct avbox_input_message *msg)
{
	assert(msg != NULL);
	free(msg);
}


/**
 * Returns a file descriptor to a pipe where
 * all input events will be sent until the file descriptor is closed,
 * in which case the prior descriptor is closed or until mbi_grab_input()
 * is called again
 */
int
avbox_input_grab(struct avbox_dispatch_object *obj)
{
	struct avbox_input_endpoint *input_endpoint;

	/* if the object is already on the stack remove it
	 * and reuse it's node, otherwise allocate a new node */
	if ((input_endpoint = avbox_input_getendpoint(obj)) == NULL) {
		if ((input_endpoint = malloc(sizeof(struct avbox_input_endpoint))) == NULL) {
			LOG_PRINT_ERROR("Could not grab input: Out of memory");
			assert(errno == ENOMEM);
			return -1;
		}
	} else {
		pthread_mutex_lock(&endpoints_lock);
		LIST_REMOVE(input_endpoint);
		pthread_mutex_unlock(&endpoints_lock);
	}

	/* initialize endpoint */
	input_endpoint->object = obj;

	/* add endpoint to stack */
	pthread_mutex_lock(&endpoints_lock);
	LIST_ADD(&endpoints, input_endpoint);
	pthread_mutex_unlock(&endpoints_lock);

	return 0;
}


/**
 * Release input.
 */
void
avbox_input_release(struct avbox_dispatch_object *obj)
{
	struct avbox_input_endpoint *endpoint;
#ifndef NDEBUG
	int released = 0;
#endif
	pthread_mutex_lock(&endpoints_lock);
	LIST_FOREACH_SAFE(struct avbox_input_endpoint*, endpoint, &endpoints, {
		if (endpoint->object == obj) {
			LIST_REMOVE(endpoint);
			free(endpoint);
#ifndef NDEBUG
			released = 1;
#endif
			break;
		}
	});
#ifndef NDEBUG
	if (!released) {
		DEBUG_PRINT("input", "Attempted to release object that is not on stack");
	}
#endif
	pthread_mutex_unlock(&endpoints_lock);
}


/**
 * Send an input event.
 */
void
avbox_input_sendevent(enum avbox_input_event e)
{
	struct avbox_input_message *ev;
	struct avbox_dispatch_object **dest = NULL;

	/* allocate memory for event */
	if ((ev = malloc(sizeof(struct avbox_input_message))) == NULL) {
		abort();
	}

	/* get the input stack */
	if ((dest = avbox_input_getendpoints()) == NULL) {
		abort();
	}

	/* initialize it */
	ev->msg = e;
	ev->payload = NULL;

	/* send the event */
	if (avbox_dispatch_sendmsg(-1, dest, AVBOX_MESSAGETYPE_INPUT,
		AVBOX_DISPATCH_ANYCAST, ev) == NULL) {
		abort();
	}

	free(dest);
}


/**
 * Initialize input subsystem
 */
int
avbox_input_init(int argc, char **argv)
{
	int i;
	char *driver_string = "directfb";

	DEBUG_PRINT("input", "Starting input dispatcher");

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--input:", 8)) {
			char *arg = argv[i] + 8;
			if (!strncmp(arg, "driver=", 7)) {
				driver_string = arg + 7;
			}
		}
	}


	/* initialize endpoints stack */
	LIST_INIT(&endpoints);

	if (!strcmp(driver_string, "directfb")) {
		/* initialize directfb input provider */
		if (mbi_directfb_init() == -1) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Could not start DirectFB provider");
		} else {
			using_directfb = 1;
		}
	}

	/* initialize libinput driver */
	if (!using_directfb && mbi_libinput_init() == -1) {
		LOG_PRINT_ERROR("Could not initialize libinput driver");
	} else if (!using_directfb) {
		using_libinput = 1;
	}

	/* initialize the tcp remote input provider */
	if (mbi_tcp_init() == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Could not start TCP provider");
	} else {
		using_tcp = 1;
	}

#ifdef ENABLE_BLUETOOTH
	/* initialize the bluetooth input provider */
	if (mbi_bluetooth_init() == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Could not start Bluetooth provider");
	} else {
		using_bluetooth = 1;
	}
#endif

	return 0;
}


void
avbox_input_shutdown(void)
{
	if (using_directfb) {
		mbi_directfb_destroy();
	}
	if (using_tcp) {
		mbi_tcp_destroy();
	}
#ifdef ENABLE_BLUETOOTH
	if (using_bluetooth) {
		mbi_bluetooth_destroy();
	}
#endif
	if (using_libinput) {
		mbi_libinput_destroy();
	}
}

