/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
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
