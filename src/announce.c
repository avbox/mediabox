/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define LOG_MODULE		"announce"

#include "debug.h"
#include "iface_util.h"
#include "timers.h"
#include "debug.h"
#include "log.h"

#define MB_ANNOUNCE_INTERVAL  	(10)
#define MB_ANNOUNCE_PORT	(49550)

#define MB_FEATURES_NONE	(0x00)
#define MB_FEATURES_DLMASTER	(0x01)
#define MB_FEATURES_PLAYER	(0x02)
#define MB_FEATURES_SHAREDLIB	(0x04)



static int iface_index;
static int sockfd;
static struct sockaddr_in addr;


static const char *
genid(void)
{
	int i, j;
	static char id[13];
	const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
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


/**
 * mb_broadcast_address() -- Broadcast the interface address.
 */
static int
mb_broadcast_address(const char * const iface_name, void *arg)
{
	char ann[512];
	char *ip;
	const char * id = genid();

	if (!strcmp("lo", iface_name)) {
		return 0;
	}

	if ((ip = ifaceutil_getip(iface_name)) == NULL) {
		return 0;
	}

	/* initialize announcement structure */
	snprintf(ann, 512, "MediaBox:%s:%s.%i:%s:DLMASTER,PLAYER,SHAREDLIB",
		id, "mediabox", iface_index++, ip);
	free(ip);

	/* broadcast announcement 3 times */
	if (sendto(sockfd, &ann, sizeof(ann), 0, &addr, sizeof(addr)) == -1 ||
		sendto(sockfd, &ann, sizeof(ann), 0, &addr, sizeof(addr)) == -1 ||
		sendto(sockfd, &ann, sizeof(ann), 0, &addr, sizeof(addr)) == -1) {
		fprintf(stderr, "announce: Could not broadcast announcement. errno=%i\n", errno);
	}

	/* fprintf(stderr, "announce: Sent %s\n", ann); */

	return -1;
}


/**
 * mb_announce_sendbroadcast() -- Send broadcast for each interface.
 */
static enum mbt_result
mb_announce_sendbroadcast(int timer_id, void *data)
{
	iface_index = 0;
	ifaceutil_enumifaces(mb_broadcast_address, NULL);
	return MB_TIMER_CALLBACK_RESULT_CONTINUE;
}


/**
 * mb_announce_start() -- Start the announce service.
 */
int
mb_announce_start(void)
{
	int broadcast = 1;
	struct timespec tv;

	sockfd = -1;

	/* create socket */
	if ((sockfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		LOG_ERROR();
		return -1;
	}

	/* enable broadcast */
	if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) == -1) {
		LOG_ERROR();
		close(sockfd);
		return sockfd = -1;
	}

	/* bind socket to broadcast address */
	addr.sin_family = PF_INET;
	addr.sin_port = htons(MB_ANNOUNCE_PORT);
	addr.sin_addr.s_addr = htonl(-1);
	memset(addr.sin_zero, 0, sizeof(addr.sin_zero));
	if (bind(sockfd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		LOG_ERROR();
		close(sockfd);
		return sockfd = -1;
	}

	/* send the first announcement immediately */
	mb_announce_sendbroadcast(0, NULL);

	/* register a timer to send announcements */
	tv.tv_sec = MB_ANNOUNCE_INTERVAL;
	tv.tv_nsec = 0;
	if (mbt_register(&tv, MB_TIMER_TYPE_AUTORELOAD, -1, mb_announce_sendbroadcast, NULL) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "announce", "Could not register timer");
		close(sockfd);
		return sockfd = -1;
	}

	return 0;
}


void
mb_announce_stop(void)
{
	if (sockfd != -1) {
		close(sockfd);
	}
}

