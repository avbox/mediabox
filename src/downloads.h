/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_DOWNLOADS_H__
#define __MB_DOWNLOADS_H__
#include "lib/dispatch.h"


struct mbox_downloads;


/**
 * Initialize the MediaBox downloads list
 */
struct mbox_downloads*
mbox_downloads_new(struct avbox_object *parent);


int
mbox_downloads_show(struct mbox_downloads * const inst);


void
mbox_downloads_destroy(struct mbox_downloads * const inst);

#endif
