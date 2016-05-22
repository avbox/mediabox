#ifndef __MB_INPUT_H__
#define __MB_INPUT_H__

#include <pthread.h>

#include "pipe_util.h"
#include "linkedlist.h"

typedef enum
{
	MBI_EVENT_PLAY,
	MBI_EVENT_PAUSE,
	MBI_EVENT_STOP,
	MBI_EVENT_MENU,
	MBI_EVENT_BACK,
	MBI_EVENT_ENTER,
	MBI_EVENT_ARROW_UP,
	MBI_EVENT_ARROW_DOWN,
	MBI_EVENT_ARROW_LEFT,
	MBI_EVENT_ARROW_RIGHT,
	MBI_EVENT_EXIT
}
mbi_event;

void
mbi_event_send(mbi_event e);

/**
 * mbi_grab_input() -- Returns a file descriptor to a pipe where
 * all input events will be sent until the file descriptor is closed,
 * in which case the prior descriptor is closed or until mbi_grab_input()
 * is called again
 */
int
mbi_grab_input(void);

int
mbi_init(void);

void
mbi_destroy(void);

#endif

