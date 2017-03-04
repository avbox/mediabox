/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __ALSA_VOLUME_H__
#define __ALSA_VOLUME_H__


int
mb_alsa_volume_get(void);


int
mb_alsa_volume_set(int volume);


int
mb_alsa_volume_init(int msgfd);


void
mb_alsa_volume_destroy(void);


#endif
