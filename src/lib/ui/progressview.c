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


#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include "video.h"
#include "../log.h"
#include "../debug.h"


struct avbox_progressview
{
	struct avbox_window *window;
	int value;
	int min;
	int max;
};


/**
 * Set the progressbar value.
 */
int
avbox_progressview_setvalue(struct avbox_progressview *inst, int value)
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
avbox_progressview_paint(struct avbox_window *window)
{
	int bar_width;
	int w, h;
	struct avbox_progressview *inst = (struct avbox_progressview*)
		avbox_window_getusercontext(window);

	assert(inst != NULL);
	assert(inst->window != NULL);
	assert(inst->min == 0);	/* TODO: Support min < 0 */

	avbox_window_getcanvassize(inst->window, &w, &h);

	bar_width = (w * inst->value) / inst->max;

	avbox_window_setbgcolor(inst->window, MBV_DEFAULT_BACKGROUND);
	avbox_window_setcolor(inst->window, MBV_DEFAULT_FOREGROUND);
	avbox_window_clear(inst->window);
	avbox_window_fillrectangle(inst->window, 0, 0, bar_width, h);

	return 1;
}


int
avbox_progressview_update(struct avbox_progressview *inst)
{
	avbox_window_update(inst->window);
	return 0;
}


void
avbox_progressview_show(struct avbox_progressview *inst)
{
	assert(inst != NULL);
	assert(inst->window != NULL);

	avbox_window_show(inst->window);
}


/**
 * Create new progressbar instance.
 */
struct avbox_progressview *
avbox_progressview_new(struct avbox_window *parent,
	int x, int y, int w, int h,
	int min, int max, int value)
{
	struct avbox_progressview *inst;

	/* allocate buffer for instance struct */
	if ((inst = malloc(sizeof(struct avbox_progressview))) == NULL) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "ui-progressbar",
			"Could not initialize instance. Out of memory");
		return NULL;
	}

	/* create widget window */
	if ((inst->window = avbox_window_new(parent,
		"ui.progressbar", AVBOX_WNDFLAGS_SUBWINDOW, x, y, w, h,
		NULL, &avbox_progressview_paint, inst)) == NULL) {
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
 * Destroy the progressbar widget.
 */
void
avbox_progressview_destroy(struct avbox_progressview *inst)
{
	assert(inst != NULL);
	avbox_window_destroy(inst->window);
	free(inst);
}
