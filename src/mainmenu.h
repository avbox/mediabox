/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_MAINMENU_H__
#define __MB_MAINMENU_H__
#include "lib/dispatch.h"

struct mbox_mainmenu;


/**
 * Initialize the MediaBox menu
 */
struct mbox_mainmenu*
mbox_mainmenu_new(struct avbox_object *parent);


int
mbox_mainmenu_show(struct mbox_mainmenu * const inst);


void
mbox_mainmenu_destroy(struct mbox_mainmenu * const inst);

#endif
