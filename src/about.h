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


#ifndef __MBOX_ABOUT_H__
#define __MBOX_ABOUT_H__

#include "lib/dispatch.h"


struct mbox_about;


/**
 * Initialize the MediaBox about box.
 */
struct mbox_about*
mbox_about_new(struct avbox_object * const parent);


/**
 * Get the underlying window.
 */
struct avbox_window *
mbox_about_window(const struct mbox_about * const inst);


#endif
