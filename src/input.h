#ifndef __MB_INPUT_H__
#define __MB_INPUT_H__

#include <pthread.h>

#include "pipe_util.h"
#include "linkedlist.h"

typedef enum
{
	MBI_EVENT_NONE,
	MBI_EVENT_PLAY,
	MBI_EVENT_PAUSE,
	MBI_EVENT_STOP,
	MBI_EVENT_MENU,
	MBI_EVENT_BACK,
	MBI_EVENT_ENTER,
	MBI_EVENT_NEXT,
	MBI_EVENT_PREV,
	MBI_EVENT_ARROW_UP,
	MBI_EVENT_ARROW_DOWN,
	MBI_EVENT_ARROW_LEFT,
	MBI_EVENT_ARROW_RIGHT,
	MBI_EVENT_CLEAR,
	MBI_EVENT_KBD_A,
	MBI_EVENT_KBD_B,
	MBI_EVENT_KBD_C,
	MBI_EVENT_KBD_D,
	MBI_EVENT_KBD_E,
	MBI_EVENT_KBD_F,
	MBI_EVENT_KBD_G,
	MBI_EVENT_KBD_H,
	MBI_EVENT_KBD_I,
	MBI_EVENT_KBD_J,
	MBI_EVENT_KBD_K,
	MBI_EVENT_KBD_L,
	MBI_EVENT_KBD_M,
	MBI_EVENT_KBD_N,
	MBI_EVENT_KBD_O,
	MBI_EVENT_KBD_P,
	MBI_EVENT_KBD_Q,
	MBI_EVENT_KBD_R,
	MBI_EVENT_KBD_S,
	MBI_EVENT_KBD_T,
	MBI_EVENT_KBD_U,
	MBI_EVENT_KBD_V,
	MBI_EVENT_KBD_W,
	MBI_EVENT_KBD_X,
	MBI_EVENT_KBD_Y,
	MBI_EVENT_KBD_Z,
	MBI_EVENT_KBD_SPACE,
	MBI_EVENT_EXIT,
	MBI_EVENT_QUIT,
}
mbi_event;

void
mbi_event_send(mbi_event e);


int
mbi_dispatchevent(mbi_event e);


/**
 * mbi_grab_input() -- Returns a file descriptor to a pipe where
 * all input events will be sent until the file descriptor is closed,
 * in which case the prior descriptor is closed or until mbi_grab_input()
 * is called again
 */
int
mbi_grab_input(void);


int
mbi_grab_input_nonblock(void);


int
mbi_init(void);

void
mbi_destroy(void);

#endif

