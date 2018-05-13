/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifdef HAVE_CONFIG_H
#	include "../../config.h"
#endif
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define LOG_MODULE "input"

#include "input.h"
#include "input-tcp.h"
#include "../linkedlist.h"
#include "../debug.h"
#include "../log.h"
#include "../dispatch.h"

#ifdef ENABLE_DIRECTFB
#	include "input-directfb.h"
#endif
#ifdef ENABLE_LIBINPUT
#	include "input-libinput.h"
#endif
#ifdef ENABLE_BLUETOOTH
#	include "../bluetooth.h"
#	include "input-bluetooth.h"
#endif
#ifdef ENABLE_WEBREMOTE
#	include "input-web.h"
#endif

LISTABLE_STRUCT(avbox_input_endpoint,
	struct avbox_object *object;
);


static pthread_mutex_t endpoints_lock = PTHREAD_MUTEX_INITIALIZER;
static LIST endpoints;
#ifdef ENABLE_DIRECTFB
static int using_directfb = 0;
#endif
#ifdef ENABLE_LIBINPUT
static int using_libinput = 0;
#endif
static int using_tcp = 0;
#ifdef ENABLE_BLUETOOTH
static int using_bluetooth = 0;
#endif
#ifdef ENABLE_WEBREMOTE
static int using_webremote = 0;
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


static struct avbox_object**
avbox_input_getendpoints(int * const cnt)
{
	int i = 0;
	struct avbox_object** objlist = NULL;
	struct avbox_input_endpoint *ep;

	pthread_mutex_lock(&endpoints_lock);

	*cnt = LIST_SIZE(&endpoints);
	/* allocate memory for endpoints array */
	if ((objlist = malloc((*cnt + 1) *
		sizeof(struct avbox_object*))) == NULL) {
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
avbox_input_getendpoint(struct avbox_object *obj)
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


/**
 * Sends an input event down the input stack.
 *
 */
void
avbox_input_eventfree(struct avbox_input_message *msg)
{
	ASSERT(msg != NULL);
	ASSERT(msg->payload == NULL);	/* as a reminder to free it */
	free(msg);
}


/**
 * Returns a file descriptor to a pipe where
 * all input events will be sent until the file descriptor is closed,
 * in which case the prior descriptor is closed or until mbi_grab_input()
 * is called again
 */
int
avbox_input_grab(struct avbox_object *obj)
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
avbox_input_release(struct avbox_object *obj)
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
avbox_input_sendevent(enum avbox_input_event e, void * const payload)
{
	int cnt;
	struct avbox_input_message *ev;
	struct avbox_object **dest = NULL;

	/* allocate memory for event */
	if ((ev = malloc(sizeof(struct avbox_input_message))) == NULL) {
		abort();
	}

	/* get the input stack */
	if ((dest = avbox_input_getendpoints(&cnt)) == NULL) {
		abort();
	}

	/* no need to send msg if no endpoints */
	if (cnt == 0) {
		return;
	}

	/* initialize it */
	ev->msg = e;
	ev->payload = payload;

	/* send the event */
	if (avbox_object_sendmsg(dest, AVBOX_MESSAGETYPE_INPUT,
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
#ifdef ENABLE_DIRECTFB
	char *driver_string = "directfb";
#elif defined(ENABLE_LIBINPUT)
	char *driver_string = "libinput";
#else
	char *driver_string = "";
#endif

	DEBUG_PRINT("input", "Starting input dispatcher");

	(void) driver_string;

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

#ifdef ENABLE_DIRECTFB
	if (!strcmp(driver_string, "directfb")) {
		/* initialize directfb input provider */
		if (mbi_directfb_init() == -1) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Could not start DirectFB provider");
		} else {
			using_directfb = 1;
		}
	}
#endif

#ifdef ENABLE_LIBINPUT
#ifndef ENABLE_DIRECTFB
#define using_directfb 0
#endif
	/* initialize libinput driver */
	if (!using_directfb && mbi_libinput_init() == -1) {
		LOG_PRINT_ERROR("Could not initialize libinput driver");
	} else if (!using_directfb) {
		using_libinput = 1;
	}
#ifdef using_directfb
#undef using_directfb
#endif
#endif

	/* initialize the tcp remote input provider */
	if (mbi_tcp_init() == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Could not start TCP provider");
	} else {
		using_tcp = 1;
	}

#ifdef ENABLE_BLUETOOTH
	if (avbox_bluetooth_ready()) {
		/* initialize the bluetooth input provider */
		if (mbi_bluetooth_init() == -1) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "input", "Could not start Bluetooth provider");
		} else {
			using_bluetooth = 1;
		}
	} else {
		using_bluetooth = 0;
	}
#endif

#ifdef ENABLE_WEBREMOTE
	if (avbox_webinput_init() == -1) {
		LOG_PRINT_ERROR("Could not start web input provider!");
	} else {
		using_webremote = 1;
	}
#endif

	return 0;
}


void
avbox_input_shutdown(void)
{
#ifdef ENABLE_DIRECTFB
	if (using_directfb) {
		mbi_directfb_destroy();
	}
#endif
	if (using_tcp) {
		mbi_tcp_destroy();
	}
#ifdef ENABLE_BLUETOOTH
	if (using_bluetooth) {
		mbi_bluetooth_destroy();
		using_bluetooth = 0;
	}
#endif
#ifdef ENABLE_LIBINPUT
	if (using_libinput) {
		mbi_libinput_destroy();
	}
#endif
#ifdef ENABLE_WEBREMOTE
	if (using_webremote) {
		avbox_webinput_shutdown();
	}
#endif
}

