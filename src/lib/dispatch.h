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


#ifndef __AVBOX_DISPATCH__
#define __AVBOX_DISPATCH__
#include <pthread.h>

/*
 * Dispatch flags.
 */
#define AVBOX_DISPATCH_UNICAST		(0)	/* unicast */
#define AVBOX_DISPATCH_BROADCAST	(1)
#define AVBOX_DISPATCH_MULTICAST	(2)
#define AVBOX_DISPATCH_ANYCAST		(4)
#define AVBOX_DISPATCH_EXPECT_REPLY	(8)


/*
 * Message types.
 */
#define AVBOX_MESSAGETYPE_INPUT		(0x01)
#define AVBOX_MESSAGETYPE_TIMER		(0x02)
#define AVBOX_MESSAGETYPE_EXCEPTION	(0x03)
#define AVBOX_MESSAGETYPE_UI		(0x04)
#define AVBOX_MESSAGETYPE_SYSTEM	(0x05)
#define AVBOX_MESSAGETYPE_DELEGATE	(0x06)
#define AVBOX_MESSAGETYPE_PLAYER	(0x07)
#define AVBOX_MESSAGETYPE_VOLUME	(0x08)
#define AVBOX_MESSAGETYPE_SELECTED	(0x09)
#define AVBOX_MESSAGETYPE_DISMISSED	(0x0A)
#define AVBOX_MESSAGETYPE_DESTROY	(0x0C)
#define AVBOX_MESSAGETYPE_CLEANUP	(0x0D)
#define AVBOX_MESSAGETYPE_USER		(0xFF)

#define AVBOX_DISPATCH_OK		(0)
#define AVBOX_DISPATCH_CONTINUE		(1)


struct avbox_object;
struct avbox_message;


/**
 * Message handler function.
 */
typedef int (*avbox_message_handler)(void *context, struct avbox_message *msg);


/**
 * Get the type of a message
 */
int
avbox_message_id(struct avbox_message * const msg);


/**
 * Close the dispatch queue.
 */
void
avbox_dispatch_close(void);


/**
 * Get the message payload.
 */
void *
avbox_message_payload(struct avbox_message * const msg);


/**
 * Get the next message on the thread's queue.
 */
struct avbox_message*
avbox_dispatch_getmsg(void);


/**
 * Peek the next message in the thread's queue.
 */
struct avbox_message *
avbox_dispatch_peekmsg(void);


/**
 * Sends a message.
 */
struct avbox_message*
avbox_object_sendmsg(struct avbox_object **dest,
	int type, int flags, void * const payload);


/**
 * Create a dispatch object.
 */
struct avbox_object*
avbox_object_new(avbox_message_handler handler, void *context);


/**
 * Destroy dispatch object.
 */
void
avbox_object_destroy(struct avbox_object *obj);


/**
 * Initialize dispatch subsystem.
 */
int
avbox_dispatch_init();


/**
 * Dispatch a message.
 */
void
avbox_message_dispatch(struct avbox_message *msg);


/**
 * Shutdown dispatch subsystem.
 */
void
avbox_dispatch_shutdown();

#endif
