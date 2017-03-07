/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include "video.h"
#include "log.h"
#include "debug.h"


struct mb_ui_progressbar
{
	struct mbv_window *window;
	int value;
	int min;
	int max;
};


/**
 * Set the progressbar value.
 */
int
mb_ui_progressbar_setvalue(struct mb_ui_progressbar *inst, int value)
{
	assert(inst != NULL);

	if (value < inst->min || value > inst->max) {
		errno = EINVAL;
		return -1;
	}

	inst->value = value;

	return 0;
}


/**
 * Repaint the progressbar.
 */
static int
mb_ui_progressbar_paint(struct mbv_window *window)
{
	int bar_width;
	int w, h;
	struct mb_ui_progressbar *inst = (struct mb_ui_progressbar*)
		mbv_window_getusercontext(window);

	assert(inst != NULL);
	assert(inst->window != NULL);
	assert(inst->min == 0);	/* TODO: Support min < 0 */

	mbv_window_getcanvassize(inst->window, &w, &h);

	bar_width = (w * inst->value) / inst->max;

	mbv_window_setbgcolor(inst->window, MBV_DEFAULT_BACKGROUND);
	mbv_window_setcolor(inst->window, MBV_DEFAULT_FOREGROUND);
	mbv_window_clear(inst->window);
	mbv_window_fillrectangle(inst->window, 0, 0, bar_width, h);

	return 1;
}


int
mb_ui_progressbar_update(struct mb_ui_progressbar *inst)
{
	mbv_window_update(inst->window);
	return 0;
}

void
mb_ui_progressbar_show(struct mb_ui_progressbar *inst)
{
	assert(inst != NULL);
	assert(inst->window != NULL);

	mbv_window_show(inst->window);
}


/**
 * mb_ui_progressbar_new() -- Create new progressbar instance.
 */
struct mb_ui_progressbar *
mb_ui_progressbar_new(struct mbv_window *parent,
	int x, int y, int w, int h,
	int min, int max, int value)
{
	struct mb_ui_progressbar *inst;

	/* allocate buffer for instance struct */
	if ((inst = malloc(sizeof(struct mb_ui_progressbar))) == NULL) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "ui-progressbar",
			"Could not initialize instance. Out of memory");
		return NULL;
	}

	/* create widget window */
	if ((inst->window = mbv_window_getchildwindow(parent,
		"ui.progressbar", x, y, w, h, &mb_ui_progressbar_paint, inst)) == NULL) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "ui-progressbar",
			"Could not create window");
		free(inst);
		return NULL;
	}

	inst->min = min;
	inst->max = max;
	inst->value = value;

	return inst;
}


/**
 * mb_ui_progressbar_destroy() -- Destroy the progressbar widget.
 */
void
mb_ui_progressbar_destroy(struct mb_ui_progressbar *inst)
{
	assert(inst != NULL);

	mbv_window_destroy(inst->window);
	free(inst);
}
