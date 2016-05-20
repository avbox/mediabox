#include <stdlib.h>
#include <directfb.h>

#include "input.h"

extern IDirectFB *dfb;
static struct mbi *instance;

int
mbi_directfb_init(struct mbi *inst)
{
	instance = inst;

	/* make sure directfb has been initialized */
	if (dfb == NULL) {
		fprintf(stderr, "mbi_directfb_init() failed!\n");
		return -1;
	}

	/* send test event */
	mbi_event_send(inst, MBI_EVENT_PLAY);

	return 0;
}

