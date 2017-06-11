#ifndef __MB_MEDIASEARCH_H__
#define __MB_MEDIASEARCH_H__
#include "lib/dispatch.h"


struct mbox_mediasearch;

/**
 * Initialize the MediaBox "Find Media" dialog.
 */
struct mbox_mediasearch*
mbox_mediasearch_new(struct avbox_object *parent);


int
mbox_mediasearch_show(struct mbox_mediasearch *inst);


void
mbox_mediasearch_destroy(struct mbox_mediasearch *inst);

#endif
