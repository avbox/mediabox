/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
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


#include "input.h"
#include "input-socket.h"
#include "debug.h"


#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)


void *
mbi_socket_connection(void *arg)
{
	struct conn_state *state = (struct conn_state*) arg;
	int fd = state->fd;
	int n;
	struct timeval tv;
	char buffer[256];
	fd_set fds;

	assert(arg != NULL);
	assert(((struct conn_state*) arg)->fd > 0);

	MB_DEBUG_SET_THREAD_NAME("input-socket");
	DEBUG_PRINT("input-socket", "Connection handler running");
	pthread_detach(pthread_self());

	bzero(buffer,256);

	while (!state->quit) {

		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		if (fcntl(fd, F_GETFD) == -1) {
			fprintf(stderr, "input-socket: Connection broken (fd=%i)\n",
				fd);
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
			fprintf(stderr, "input-socket: select() returned %i\n", n);
			break;
		}

		if (!FD_ISSET(fd, &fds)) {
			continue;
		}

		if ((n = read(fd, buffer, 255)) <= 0) {
			break;
		}

		if (!memcmp("MENU", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_MENU);
		} else if (!memcmp("LEFT", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_ARROW_LEFT);
		} else if (!memcmp("RIGHT", buffer, 5)) {
			avbox_input_sendevent(MBI_EVENT_ARROW_RIGHT);
		} else if (!memcmp("UP", buffer, 2)) {
			avbox_input_sendevent(MBI_EVENT_ARROW_UP);
		} else if (!memcmp("DOWN", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_ARROW_DOWN);
		} else if (!memcmp("ENTER", buffer, 5)) {
			avbox_input_sendevent(MBI_EVENT_ENTER);
		} else if (!memcmp("BACK", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_BACK);
		} else if (!memcmp("PLAY", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_PLAY);
		} else if (!memcmp("STOP", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_STOP);
		} else if (!memcmp("CLEAR", buffer, 5)) {
			avbox_input_sendevent(MBI_EVENT_CLEAR);
		} else if (!memcmp("PREV", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_PREV);
		} else if (!memcmp("NEXT", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_NEXT);
		} else if (!memcmp("INFO", buffer, 4)) {
			avbox_input_sendevent(MBI_EVENT_INFO);
		} else if (!memcmp("VOLUP", buffer, 5)) {
			avbox_input_sendevent(MBI_EVENT_VOLUME_UP);
		} else if (!memcmp("VOLDOWN", buffer, 7)) {
			avbox_input_sendevent(MBI_EVENT_VOLUME_DOWN);
		} else if (!memcmp("KEY:", buffer, 4)) {
#define ELIF_KEY(x) \
	else if (!memcmp(buffer + 4, STRINGIZE(x), 1)) { \
		avbox_input_sendevent(MBI_EVENT_KBD_ ##x ); \
	}

			if (!memcmp(buffer + 4, " ", 1)) {
				avbox_input_sendevent(MBI_EVENT_KBD_SPACE);
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
		} else {
			DEBUG_VPRINT("input-socket", "Unknown command '%s'", buffer);
		}
	}

	DEBUG_VPRINT("input-socket", "Closing connection (fd=%i)", fd);

	close(fd);

	if (state->closed_callback != NULL) {
		state->closed_callback(state);
	}

	return NULL;
}

