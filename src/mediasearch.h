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


#ifndef __MB_MEDIASEARCH_H__
#define __MB_MEDIASEARCH_H__

#include "lib/dispatch.h"


struct mbox_mediasearch;

/**
 * Initialize the MediaBox "Find Media" dialog.
 */
struct mbox_mediasearch*
mbox_mediasearch_new(struct avbox_object *parent);


int
mbox_mediasearch_show(struct mbox_mediasearch *inst);


void
mbox_mediasearch_destroy(struct mbox_mediasearch *inst);

#endif
