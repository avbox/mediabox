/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __INPUT_SOCKET_H__
#define __INPUT_SOCKET_H__
#include "../linkedlist.h"

struct conn_state;


typedef void (*socket_closed_callback)(struct conn_state *state);


LISTABLE_STRUCT(conn_state,
	int fd;
	int quit;
	pthread_t thread;
	socket_closed_callback closed_callback;
);


void *
mbi_socket_connection(void *arg);

#endif
