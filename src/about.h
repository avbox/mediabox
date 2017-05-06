#ifndef __MB_ABOUT_H__
#define __MB_ABOUT_H__
#include "lib/dispatch.h"


struct mbox_about;


/**
 * Initialize the MediaBox about box.
 */
struct mbox_about*
mbox_about_new(struct avbox_dispatch_object *parent);


int
mbox_about_show(struct mbox_about *inst);


void
mbox_about_destroy(struct mbox_about *inst);

#endif
