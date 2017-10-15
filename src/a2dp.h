#ifndef __MB_A2DP_H__
#define __MB_A2DP_H__
#include "lib/dispatch.h"


struct mbox_a2dp;


/**
 * Initialize the MediaBox about box.
 */
struct mbox_a2dp*
mbox_a2dp_new(struct avbox_object *parent);


int
mbox_a2dp_show(struct mbox_a2dp *inst);


void
mbox_a2dp_destroy(struct mbox_a2dp *inst);

#endif
