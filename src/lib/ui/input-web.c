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

#include <libwebsockets.h>

#define LOG_MODULE "input-web"

#include "input.h"
#include "../debug.h"
#include "../log.h"
#include "../file_util.h"
#include "../thread.h"

#define MAX_RESPONSE_LENGTH	(1024LL * 1024LL)


static int running;
static struct lws_context* web_server_ctx;
static struct avbox_thread* web_server_thread;
static struct avbox_delegate* web_server_task;
static char* remote_html;
static int remote_html_len;


static int
callback_http(struct lws *wsi,
	 enum lws_callback_reasons reason, void *user, void *in, size_t len);
static int
callback_websocket(struct lws*const wsi,
	enum lws_callback_reasons reason, void*const user, void*const in, size_t len);


static const struct lws_http_mount mount =
{
	/* .mount_next */		NULL,		/* linked-list "next" */
	/* .mountpoint */		"/",		/* mountpoint URL */
	/* .origin */			NULL,		/* protocol */
	/* .def */			NULL,
	/* .protocol */			"http",
	/* .cgienv */			NULL,
	/* .extra_mimetypes */		NULL,
	/* .interpret */		NULL,
	/* .cgi_timeout */		0,
	/* .cache_max_age */		0,
	/* .auth_mask */		0,
	/* .cache_reusable */		0,
	/* .cache_revalidate */		0,
	/* .cache_intermediaries */	0,
	/* .origin_protocol */		LWSMPRO_CALLBACK, /* dynamic */
	/* .mountpoint_len */		1,		/* char count */
	/* .basic_auth_login_file */	NULL
};


static struct lws_protocols protocols[] =
{
	{ "http", callback_http, 0, 0 },
	{ "webremote", callback_websocket, 0, 1024*1024, 0, NULL, 0 },
	{ NULL, NULL, 0, 0 }
};


static int
cmdcmp(char*const in, size_t len, const char*const match)
{
	if (strlen(match) != len)
		return 1;
	return memcmp(match, in, len);
}


static int
callback_websocket(struct lws*const wsi,
	enum lws_callback_reasons reason, void*const user, void*const in, size_t len)
{
	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
	{
		DEBUG_VPRINT(LOG_MODULE, "Protocol init %p, protocol=%s",
			wsi, lws_get_protocol(wsi));
		break;
	}
	case LWS_CALLBACK_ESTABLISHED:
	{
		DEBUG_PRINT(LOG_MODULE, "Connection established");
		break;
	}
	case LWS_CALLBACK_CLOSED:
	{
		DEBUG_PRINT(LOG_MODULE, "Connection closed");
		break;
	}
	case LWS_CALLBACK_SERVER_WRITEABLE:
	{
		DEBUG_PRINT(LOG_MODULE, "Server writeable");
		break;
	}
	case LWS_CALLBACK_RECEIVE:
	{
		/* send the command to the input stack */
		if (!cmdcmp(in, len, "VOLUP")) {
			avbox_input_sendevent(MBI_EVENT_VOLUME_UP, NULL);
		} else if (!cmdcmp(in, len, "UP")) {
			avbox_input_sendevent(MBI_EVENT_ARROW_UP, NULL);
		} else if (!cmdcmp(in, len, "TRACK")) {
			avbox_input_sendevent(MBI_EVENT_TRACK, NULL);
		} else if (!cmdcmp(in, len, "LEFT")) {
			avbox_input_sendevent(MBI_EVENT_ARROW_LEFT, NULL);
		} else if (!cmdcmp(in, len, "MENU")) {
			avbox_input_sendevent(MBI_EVENT_MENU, NULL);
		} else if (!cmdcmp(in, len, "RIGHT")) {
			avbox_input_sendevent(MBI_EVENT_ARROW_RIGHT, NULL);
		} else if (!cmdcmp(in, len, "VOLDOWN")) {
			avbox_input_sendevent(MBI_EVENT_VOLUME_DOWN, NULL);
		} else if (!cmdcmp(in, len, "DOWN")) {
			avbox_input_sendevent(MBI_EVENT_ARROW_DOWN, NULL);
		} else if (!cmdcmp(in, len, "MUTE")) {
			avbox_input_sendevent(MBI_EVENT_VOLUME_DOWN, NULL);
		} else if (!cmdcmp(in, len, "BACK")) {
			avbox_input_sendevent(MBI_EVENT_BACK, NULL);
		} else if (!cmdcmp(in, len, "ENTER")) {
			avbox_input_sendevent(MBI_EVENT_ENTER, NULL);
		} else if (!cmdcmp(in, len, "PREV")) {
			avbox_input_sendevent(MBI_EVENT_PREV, NULL);
		} else if (!cmdcmp(in, len, "NEXT")) {
			avbox_input_sendevent(MBI_EVENT_NEXT, NULL);
		} else if (!cmdcmp(in, len, "STOP")) {
			avbox_input_sendevent(MBI_EVENT_STOP, NULL);
		} else if (!cmdcmp(in, len, "PLAY")) {
			avbox_input_sendevent(MBI_EVENT_PLAY, NULL);
		} else if (!cmdcmp(in, len, "INFO")) {
			avbox_input_sendevent(MBI_EVENT_INFO, NULL);
		} else if (len > 4 && !memcmp(in, "URL:", 4)) {
			const int sz = len - 4;
			char*const url = malloc(sz + 1);
			if (url != NULL) {
				url[sz] = '\0';
				memcpy(url, in + 4, sz);
				avbox_input_sendevent(MBI_EVENT_URL, url);
			}
		}
		break;
	}
	default:
		break;
	}
	return 0;
}


static int
callback_http(struct lws *wsi,
	 enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
	uint8_t buf[LWS_PRE + 256], *start = &buf[LWS_PRE], *p = start,
		*end = &buf[sizeof(buf) - 1];

	switch (reason) {
	case LWS_CALLBACK_HTTP:
	{
		/* prepare and write http headers */
		if (lws_add_http_common_headers(wsi,
			HTTP_STATUS_OK, "text/html", remote_html_len, &p, end)) {
			return 1;
		}
		if (lws_finalize_write_http_header(wsi, start, &p, end)) {
			return 1;
		}

		/* write the body separately */
		lws_callback_on_writable(wsi);

		return 0;
	}
	case LWS_CALLBACK_HTTP_WRITEABLE:
	{
		int bytes_written;
		ASSERT(remote_html_len <= MAX_RESPONSE_LENGTH);

		if ((bytes_written = lws_write(wsi, (unsigned char*)remote_html, remote_html_len,
			      LWS_WRITE_HTTP)) != remote_html_len) {
			LOG_VPRINT_ERROR("Could not write entire response (total=%i, written=%i)",
				(int) remote_html_len, bytes_written);
			return 1;
		}

		if (lws_http_transaction_completed(wsi)) {
			return -1;
		}

		return 0;
	}
	default:
		break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);
}


static void*
web_server_listen(void*const arg)
{
	DEBUG_SET_THREAD_NAME("webserver");
	DEBUG_VPRINT(LOG_MODULE, "Waiting for connections (ctx=%p)",
		web_server_ctx);
	ASSERT(web_server_ctx != NULL);
	while (running && lws_service(web_server_ctx, 1000) >= 0);
	DEBUG_PRINT(LOG_MODULE, "Webservice shutting down");
	return NULL;
}


int
avbox_webinput_init(void)
{
	int len;
	struct lws_context_creation_info info;

	DEBUG_PRINT(LOG_MODULE, "Initializing webinput driver");

	/* read the remote html */
	if ((remote_html = avbox_get_resource("mediabox/webremote.html",
		&len)) == NULL) {
		LOG_PRINT_ERROR("Could not get resource mediabox/webremote.html");
		return -1;
	}

	remote_html_len = len;
	running = 1;

	/* disable LWS logging */
	lws_set_log_level(0, NULL);

	/* create web server */
	memset(&info, 0, sizeof(struct lws_context_creation_info));
	info.port = (getuid() == 0) ? 80 : 8080;
	info.mounts = &mount;
	info.protocols = protocols;
	info.pt_serv_buf_size = MAX_RESPONSE_LENGTH;
	if ((web_server_ctx = lws_create_context(&info)) == NULL) {
		LOG_PRINT_ERROR("Could not initialize web server!");
		return -1;
	}

	/* create listener thread */
	if ((web_server_thread = avbox_thread_new(NULL, NULL, 0, 0)) == NULL) {
		LOG_PRINT_ERROR("Could not create web server thread!");
		lws_context_destroy(web_server_ctx);
		free(remote_html);
		return -1;
	}

	if ((web_server_task = avbox_thread_delegate(web_server_thread,
		web_server_listen, NULL)) == NULL) {
		avbox_thread_destroy(web_server_thread);
		lws_context_destroy(web_server_ctx);
		free(remote_html);
		return -1;
	}

	return 0;
}


void
avbox_webinput_shutdown(void)
{
	DEBUG_PRINT(LOG_MODULE, "Shutting down webinput driver");
	running = 0;
	avbox_delegate_wait(web_server_task, NULL);
	avbox_thread_destroy(web_server_thread);
	lws_context_destroy(web_server_ctx);
	free(remote_html);
}
