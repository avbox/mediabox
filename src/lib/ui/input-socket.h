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


#ifndef __INPUT_SOCKET_H__
#define __INPUT_SOCKET_H__

#include <pthread.h>

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
