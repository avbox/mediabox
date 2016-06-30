#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "debug.h"

#define MB_ANNOUNCE_INTERVAL  	(10)
#define MB_ANNOUNCE_PORT	(49550)

#define MB_FEATURES_NONE	(0x00)
#define MB_FEATURES_DLMASTER	(0x01)
#define MB_FEATURES_PLAYER	(0x02)
#define MB_FEATURES_SHAREDLIB	(0x04)


static pthread_t announce_thread;
static pthread_mutex_t announce_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t announce_cond = PTHREAD_COND_INITIALIZER;
static int announce_stop = 1;


void *
mb_announce_worker(void *arg)
{
	int sockfd = -1, broadcast = 1;
	struct sockaddr_in addr;
	struct timespec tv;
	struct timeval now;
	char ann[512];

	(void) arg;

	MB_DEBUG_SET_THREAD_NAME("announcer");

	fprintf(stderr, "announce: Starting\n");

	/* initialize announcement structure */
	snprintf(ann, 512, "MediaBox:%s:%s:DLMASTER,PLAYER,SHAREDLIB",
		"mediabox", "10.10.0.10");

	/* create socket */
	if ((sockfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		fprintf(stderr, "announce: Could not create socket\n");
		goto end;
	}

	/* enable broadcast */
	if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) == -1) {
		fprintf(stderr, "announce: setsockopt() failed\n");
		goto end;
	}

	/* bind socket to broadcast address */
	addr.sin_family = PF_INET;
	addr.sin_port = htons(MB_ANNOUNCE_PORT);
	addr.sin_addr.s_addr = htonl(-1);
	memset(addr.sin_zero, 0, sizeof(addr.sin_zero));
	if (bind(sockfd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		fprintf(stderr, "announce: bind() failed. errno=%i\n", errno);
		goto end;
	}

	/* broadcast announcement 3 times every MB_ANNOUNCE_INTERVAL seconds */
	while (!announce_stop) {
		if (sendto(sockfd, &ann, sizeof(ann), 0, &addr, sizeof(addr)) == -1 ||
			sendto(sockfd, &ann, sizeof(ann), 0, &addr, sizeof(addr)) == -1 ||
			sendto(sockfd, &ann, sizeof(ann), 0, &addr, sizeof(addr)) == -1) {
			fprintf(stderr, "announce: Could not broadcast announcement. errno=%i\n", errno);
		}

		gettimeofday(&now, NULL);
		tv.tv_sec = now.tv_sec + MB_ANNOUNCE_INTERVAL;
		pthread_mutex_lock(&announce_lock);
		pthread_cond_timedwait(&announce_cond, &announce_lock, &tv);
		pthread_mutex_unlock(&announce_lock);
	}

end:
	fprintf(stderr, "announce: Stopping\n");

	if (sockfd != -1) {
		close(sockfd);
	}

	return NULL;
}


int
mb_announce_start()
{
	assert(announce_stop == 1);

	announce_stop = 0;
	if (pthread_create(&announce_thread, NULL, mb_announce_worker, NULL) < 0) {
		fprintf(stderr, "announce: Announcer thread failed\n");
		return -1;
	}
	return 0;
}


void
mb_announce_stop()
{
	assert(announce_stop == 0);

	announce_stop = 1;
	pthread_cond_signal(&announce_cond);
	pthread_join(announce_thread, NULL);
}

