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
#	include <libavbox/avbox.h>
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

#include <libavbox/avbox.h>


static int sockfd = -1;
static int newsockfd = -1;
static int server_quit = 0;
static pthread_t thread;
LIST sockets;


static void
avbox_input_bluetooth_socket_closed(struct socket_context * const ctx)
{
	DEBUG_VPRINT("input-bluetooth", "Connection closed (fd=%i)", ctx->fd);
	LIST_REMOVE(ctx);
	free(ctx);
}


static void *
avbox_input_bluetooth_listener(void *arg)
{
	int channelno;
	unsigned int clilen;
	struct sockaddr_rc serv_addr, cli_addr;
	struct timeval tv;
	struct socket_context *ctx = NULL;
	fd_set fds;
	int n;
	pthread_attr_t attr;

	DEBUG_SET_THREAD_NAME("input-bluetooth");
	DEBUG_PRINT(LOG_MODULE, "Bluetooth input server starting");

	while (!server_quit) {
		channelno = 1;
		sockfd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
		if (sockfd < 0) {
			LOG_VPRINT_ERROR("Could not open socket: %s",
				strerror(errno));
			sleep(1);
			continue;
		}
rebind:
		bzero((char *) &serv_addr, sizeof(serv_addr));
		serv_addr.rc_family = AF_BLUETOOTH;
		serv_addr.rc_channel = channelno;
		serv_addr.rc_bdaddr = *BDADDR_ANY;

		if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
			LOG_VPRINT_ERROR("Could not bind() socket to channel %d: %s",
				channelno, strerror(errno));
			if (channelno < 30) {
				channelno++;
				goto rebind;
			}
			close(sockfd);
			sleep(5);
			continue;
		}

		if (listen(sockfd, 1) == -1) {
			LOG_VPRINT_ERROR("Could not listen() on socket: %s",
				strerror(errno));
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

			tv.tv_sec = 1;
			tv.tv_usec = 0;
			if ((n = select(sockfd + 1, &fds, NULL, NULL, &tv)) == 0) {
				continue;
			} else if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				LOG_VPRINT_ERROR("select() error: %s",
					strerror(errno));
				break;
			}

			if (!FD_ISSET(sockfd, &fds)) {
				continue;
			}

			if ((newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen)) <= 0) {
				LOG_VPRINT_ERROR("Could no accept() socket (fd=%d): %s",
					newsockfd, strerror(errno));
				continue;
			}

			DEBUG_VPRINT("input-bluetooth", "Incoming connection accepted (fd=%i)", newsockfd);

			if ((ctx = malloc(sizeof(struct socket_context))) == NULL) {
				LOG_PRINT_ERROR("Could not allocate connection context");
				close(newsockfd);
				continue;
			}

			ctx->fd = newsockfd;
			ctx->quit = 0;
			ctx->closed_callback = avbox_input_bluetooth_socket_closed;

			LIST_ADD(&sockets, ctx);

			if (pthread_create(&ctx->thread, &attr, &avbox_input_bluetooth_listener, ctx) != 0) {
				LOG_PRINT_ERROR("Could not create bluetooth socket thread");
				LIST_REMOVE(ctx);
				close(newsockfd);
				free(ctx);
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

	if (pthread_create(&thread, NULL, avbox_input_bluetooth_listener, NULL) != 0) {
		LOG_PRINT_ERROR("Could not create bluetooth listener thread");
		return -1;
	}
	return 0;
}


void
mbi_bluetooth_destroy(void)
{
	struct socket_context *socket;

	DEBUG_PRINT("input-bluetooth", "Exiting (give me 2 secs)");

	/* Close all connections */
	DEBUG_PRINT("input-bluetooth", "Closing all open sockets");
	LIST_FOREACH(struct socket_context*, socket, &sockets) {
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
