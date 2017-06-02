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
#define AVBOX_MESSAGETYPE_QUIT		(0x0B)
#define AVBOX_MESSAGETYPE_USER		(0xFF)

#define AVBOX_DISPATCH_OK		(0)
#define AVBOX_DISPATCH_CONTINUE		(1)


struct avbox_dispatch_object;
struct avbox_message;


/**
 * Message handler function.
 */
typedef int (*avbox_message_handler)(void *context, struct avbox_message *msg);


/**
 * Swap two endpoints.
 */
void
avbox_dispatch_swapep(struct avbox_dispatch_object *a,
	struct avbox_dispatch_object *b);


/**
 * Get the type of a message
 */
int
avbox_dispatch_getmsgtype(struct avbox_message *msg);


/**
 * Get the message payload.
 */
void *
avbox_dispatch_getmsgpayload(struct avbox_message *msg);


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
avbox_dispatch_sendmsg(const int tid, struct avbox_dispatch_object **dest,
	int type, int flags, void *payload);


/**
 * Create a dispatch object.
 */
struct avbox_dispatch_object*
avbox_dispatch_createobject(avbox_message_handler handler, int flags, void *context);


/**
 * Destroy dispatch object.
 */
void
avbox_dispatch_destroyobject(struct avbox_dispatch_object *obj);


/**
 * Initialize dispatch subsystem.
 */
int
avbox_dispatch_init();


/**
 * Dispatch a message.
 */
void
avbox_dispatch_dispatchmsg(struct avbox_message *msg);


/**
 * Shutdown dispatch subsystem.
 */
void
avbox_dispatch_shutdown();

#endif