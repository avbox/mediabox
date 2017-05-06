/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
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
