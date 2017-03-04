/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_DLBE_H__
#define __MB_DLBE_H__


int
mb_downloadmanager_addurl(char *url);


int
mb_downloadmanager_init(void);


void
mb_downloadmanager_destroy(void);

#endif
