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
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>

#define LOG_MODULE "input-socket"

#include "input.h"
#include "input-socket.h"
#include "../debug.h"
#include "../log.h"


#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)


void *
avbox_input_socket_connect(void *arg)
{
	struct socket_context *ctx = (struct socket_context*) arg;
	int fd = ctx->fd;
	int n;
	ssize_t ret;
	struct timeval tv;
	char buffer[4096], *pbuf;
	fd_set fds;

	ASSERT(arg != NULL);
	ASSERT(((struct socket_context*) arg)->fd > 0);

	DEBUG_SET_THREAD_NAME("input-socket");
	DEBUG_PRINT("input-socket", "Connection handler running");

	pthread_detach(pthread_self());

	/* clear the buffer */
	bzero(buffer, sizeof(buffer));

	while (!ctx->quit) {

		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		/* check if the connection is still good */
		if (fcntl(fd, F_GETFD) == -1) {
			LOG_VPRINT_ERROR("Connection broken (fd=%i)", fd);
			break;
		}

		tv.tv_sec = 1;
		tv.tv_usec = 0;
		if ((n = select(fd + 1, &fds, NULL, NULL, &tv)) == 0) {
			continue;
		} else if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			LOG_VPRINT_ERROR("select() returned %i", n);
			break;
		}

		/* if there's no data continue waiting */
		if (!FD_ISSET(fd, &fds)) {
			continue;
		}

		/* read the next line one char at the time for now */
		n = 0;
		pbuf = buffer;
		while (n < sizeof(buffer) - 1) {
			if (ctx->quit) {
				goto end;
			}
			if ((ret = read(fd, pbuf, 1)) == -1) {
				if (errno == EAGAIN || errno == EINTR) {
					continue;
				} else {
					LOG_VPRINT_ERROR("Unable to read() from socket: %s",
						strerror(errno));
					goto end;
				}
			} else if (ret == 0) {
				goto end; /* eof */
			}
			if (*pbuf == '\n') {
				*pbuf = '\0';
				break;
			}
			n++;
			pbuf++;
		}

		/* process the command */
		if (!strncmp("DOWNLOAD:", buffer, 9)) {
			char *url;
			if ((url = strdup(buffer + 9)) == NULL) {
				LOG_PRINT_ERROR("Could not allocate memory for DOWNLOAD link");
			} else {
				avbox_input_sendevent(MBI_EVENT_DOWNLOAD, url);
			}
		} else if (!strncmp("MENU_LONG", buffer, 9)) {
			avbox_input_sendevent(MBI_EVENT_CONTEXT, NULL);
		} else if (!strncmp("MENU", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_MENU, NULL);
		} else if (!strncmp("LEFT", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_ARROW_LEFT, NULL);
		} else if (!strncmp("RIGHT", buffer, 5)) {
			avbox_input_sendevent(MBI_EVENT_ARROW_RIGHT, NULL);
		} else if (!strncmp("UP", buffer, 2)) {
			avbox_input_sendevent(MBI_EVENT_ARROW_UP, NULL);
		} else if (!strncmp("DOWN", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_ARROW_DOWN, NULL);
		} else if (!strncmp("ENTER", buffer, 5)) {
			avbox_input_sendevent(MBI_EVENT_ENTER, NULL);
		} else if (!strncmp("BACK", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_BACK, NULL);
		} else if (!strncmp("PLAY", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_PLAY, NULL);
		} else if (!strncmp("STOP", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_STOP, NULL);
		} else if (!strncmp("CLEAR", buffer, 5)) {
			avbox_input_sendevent(MBI_EVENT_CLEAR, NULL);
		} else if (!strncmp("PREV", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_PREV, NULL);
		} else if (!strncmp("NEXT", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_NEXT, NULL);
		} else if (!strncmp("INFO", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_INFO, NULL);
		} else if (!strncmp("VOLUP", buffer, 5)) {
			avbox_input_sendevent(MBI_EVENT_VOLUME_UP, NULL);
		} else if (!strncmp("VOLDOWN", buffer, 7)) {
			avbox_input_sendevent(MBI_EVENT_VOLUME_DOWN, NULL);
		} else if (!strncmp("KEY:", buffer, 4)) {
#define ELIF_KEY(x) \
	else if (!strncmp(buffer + 4, STRINGIZE(x), 1)) { \
		avbox_input_sendevent(MBI_EVENT_KBD_ ##x, NULL); \
	}

			if (!strncmp(buffer + 4, " ", 1)) {
				avbox_input_sendevent(MBI_EVENT_KBD_SPACE, NULL);
			}
			ELIF_KEY(A)
			ELIF_KEY(B)
			ELIF_KEY(C)
			ELIF_KEY(D)
			ELIF_KEY(E)
			ELIF_KEY(F)
			ELIF_KEY(G)
			ELIF_KEY(H)
			ELIF_KEY(I)
			ELIF_KEY(J)
			ELIF_KEY(K)
			ELIF_KEY(L)
			ELIF_KEY(M)
			ELIF_KEY(N)
			ELIF_KEY(O)
			ELIF_KEY(P)
			ELIF_KEY(Q)
			ELIF_KEY(R)
			ELIF_KEY(S)
			ELIF_KEY(T)
			ELIF_KEY(U)
			ELIF_KEY(V)
			ELIF_KEY(W)
			ELIF_KEY(X)
			ELIF_KEY(Y)
			ELIF_KEY(Z)
#undef ELIF_KEY
		} else if (!strncmp("URL:", buffer, 4)) {
			char *url;
			if ((url = strdup(buffer + 4)) == NULL) {
				LOG_PRINT_ERROR("Could not allocate memory for URL");
			} else {
				avbox_input_sendevent(MBI_EVENT_URL, url);
			}
		} else if (!strncmp("TRACK", buffer, 5)) {
			avbox_input_sendevent(MBI_EVENT_TRACK, NULL);
		} else if (!strncmp("TRACK_LONG", buffer, 10)) {
			avbox_input_sendevent(MBI_EVENT_TRACK_LONG, NULL);
		} else {
			DEBUG_VPRINT(LOG_MODULE, "Unknown command '%s'", buffer);
		}
	}
end:
	DEBUG_VPRINT(LOG_MODULE, "Closing connection (fd=%i)", fd);

	close(fd);

	if (ctx->closed_callback != NULL) {
		ctx->closed_callback(ctx);
	}

	return NULL;
}
