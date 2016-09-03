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
#include "debug.h"


#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)


struct conn_state
{
	int fd;
	pthread_t thread;
};


static int sockfd = -1;
static int newsockfd = -1;
static int server_quit = 0;
static pthread_t thread;


static void *
mbi_tcp_connection(void *arg)
{
	int fd = (int) ((struct conn_state*) arg)->fd;
	int n;
	struct timeval tv;
	char buffer[256];
	fd_set fds;

	assert(arg != NULL);
	assert(((struct conn_state*) arg)->fd > 0);

	MB_DEBUG_SET_THREAD_NAME("input-tcp-conn");
	DEBUG_PRINT("input-tcp-conn", "Connection handler running");
	pthread_detach(pthread_self());

	bzero(buffer,256);

	while (!server_quit) {

		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		if (fcntl(fd, F_GETFD) == -1) {
			fprintf(stderr, "input-tcp: Connection broken (fd=%i)\n",
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
			fprintf(stderr, "input-tcp: select() returned %i\n", n);
			break;
		}

		if (!FD_ISSET(fd, &fds)) {
			continue;
		}

		if ((n = read(fd, buffer, 255)) <= 0) {
			break;
		}

		if (!memcmp("MENU", buffer, 4)) {
			mbi_event_send(MBI_EVENT_MENU);
		} else if (!memcmp("LEFT", buffer, 4)) {
			mbi_event_send(MBI_EVENT_ARROW_LEFT);
		} else if (!memcmp("RIGHT", buffer, 5)) {
			mbi_event_send(MBI_EVENT_ARROW_RIGHT);
		} else if (!memcmp("UP", buffer, 2)) {
			mbi_event_send(MBI_EVENT_ARROW_UP);
		} else if (!memcmp("DOWN", buffer, 4)) {
			mbi_event_send(MBI_EVENT_ARROW_DOWN);
		} else if (!memcmp("ENTER", buffer, 5)) {
			mbi_event_send(MBI_EVENT_ENTER);
		} else if (!memcmp("BACK", buffer, 4)) {
			mbi_event_send(MBI_EVENT_BACK);
		} else if (!memcmp("PLAY", buffer, 4)) {
			mbi_event_send(MBI_EVENT_PLAY);
		} else if (!memcmp("STOP", buffer, 4)) {
			mbi_event_send(MBI_EVENT_STOP);
		} else if (!memcmp("CLEAR", buffer, 5)) {
			mbi_event_send(MBI_EVENT_CLEAR);
		} else if (!memcmp("PREV", buffer, 4)) {
			mbi_event_send(MBI_EVENT_PREV);
		} else if (!memcmp("NEXT", buffer, 4)) {
			mbi_event_send(MBI_EVENT_NEXT);
		} else if (!memcmp("INFO", buffer, 4)) {
			mbi_event_send(MBI_EVENT_INFO);
		} else if (!memcmp("KEY:", buffer, 4)) {
#define ELIF_KEY(x) \
	else if (!memcmp(buffer + 4, STRINGIZE(x), 1)) { \
		mbi_event_send(MBI_EVENT_KBD_ ##x ); \
	}

			if (!memcmp(buffer + 4, " ", 1)) {
				mbi_event_send(MBI_EVENT_KBD_SPACE);
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
			DEBUG_VPRINT("input-tcp-conn", "Unknown command '%s'", buffer);
		}
	}

	DEBUG_VPRINT("input-tcp", "Closing connection (fd=%i)", fd);

	close(fd);
	free(arg); /* free conn state */

	return NULL;
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

			if (pthread_create(&state->thread, NULL, &mbi_tcp_connection, state) != 0) {
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
	if (pthread_create(&thread, NULL, mbi_tcp_server, NULL) != 0) {
		fprintf(stderr, "Could not create TCP server thread\n");
		return -1;
	}
	return 0;
}


void
mbi_tcp_destroy(void)
{
	DEBUG_PRINT("input-tcp", "Exiting (give me 2 secs)");

	server_quit = 1;
	if (newsockfd != -1) {
		close(newsockfd);
	}
	close(sockfd);
	pthread_join(thread, NULL);
}

