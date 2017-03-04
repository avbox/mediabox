/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_SU_H__
#define __MB_SU_H__

#include <unistd.h>
#include <pwd.h>


int
mb_su_canroot(void);


int
mb_su_gainroot(void);


void
mb_su_droproot(void);

#endif
