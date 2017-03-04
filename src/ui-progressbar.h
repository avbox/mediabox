/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __UI_PROGRESSBAR_H__
#define __UI_PROGRESSBAR_H__


/* Abstract progressbar type */
struct mb_ui_progressbar;



void
mb_ui_progressbar_show(struct mb_ui_progressbar *inst);


/**
 * mb_ui_progressbar_setvalue() -- Set the progressbar value.
 */
int
mb_ui_progressbar_setvalue(struct mb_ui_progressbar *inst, int value);


/**
 * mb_ui_progressbar_update() -- Repaint the progressbar.
 */
int
mb_ui_progressbar_update(struct mb_ui_progressbar *inst);


/**
 * mb_ui_progressbar_new() -- Create new progressbar instance.
 */
struct mb_ui_progressbar *
mb_ui_progressbar_new(struct mbv_window *parent,
	int x, int y, int w, int h,
	int min, int max, int value);


/**
 * mb_ui_progressbar_destroy() -- Destroy the progressbar widget.
 */
void
mb_ui_progressbar_destroy(struct mb_ui_progressbar *inst);

#endif
