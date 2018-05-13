/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2018 Fernando Rodriguez
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
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include <onion/onion.h>
#include <onion/log.h>
#include <onion/version.h>
#include <onion/http.h>
#include <onion/websocket.h>

#define LOG_MODULE "input-web"

#include "input.h"
#include "../debug.h"
#include "../log.h"
#include "../file_util.h"
#include "../thread.h"


static onion* web_server;
static char* remote_html;
static struct avbox_thread* web_server_thread;
static struct avbox_delegate* web_server_task;


#define DIKS_ESCAPE 		(1)
#define DIKS_RETURN 		(28)
#define DIKS_SHIFT 		(42)
#define DIKS_CURSOR_UP 		(103)
#define DIKS_CURSOR_DOWN 	(108)
#define DIKS_CURSOR_LEFT 	(105)
#define DIKS_CURSOR_RIGHT 	(106)
#define DIKS_BACKSPACE 		(14)
#define DIKS_SPACE		(57)
#define DIKS_LEFT_CTRL		(29)
#define DIKS_LEFT_ALT		(56)

#define DIKS_KEY_A		(30)
#define DIKS_KEY_B		(48)
#define DIKS_KEY_C		(46)
#define DIKS_KEY_D		(32)
#define DIKS_KEY_E		(18)
#define DIKS_KEY_F		(33)
#define DIKS_KEY_G		(34)
#define DIKS_KEY_H		(35)
#define DIKS_KEY_I		(23)
#define DIKS_KEY_J		(36)
#define DIKS_KEY_K		(37)
#define DIKS_KEY_L		(38)
#define DIKS_KEY_M		(50)
#define DIKS_KEY_N		(49)
#define DIKS_KEY_O		(24)
#define DIKS_KEY_P		(25)
#define DIKS_KEY_Q		(16)
#define DIKS_KEY_R		(19)
#define DIKS_KEY_S		(31)
#define DIKS_KEY_T		(20)
#define DIKS_KEY_U		(22)
#define DIKS_KEY_V		(47)
#define DIKS_KEY_W		(17)
#define DIKS_KEY_X		(45)
#define DIKS_KEY_Y		(21)
#define DIKS_KEY_Z		(44)

#define CASE_KEYBOARD(x) \
	case DIKS_KEY_ ## x: \
		avbox_input_sendevent(MBI_EVENT_KBD_ ## x, NULL); \
		break;

static onion_connection_status
websocket_handler(void *data, onion_websocket *ws, ssize_t data_ready_len)
{
	char *buf;
	int len;

	/* allocate a buffer large enough for all data */
	if ((buf = malloc(data_ready_len + 1)) == NULL) {
		LOG_VPRINT_ERROR("Could not allocate %zd bytes buffer for message!",
			data_ready_len);
		return OCS_INTERNAL_ERROR;
	}

	DEBUG_VPRINT(LOG_MODULE, "Attempting to read %zd bytes",
		data_ready_len);

	/* read all available data */
	while ((len = onion_websocket_read(ws, buf, data_ready_len)) < 0) {
		if (errno == EAGAIN) {
			free(buf);
			return OCS_CLOSE_CONNECTION;
		}
		LOG_VPRINT_ERROR("Error reading data: %d: %s (%zd)",
			errno, strerror(errno), data_ready_len);
		free(buf);
		return OCS_NEED_MORE_DATA;
	}

	/* ensure data is null terminated */
	buf[len] = '\0';

	DEBUG_VPRINT(LOG_MODULE, "Read %i bytes out of %i",
		len, data_ready_len);

	/* send the command to the input stack */
	if (!strcmp(buf, "VOLUP")) {
		avbox_input_sendevent(MBI_EVENT_VOLUME_UP, NULL);
	} else if (!strcmp(buf, "UP")) {
		avbox_input_sendevent(MBI_EVENT_ARROW_UP, NULL);
	} else if (!strcmp(buf, "TRACK")) {
		avbox_input_sendevent(MBI_EVENT_TRACK, NULL);
	} else if (!strcmp(buf, "LEFT")) {
		avbox_input_sendevent(MBI_EVENT_ARROW_LEFT, NULL);
	} else if (!strcmp(buf, "MENU")) {
		avbox_input_sendevent(MBI_EVENT_MENU, NULL);
	} else if (!strcmp(buf, "RIGHT")) {
		avbox_input_sendevent(MBI_EVENT_ARROW_RIGHT, NULL);
	} else if (!strcmp(buf, "VOLDOWN")) {
		avbox_input_sendevent(MBI_EVENT_VOLUME_DOWN, NULL);
	} else if (!strcmp(buf, "DOWN")) {
		avbox_input_sendevent(MBI_EVENT_ARROW_DOWN, NULL);
	} else if (!strcmp(buf, "MUTE")) {
		avbox_input_sendevent(MBI_EVENT_VOLUME_DOWN, NULL);
	} else if (!strcmp(buf, "BACK")) {
		avbox_input_sendevent(MBI_EVENT_BACK, NULL);
	} else if (!strcmp(buf, "ENTER")) {
		avbox_input_sendevent(MBI_EVENT_ENTER, NULL);
	} else if (!strcmp(buf, "PREV")) {
		avbox_input_sendevent(MBI_EVENT_PREV, NULL);
	} else if (!strcmp(buf, "NEXT")) {
		avbox_input_sendevent(MBI_EVENT_NEXT, NULL);
	} else if (!strcmp(buf, "STOP")) {
		avbox_input_sendevent(MBI_EVENT_STOP, NULL);
	} else if (!strcmp(buf, "PLAY")) {
		avbox_input_sendevent(MBI_EVENT_PLAY, NULL);
	} else if (!strcmp(buf, "INFO")) {
		avbox_input_sendevent(MBI_EVENT_INFO, NULL);
	} else if (!strncmp(buf, "URL:", 4)) {
		char*const url = strdup(buf + 4);
		if (url != NULL) {
			avbox_input_sendevent(MBI_EVENT_URL, url);
		}
	}

	free(buf);
	return OCS_NEED_MORE_DATA;
}


static onion_connection_status
websocket_accept(void*const data, onion_request*const req, onion_response*const res)
{
	onion_websocket*const ws = onion_websocket_new(req, res);

	/* if this is an http:// request so just send the
	 * remote page */
	if (ws == NULL) {
		onion_response_write0(res, remote_html);
		return OCS_PROCESSED;
	}

	onion_websocket_set_callback(ws, websocket_handler);
	DEBUG_PRINT(LOG_MODULE, "Websocket created");
	return OCS_WEBSOCKET;
}


static void*
web_server_listen(void*const arg)
{
	DEBUG_SET_THREAD_NAME("webserver");
	ASSERT(web_server != NULL);
	onion_listen(web_server);
	return NULL;
}


int
avbox_webinput_init(void)
{
	int len;
	const char*const port = (getuid() == 0) ? "80" : "8080";

	DEBUG_PRINT(LOG_MODULE, "Initializing webinput driver");

	/* read the remote html */
	if ((remote_html = avbox_get_resource("mediabox/webremote.html",
		&len)) == NULL) {
		LOG_PRINT_ERROR("Could not get resource mediabox/webremote.html");
		return -1;
	}

	ONION_VERSION_IS_COMPATIBLE_OR_ABORT();

	/* create onion server */
	if ((web_server = onion_new(O_POOL)) == NULL) {
		ABORT("Could not create web server!");
	}

	/* configure it */
	onion_set_timeout(web_server, 5000);
	onion_set_hostname(web_server, "0.0.0.0");
	onion_set_port(web_server, port);
	onion_url *urls = onion_root_url(web_server);
	onion_url_add_static(urls, "", remote_html, HTTP_OK);
	onion_url_add(urls, "ws", websocket_accept);

	/* create listener thread */
	if ((web_server_thread = avbox_thread_new(NULL, NULL, 0, 0)) == NULL) {
		LOG_PRINT_ERROR("Could not create web server thread!");
		onion_free(web_server);
		free(remote_html);
		return -1;
	}

	if ((web_server_task = avbox_thread_delegate(web_server_thread,
		web_server_listen, NULL)) == NULL) {
		avbox_thread_destroy(web_server_thread);
		onion_free(web_server);
		free(remote_html);
		return -1;
	}

	return 0;
}


void
avbox_webinput_shutdown(void)
{
	DEBUG_PRINT(LOG_MODULE, "Shutting down webinput driver");
	onion_listen_stop(web_server);
	avbox_delegate_wait(web_server_task, NULL);
	avbox_thread_destroy(web_server_thread);
	onion_free(web_server);
	free(remote_html);
}
