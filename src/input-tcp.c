#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>



#include "input.h"
#include "debug.h"


static int sockfd = -1;
static pthread_t thread;

static void *
mbi_tcp_server(void *arg)
{
	int newsockfd = -1, portno;
	unsigned int clilen;
	char buffer[256];
	struct sockaddr_in serv_addr, cli_addr;
	int n;


	MB_DEBUG_SET_THREAD_NAME("input-tcp");

	fprintf(stderr, "TCP Server starting\n");

	while (1) {

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
			sleep(1);
			continue;
		}

		listen(sockfd,5);
		clilen = sizeof(cli_addr);
		while((newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen)) > 0) {
			bzero(buffer,256);
			while ((n = read(newsockfd, buffer, 255)) > 0) {

				fprintf(stderr, "Here is the message: %s\n",buffer);

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
				}
			}

			close(newsockfd);
			newsockfd = -1;
		}
	}

	fprintf(stderr, "input-tcp: TCP Input server exiting\n");
	if (newsockfd != -1) {
		close(newsockfd);
	}
	close(sockfd);
	sockfd = -1;

 

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
	close(sockfd);
	pthread_join(thread, NULL);
}

