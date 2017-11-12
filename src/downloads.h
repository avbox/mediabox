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


#ifndef __MB_DOWNLOADS_H__
#define __MB_DOWNLOADS_H__

#include "lib/dispatch.h"


struct mbox_downloads;


/**
 * Initialize the MediaBox downloads list
 */
struct mbox_downloads*
mbox_downloads_new(struct avbox_object *parent);


int
mbox_downloads_show(struct mbox_downloads * const inst);


/**
 * Gets the underlying window.
 */
struct avbox_window *
mbox_downloads_window(const struct mbox_downloads * const inst);


#endif
