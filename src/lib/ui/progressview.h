/**
 * avbox - Toolkit for Embedded Multimedia Applications
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


#ifndef __UI_PROGRESSBAR_H__
#define __UI_PROGRESSBAR_H__

#include "video.h"


/* Abstract progressbar type */
struct avbox_progressview;



void
avbox_progressview_show(struct avbox_progressview *inst);


/**
 * Set the progressbar value.
 */
int
avbox_progressview_setvalue(struct avbox_progressview *inst, int value);


/**
 * Repaint the progressbar.
 */
int
avbox_progressview_update(struct avbox_progressview *inst);


/**
 * Create new progressbar instance.
 */
struct avbox_progressview *
avbox_progressview_new(struct avbox_window *parent,
	int x, int y, int w, int h,
	int min, int max, int value);


/**
 * mb_ui_progressbar_destroy() -- Destroy the progressbar widget.
 */
void
avbox_progressview_destroy(struct avbox_progressview *inst);

#endif
