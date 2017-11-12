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


#ifndef __MBOX_OVERLAY_H__
#define __MBOX_OVERLAY_H__

#define MBOX_OVERLAY_STATE_READY	(0)
#define MBOX_OVERLAY_STATE_PLAYING	(1)
#define MBOX_OVERLAY_STATE_PAUSED 	(2)


struct mbox_overlay;
struct avbox_player;

/**
 * Show the overlay.
 */
void
mbox_overlay_show(struct mbox_overlay * const inst, int secs);


/**
 * Create an overlay instance.
 */
struct mbox_overlay *
mbox_overlay_new(struct avbox_player * player);


/**
 * Get the underlying window.
 */
struct avbox_window *
mbox_overlay_window(const struct mbox_overlay * const inst);


#endif
