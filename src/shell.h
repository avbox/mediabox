/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_SHELL_H__
#define __MB_SHELL_H__


/**
 * mbs_get_active_player() -- Gets the currently active player instance.
 */
struct avbox_player *
mbox_shell_getactiveplayer(void);


/**
 * Gets the shell's message queue fd.
 */
int
mbox_shell_getqueue(void);


/**
 * Initialize the MediaBox shell
 */
int
mbox_shell_init(int lauch_avmount, int launch_mediatomb);


int
mbox_shell_show(void);


void
mbox_shell_reboot(void);

#endif
