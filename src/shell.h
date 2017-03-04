/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_SHELL_H__
#define __MB_SHELL_H__


/**
 * mbs_get_active_player() -- Gets the currently active player instance.
 */
struct mbp *
mbs_get_active_player(void);


/**
 * Gets the shell's message queue fd.
 */
int
mbs_getqueue(void);


/**
 * mbs_init() -- Initialize the MediaBox shell
 */
int
mbs_init(void);


int
mbs_showdialog(void);


void
mbs_reboot(void);


void
mbs_destroy(void);

#endif
