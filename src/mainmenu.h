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


#ifndef __MB_MAINMENU_H__
#define __MB_MAINMENU_H__

#include <libavbox/dispatch.h>

struct mbox_mainmenu;


/**
 * Initialize the MediaBox menu
 */
struct mbox_mainmenu*
mbox_mainmenu_new(struct avbox_object *parent);


/**
 * Get the underlying window
 */
struct avbox_window *
mbox_mainmenu_window(struct mbox_mainmenu * const inst);


int
mbox_mainmenu_show(struct mbox_mainmenu * const inst);


#endif
