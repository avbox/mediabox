/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_SU_H__
#define __MB_SU_H__

#include <unistd.h>
#include <pwd.h>


int
avbox_canroot(void);


int
avbox_gainroot(void);


void
avbox_droproot(void);

#endif
