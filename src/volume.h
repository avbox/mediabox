/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __ALSA_VOLUME_H__
#define __ALSA_VOLUME_H__


int
avbox_volume_get(void);


int
avbox_volume_set(int volume);


int
avbox_volume_init(int msgfd);


void
avbox_volume_shutdown(void);

#endif
