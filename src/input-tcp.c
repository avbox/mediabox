#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
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



static int sockfd = -1;
static int newsockfd = -1;
static int server_quit = 0;
static pthread_t thread;


static void *
mbi_tcp_server(void *arg)
{
	int portno;
	unsigned int clilen;
	char buffer[256];
	struct sockaddr_in serv_addr, cli_addr;
	struct timeval tv;
	fd_set fds;
	int n;


	MB_DEBUG_SET_THREAD_NAME("input-tcp");

	fprintf(stderr, "input-tcp: TCP Server starting\n");

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

		fprintf(stderr, "input-tcp: Listening for connections on port %i\n",
			portno);

		while(!server_quit) {

			FD_ZERO(&fds);
			FD_SET(sockfd, &fds);

			fprintf(stderr, "input-tcp: Waiting for connection\n");

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

			fprintf(stderr, "input-tcp: Incoming connection accepted (fd=%i)\n",
				newsockfd);

			bzero(buffer,256);
			while (!server_quit) {

				FD_ZERO(&fds);
				FD_SET(newsockfd, &fds);

				if (fcntl(newsockfd, F_GETFD) == -1) {
					fprintf(stderr, "input-tcp: Connection broken (fd=%i)\n",
						newsockfd);
					break;
				}

				tv.tv_sec = 1;
				tv.tv_usec = 0;
				if ((n = select(newsockfd + 1, &fds, NULL, NULL, &tv)) == 0) {
					continue;
				} else if (n < 0) {
					if (errno == EINTR) {
						continue;
					}
					fprintf(stderr, "input-tcp: select() returned %i\n", n);
					break;
				}

				if (!FD_ISSET(newsockfd, &fds)) {
					continue;
				}

				if ((n = read(newsockfd, buffer, 255)) <= 0) {
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
					fprintf(stderr, "input-tcp: Unknown command: '%s'\n",
						buffer);
				}
			}

			fprintf(stderr, "input-tcp: Closing connection (fd=%i)\n",
				newsockfd);
			close(newsockfd);
			newsockfd = -1;
		}
		close(sockfd);
		sockfd = -1;
	}

	fprintf(stderr, "input-tcp: TCP Input server exiting\n");

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
	fprintf(stderr, "input-tcp: Exiting (give me 2 secs)\n");
	server_quit = 1;
	if (newsockfd != -1) {
		close(newsockfd);
	}
	close(sockfd);
	pthread_join(thread, NULL);
}

