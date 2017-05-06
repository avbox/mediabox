/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define LOG_MODULE		"announce"

#include "lib/log.h"
#include "lib/debug.h"
#include "lib/timers.h"
#include "lib/iface_util.h"

#define MB_ANNOUNCE_INTERVAL  	(10)
#define MB_ANNOUNCE_PORT	(49550)

#define MB_FEATURES_NONE	(0x00)
#define MB_FEATURES_DLMASTER	(0x01)
#define MB_FEATURES_PLAYER	(0x02)
#define MB_FEATURES_SHAREDLIB	(0x04)



static int iface_index;
static int sockfd;
static int timerid;
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
 * Broadcast the interface address.
 */
static int
avbox_discovery_broadcast(const char * const iface_name, void *arg)
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
	const size_t len = strlen(ann);
	if (sendto(sockfd, &ann, len, 0, &addr, sizeof(addr)) == -1 ||
		sendto(sockfd, &ann, len, 0, &addr, sizeof(addr)) == -1 ||
		sendto(sockfd, &ann, len, 0, &addr, sizeof(addr)) == -1) {
		LOG_VPRINT_ERROR("Could not broadcast announcement: %s",
			strerror(errno));
	}

	return -1;
}


/**
 * Send broadcast for each interface.
 */
static enum avbox_timer_result
avbox_discovery_sendbroadcast(int timer_id, void *data)
{
	iface_index = 0;
	ifaceutil_enumifaces(avbox_discovery_broadcast, NULL);
	return AVBOX_TIMER_CALLBACK_RESULT_CONTINUE;
}


/**
 * Start the announce service.
 */
int
avbox_discovery_init(void)
{
	int broadcast = 1;
	struct timespec tv;

	/* create socket */
	if ((sockfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		LOG_VPRINT_ERROR("Could not create socket: %s",
			strerror(errno));
		return -1;
	}

	/* enable broadcast */
	if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) == -1) {
		LOG_VPRINT_ERROR("Could not set socket's SO_BROADCAST: %s",
			strerror(errno));
		close(sockfd);
		return sockfd = -1;
	}

	/* bind socket to broadcast address */
	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = PF_INET;
	addr.sin_port = htons(MB_ANNOUNCE_PORT);
	addr.sin_addr.s_addr = htonl(-1);
	memset(addr.sin_zero, 0, sizeof(addr.sin_zero));
	if (bind(sockfd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		LOG_VPRINT_ERROR("Could not bind socket: %s",
			strerror(errno));
		close(sockfd);
		return sockfd = -1;
	}

	/* send the first announcement immediately */
	avbox_discovery_sendbroadcast(0, NULL);

	/* register a timer to send announcements */
	tv.tv_sec = MB_ANNOUNCE_INTERVAL;
	tv.tv_nsec = 0;
	if ((timerid = avbox_timer_register(&tv, AVBOX_TIMER_TYPE_AUTORELOAD,
		NULL, avbox_discovery_sendbroadcast, NULL)) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "announce", "Could not register timer");
		close(sockfd);
		return sockfd = -1;
	}

	return 0;
}


void
avbox_discovery_shutdown(void)
{
	/* cancel timer before closing socket */
	if (timerid != -1) {
		avbox_timer_cancel(timerid);
		timerid = -1;
	}
	if (sockfd != -1) {
		close(sockfd);
		sockfd = -1;
	}
}

