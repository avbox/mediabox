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
#include "iface_util.h"

#define MB_ANNOUNCE_INTERVAL  	(10)
#define MB_ANNOUNCE_PORT	(49550)

#define MB_FEATURES_NONE	(0x00)
#define MB_FEATURES_DLMASTER	(0x01)
#define MB_FEATURES_PLAYER	(0x02)
#define MB_FEATURES_SHAREDLIB	(0x04)

struct bcast_context
{
	int index;
	int sockfd;
	struct sockaddr_in addr;
};

static pthread_t announce_thread;
static pthread_mutex_t announce_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t announce_cond = PTHREAD_COND_INITIALIZER;
static int announce_stop = 1;

static const char *
genid(void)
{
	int i, j;
	static char id[13];
	const char const charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	static int gen = 0;
	if (gen == 1) {
		return id;
	}
	for(i = 0; i < 12; i++) {
		j = (int) ((double) rand() / RAND_MAX * (sizeof(charset) - 1));
		id[i] = charset[j];
	}
	id[12] = '\0';
	gen = 1;
	return id;
}



static int
mb_broadcast_address(const char * const iface_name, void *arg)
{
	char ann[512];
	char *ip;
	struct bcast_context *ctx = (struct bcast_context*) arg;
	const char * id = genid();


	if (!strcmp("lo", iface_name)) {
		return 0;
	}

	if ((ip = ifaceutil_getip(iface_name)) == NULL) {
		return 0;
	}

	/* initialize announcement structure */
	snprintf(ann, 512, "MediaBox:%s:%s.%i:%s:DLMASTER,PLAYER,SHAREDLIB",
		id, "mediabox", ctx->index++, ip);
	free(ip);

	/* broadcast announcement 3 times */
	if (sendto(ctx->sockfd, &ann, sizeof(ann), 0, &ctx->addr, sizeof(ctx->addr)) == -1 ||
		sendto(ctx->sockfd, &ann, sizeof(ann), 0, &ctx->addr, sizeof(ctx->addr)) == -1 ||
		sendto(ctx->sockfd, &ann, sizeof(ann), 0, &ctx->addr, sizeof(ctx->addr)) == -1) {
		fprintf(stderr, "announce: Could not broadcast announcement. errno=%i\n", errno);
	}

	fprintf(stderr, "announce: Sent %s\n", ann);

	return -1;
}

void *
mb_announce_worker(void *arg)
{
	int broadcast = 1;
	struct timespec tv;
	struct timeval now;
	struct bcast_context ctx;

	(void) arg;

	MB_DEBUG_SET_THREAD_NAME("announcer");

	fprintf(stderr, "announce: Starting\n");

	ctx.sockfd = -1;

	/* create socket */
	if ((ctx.sockfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		fprintf(stderr, "announce: Could not create socket\n");
		goto end;
	}

	/* enable broadcast */
	if (setsockopt(ctx.sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) == -1) {
		fprintf(stderr, "announce: setsockopt() failed\n");
		goto end;
	}

	/* bind socket to broadcast address */
	ctx.addr.sin_family = PF_INET;
	ctx.addr.sin_port = htons(MB_ANNOUNCE_PORT);
	ctx.addr.sin_addr.s_addr = htonl(-1);
	memset(ctx.addr.sin_zero, 0, sizeof(ctx.addr.sin_zero));
	if (bind(ctx.sockfd, (struct sockaddr *) &ctx.addr, sizeof(ctx.addr)) == -1) {
		fprintf(stderr, "announce: bind() failed. errno=%i\n", errno);
		goto end;
	}

	/* broadcast announcement every MB_ANNOUNCE_INTERVAL seconds */
	while (!announce_stop) {
		/* broadcast announcement for each valid interface */
		ctx.index = 0;
		ifaceutil_enumifaces(mb_broadcast_address, &ctx);

		gettimeofday(&now, NULL);
		tv.tv_sec = now.tv_sec + MB_ANNOUNCE_INTERVAL;
		pthread_mutex_lock(&announce_lock);
		pthread_cond_timedwait(&announce_cond, &announce_lock, &tv);
		pthread_mutex_unlock(&announce_lock);
	}

end:
	fprintf(stderr, "announce: Stopping\n");

	if (ctx.sockfd != -1) {
		close(ctx.sockfd);
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

