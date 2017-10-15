/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef ENABLE_BLUETOOTH
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <fcntl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/rfcomm.h>

#define LOG_MODULE "input-bluetooth"

#include "input.h"
#include "input-socket.h"
#include "../linkedlist.h"
#include "../debug.h"
#include "../log.h"
#include "../process.h"
#include "../bluetooth.h"


static int sockfd = -1;
static int newsockfd = -1;
static int server_quit = 0;
static pthread_t thread;


LIST_DECLARE_STATIC(sockets);


static void
mbi_bluetooth_socket_closed(struct conn_state *state)
{
	DEBUG_VPRINT("input-bluetooth", "Connection closed (fd=%i)", state->fd);
	LIST_REMOVE(state);
	free(state);
}


static void *
mbi_bluetooth_server(void *arg)
{
	int channelno;
	unsigned int clilen;
	struct sockaddr_rc serv_addr, cli_addr;
	struct timeval tv;
	struct conn_state *state = NULL;
	fd_set fds;
	int n;
	pthread_attr_t attr;

	MB_DEBUG_SET_THREAD_NAME("input-bluetooth");
	DEBUG_PRINT("input-bluetooth", "Bluetooth input server starting");

	if (pthread_attr_init(&attr) != 0) {
		LOG_PRINT_ERROR("Could not initialize pthread attributes");
		return NULL;
	}

	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	while (!server_quit) {
		channelno = 1;
		sockfd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
		if (sockfd < 0) {
			fprintf(stderr, "mbi_bluetooth: Could not open socket\n");
			sleep(1);
			continue;
		}
rebind:
		bzero((char *) &serv_addr, sizeof(serv_addr));
		serv_addr.rc_family = AF_BLUETOOTH;
		serv_addr.rc_channel = channelno;
		serv_addr.rc_bdaddr = *BDADDR_ANY;

		if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
			fprintf(stderr, "mbi_bluetooth: Could not bind to socket (channel=%i)\n",
				channelno);
			if (channelno < 30) {
				channelno++;
				goto rebind;
			}
			close(sockfd);
			sleep(5);
			continue;
		}

		if (listen(sockfd, 1) == -1) {
			fprintf(stderr, "input-bluetooth: listen() failed\n");
			close(sockfd);
			sleep(5);
			continue;
		}
		clilen = sizeof(cli_addr);

		/* register the bluetooth service */
		avbox_bluetooth_register_service(channelno);

		DEBUG_VPRINT("input-bluetooth", "Listening for connections on RFCOMM channel %i",
			channelno);

		while(!server_quit) {

			FD_ZERO(&fds);
			FD_SET(sockfd, &fds);

			/* fprintf(stderr, "input-bluetooth: Waiting for connection\n"); */

			tv.tv_sec = 1;
			tv.tv_usec = 0;
			if ((n = select(sockfd + 1, &fds, NULL, NULL, &tv)) == 0) {
				continue;
			} else if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				fprintf(stderr, "input-bluetooth: select() returned %i\n", n);
				break;
			}

			if (!FD_ISSET(sockfd, &fds)) {
				continue;
			}

			if ((newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen)) <= 0) {
				fprintf(stderr, "input-bluetooth: Could not accept socket. ret=%i\n",
					newsockfd);
				continue;
			}

			DEBUG_VPRINT("input-bluetooth", "Incoming connection accepted (fd=%i)", newsockfd);

			if ((state = malloc(sizeof(struct conn_state))) == NULL) {
				fprintf(stderr, "input-bluetooth: Could not allocate connection state: Out of memory\n");
				close(newsockfd);
				continue;
			}

			state->fd = newsockfd;
			state->quit = 0;
			state->closed_callback = mbi_bluetooth_socket_closed;

			LIST_ADD(&sockets, state);

			if (pthread_create(&state->thread, &attr, &mbi_socket_connection, state) != 0) {
				LOG_PRINT_ERROR("Could not create bluetooth socket thread");
				LIST_REMOVE(state);
				close(newsockfd);
				free(state);
				continue;
			}
		}
		close(sockfd);
		sockfd = -1;
	}

	DEBUG_PRINT("input-bluetooth", "TCP input server exiting");

	return NULL;
}


/**
 * Initialize the bluetooth input server
 */
int
mbi_bluetooth_init(void)
{
	DEBUG_PRINT("input-bluetooth", "Initializing bluetooth input server");
	LIST_INIT(&sockets);

	if (pthread_create(&thread, NULL, mbi_bluetooth_server, NULL) != 0) {
		fprintf(stderr, "input-bluetooth: Could not create bluetooth server thread\n");
		return -1;
	}
	return 0;
}


void
mbi_bluetooth_destroy(void)
{
	struct conn_state *socket;

	DEBUG_PRINT("input-bluetooth", "Exiting (give me 2 secs)");

	/* Close all connections */
	DEBUG_PRINT("input-bluetooth", "Closing all open sockets");
	LIST_FOREACH(struct conn_state*, socket, &sockets) {
		socket->quit = 1;
		/* pthread_join(socket->thread, NULL); */
	}

	server_quit = 1;
	if (newsockfd != -1) {
		close(newsockfd);
	}
	close(sockfd);
	pthread_join(thread, NULL);
}

#endif
