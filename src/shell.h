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
avbox_shell_getactiveplayer(void);


/**
 * Gets the shell's message queue fd.
 */
int
avbox_shell_getqueue(void);


/**
 * Initialize the MediaBox shell
 */
int
avbox_shell_init(void);


int
avbox_shell_run(void);


void
avbox_shell_reboot(void);


void
avbox_shell_shutdown(void);

#endif
