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
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

#define LOG_MODULE "input-tcp"

#include "input.h"
#include "input-socket.h"
#include "../linkedlist.h"
#include "../debug.h"
#include "../avbox.h"


static int sockfd = -1;
static int newsockfd = -1;
static int server_quit = 0;
static struct avbox_thread *thread = NULL;
static struct avbox_delegate *worker = NULL;


LIST_DECLARE_STATIC(sockets);


static void
avbox_tcp_socket_closed(struct socket_context *ctx)
{
	DEBUG_VPRINT(LOG_MODULE, "Connection closed (fd=%i)", ctx->fd);
	LIST_REMOVE(ctx);
	free(ctx);
}


static void *
avbox_tcp_listener(void *arg)
{
	int portno;
	unsigned int clilen;
	struct sockaddr_in serv_addr, cli_addr;
	struct timeval tv;
	struct socket_context *ctx = NULL;
	fd_set fds;
	int n;

	DEBUG_SET_THREAD_NAME("input-tcp");
	DEBUG_PRINT("input-tcp", "TCP input server starting");

	while (!server_quit) {

		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0) {
			LOG_VPRINT_ERROR("Could not open socket: %s. Will keep trying.",
				strerror(errno));
			sleep(1);
			continue;
		}

		const int reuse_addr = 1;
		setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void*) &reuse_addr,
			sizeof(reuse_addr));
		bzero((char *) &serv_addr, sizeof(serv_addr));
		portno = 2048;
		serv_addr.sin_family = AF_INET;
		serv_addr.sin_addr.s_addr = INADDR_ANY;
		serv_addr.sin_port = htons(portno);
		if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
			LOG_VPRINT_ERROR("Could not bind socket: %s",
				strerror(errno));
			close(sockfd);
			sockfd = -1;
			sleep(5);
			continue;
		}

		listen(sockfd, 1);
		clilen = sizeof(cli_addr);

		DEBUG_VPRINT(LOG_MODULE, "Listening for connections on port %i",
			portno);

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
				LOG_VPRINT_ERROR(LOG_MODULE, "select() error: %s",
					strerror(errno));
				break;
			}
			
			if (!FD_ISSET(sockfd, &fds)) {
				continue;
			}

			if ((newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen)) <= 0) {
				LOG_VPRINT_ERROR("accept() error: %s",
					strerror(errno));
				continue;
			}

			DEBUG_VPRINT(LOG_MODULE, "Incoming connection accepted (fd=%i)", newsockfd);

			if ((ctx = malloc(sizeof(struct socket_context))) == NULL) {
				LOG_PRINT_ERROR("Could not accept connection. Out of memory");
				close(newsockfd);
				continue;
			}

			ctx->fd = newsockfd;
			ctx->quit = 0;
			ctx->closed_callback = avbox_tcp_socket_closed;

			LIST_ADD(&sockets, ctx);

			if (pthread_create(&ctx->thread, NULL, &avbox_input_socket_connect, ctx) != 0) {
				LOG_PRINT_ERROR("Could not accept connection. Thread creation failed");
				close(newsockfd);
				free(ctx);
				continue;
			}
		}
		close(sockfd);
		sockfd = -1;
	}

	DEBUG_PRINT(LOG_MODULE, "TCP input server exiting");

	return NULL;
}


/**
 * Initialize the tcp input server
 */
int
mbi_tcp_init(void)
{
	LIST_INIT(&sockets);

	if ((thread = avbox_thread_new(NULL, NULL, AVBOX_THREAD_REALTIME, -5)) == NULL) {
		LOG_VPRINT_ERROR("Could not create TCP listener thread: %s",
			strerror(errno));
		return -1;
	}
	if ((worker = avbox_thread_delegate(thread, avbox_tcp_listener, NULL)) == NULL) {
		LOG_VPRINT_ERROR("Could not delegate TCP listener worker: %s",
			strerror(errno));
		avbox_thread_destroy(thread);
		thread = NULL;
		return -1;
	}
	return 0;
}


void
mbi_tcp_destroy(void)
{
	struct socket_context *ctx;

	DEBUG_PRINT("input-tcp", "Exiting (give me 2 secs)");

	/* Close all connections */
	DEBUG_PRINT("input-tcp", "Closing all open sockets");
	LIST_FOREACH(struct socket_context*, ctx, &sockets) {
		ctx->quit = 1;
		pthread_join(ctx->thread, NULL);
	}

	server_quit = 1;
	if (newsockfd != -1) {
		close(newsockfd);
	}
	close(sockfd);

	avbox_delegate_wait(worker, NULL);
	avbox_thread_destroy(thread);
	thread = NULL;

}
