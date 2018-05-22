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


#ifndef __MB_UI_TEXTVIEW_H__
#define __MB_UI_TEXTVIEW_H__


/**
 * Opaque textview widget structure.
 */
struct mb_ui_textview;


/**
 * Sets the textview text.
 */
int
mb_ui_textview_settext(struct mb_ui_textview * const inst,
	const char * const text);


/**
 * Repaint the widget.
 */
int
mb_ui_textview_update(struct mb_ui_textview * const inst);


/**
 * Creates a new textview widget.
 */
struct mb_ui_textview *
mb_ui_textview_new(struct avbox_window * const parent,
	const char * const text, const int x, const int y,
	const int w, const int h);


/**
 * Destroys a textview widget.
 */
void
mb_ui_textview_destroy(const struct mb_ui_textview *inst);


#endif
