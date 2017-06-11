/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_LIBRARY_H__
#define __MB_LIBRARY_H__
#include "lib/dispatch.h"


struct mbox_library;


/**
 *Initialize the MediaBox menu
 */
struct mbox_library*
mbox_library_new(struct avbox_object *parent);


int
mbox_library_show(struct mbox_library * const inst);


void
mbox_library_destroy(struct mbox_library * const inst);


#endif
