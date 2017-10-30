/**
 * MediaBox - Linux based set-top firmware
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
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
