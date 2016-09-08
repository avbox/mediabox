#ifdef HAVE_CONFIG_H
#include "config.h"
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


#include "input.h"
#include "input-socket.h"
#include "linkedlist.h"
#include "debug.h"


static int sockfd = -1;
static int newsockfd = -1;
static int server_quit = 0;
static pthread_t thread;


LIST_DECLARE_STATIC(sockets);


static void
mbi_tcp_socket_closed(struct conn_state *state)
{
	DEBUG_VPRINT("input-tcp", "Connection closed (fd=%i)", state->fd);
	LIST_REMOVE(state);
	free(state);
}


static void *
mbi_tcp_server(void *arg)
{
	int portno;
	unsigned int clilen;
	struct sockaddr_in serv_addr, cli_addr;
	struct timeval tv;
	struct conn_state *state = NULL;
	fd_set fds;
	int n;

	MB_DEBUG_SET_THREAD_NAME("input-tcp");
	DEBUG_PRINT("input-tcp", "TCP input server starting");

	while (!server_quit) {

		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0) {
			fprintf(stderr, "mbi_tcp: Could not open socket\n");
			sleep(1);
			continue;
		}

		bzero((char *) &serv_addr, sizeof(serv_addr));
		portno = 2048;
		serv_addr.sin_family = AF_INET;
		serv_addr.sin_addr.s_addr = INADDR_ANY;
		serv_addr.sin_port = htons(portno);
		if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
			fprintf(stderr, "mbi_tcp: Could not bind to socket\n");
			close(sockfd);
			sockfd = -1;
			sleep(5);
			continue;
		}

		listen(sockfd, 1);
		clilen = sizeof(cli_addr);

		DEBUG_VPRINT("input-tcp", "Listening for connections on port %i", portno);

		while(!server_quit) {

			FD_ZERO(&fds);
			FD_SET(sockfd, &fds);

			/* fprintf(stderr, "input-tcp: Waiting for connection\n"); */

			tv.tv_sec = 1;
			tv.tv_usec = 0;
			if ((n = select(sockfd + 1, &fds, NULL, NULL, &tv)) == 0) {
				continue;
			} else if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				fprintf(stderr, "input-tcp: select() returned %i\n", n);
				break;
			}
			
			if (!FD_ISSET(sockfd, &fds)) {
				continue;
			}

			if ((newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen)) <= 0) {
				fprintf(stderr, "input-tcp: Could not accept socket. ret=%i\n",
					newsockfd);
				continue;
			}

			DEBUG_VPRINT("input-tcp", "Incoming connection accepted (fd=%i)", newsockfd);

			if ((state = malloc(sizeof(struct conn_state))) == NULL) {
				fprintf(stderr, "input-tcp: Could not allocate connection state: Out of memory\n");
				close(newsockfd);
				continue;
			}

			state->fd = newsockfd;
			state->quit = 0;
			state->closed_callback = mbi_tcp_socket_closed;

			LIST_ADD(&sockets, state);

			if (pthread_create(&state->thread, NULL, &mbi_socket_connection, state) != 0) {
				fprintf(stderr, "input-tcp: Could not launch connection thread\n");
				close(newsockfd);
				free(state);
				continue;
			}
		}
		close(sockfd);
		sockfd = -1;
	}

	DEBUG_PRINT("input-tcp", "TCP input server exiting");

	return NULL;
}


/**
 * mbi_tcp_init() -- Initialize the tcp input server
 */
int
mbi_tcp_init(void)
{
	LIST_INIT(&sockets);

	if (pthread_create(&thread, NULL, mbi_tcp_server, NULL) != 0) {
		fprintf(stderr, "Could not create TCP server thread\n");
		return -1;
	}
	return 0;
}


void
mbi_tcp_destroy(void)
{
	struct conn_state *socket;

	DEBUG_PRINT("input-tcp", "Exiting (give me 2 secs)");

	/* Close all connections */
	DEBUG_PRINT("input-tcp", "Closing all open sockets");
	LIST_FOREACH(struct conn_state*, socket, &sockets) {
		socket->quit = 1;
		pthread_join(socket->thread, NULL);
	}

	server_quit = 1;
	if (newsockfd != -1) {
		close(newsockfd);
	}
	close(sockfd);
	pthread_join(thread, NULL);
}

