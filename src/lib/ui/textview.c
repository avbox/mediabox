/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#define LOG_MODULE "textview"

#include "video.h"
#include "../log.h"
#include "../debug.h"


struct mb_ui_textview
{
	struct avbox_window *window;
	const char *text;
};


static int
mb_ui_textview_repaint(struct avbox_window *window)
{
	int w, h;
	cairo_t *context;
	PangoLayout *layout;
	struct mb_ui_textview * const inst = (struct mb_ui_textview*)
		avbox_window_getusercontext(window);

	/* if there's nothing to draw return success */
	if (inst->text == NULL || strlen(inst->text) == 0) {
		return 0;
	}

	/* get the size of the widget window */
	avbox_window_getcanvassize(inst->window, &w, &h);

	/* clear the window */
	avbox_window_clear(inst->window);

	/* get the cairo context for it */
	if ((context = avbox_window_cairo_begin(inst->window)) == NULL) {
		LOG_PRINT_ERROR("Could not create cairo context");
		return -1;
	}

	/* create a PangoLayout object */
	if ((layout = pango_cairo_create_layout(context)) == NULL) {
		LOG_PRINT_ERROR("Could not create pango layout");
		goto end;
	}

	/* initialize layout */
	pango_layout_set_font_description(layout, mbv_getdefaultfont());
	pango_layout_set_width(layout, w * PANGO_SCALE);
	pango_layout_set_height(layout, h * PANGO_SCALE);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	pango_layout_set_text(layout, inst->text, -1);

	/* render the layout */
	cairo_move_to(context, 0, 0);
	cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(avbox_window_getcolor(inst->window)));
	pango_cairo_update_layout(context, layout);
	pango_cairo_show_layout(context, layout);

	/* cleanup */
	g_object_unref(layout);

end:
	avbox_window_cairo_end(inst->window);

	return 1;
}

/**
 * Sets the textview text.
 */
int
mb_ui_textview_settext(struct mb_ui_textview * const inst,
	const char * const text)
{
	assert(inst != NULL);

	if (inst->text != NULL) {
		free((void*) inst->text);
	}
	if ((inst->text = strdup(text)) == NULL) {
		LOG_PRINT_ERROR("Could not set text");
		return -1;
	}
	return 0;
}


/**
 * Repaint the widget.
 */
int
mb_ui_textview_update(struct mb_ui_textview * const inst)
{
	assert(inst != NULL);
	assert(inst->window != NULL);

	/* if there's nothing to draw return success */
	if (inst->text == NULL || strlen(inst->text) == 0) {
		return 0;
	}

	avbox_window_update(inst->window);
	return 0;
}


/**
 * Creates a new textview widget.
 */
struct mb_ui_textview *
mb_ui_textview_new(struct avbox_window * const parent,
	const char * const text, const int x, const int y,
	const int w, const int h)
{
	struct mb_ui_textview *inst;

	assert(parent != NULL);

	/* allocate the textview object */
	if ((inst = malloc(sizeof(struct mb_ui_textview))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate textview object");
		errno = ENOMEM;
		return NULL;
	}

	/* create a sub-window for the widget */
	if ((inst->window = avbox_window_new(parent,
		"textview", AVBOX_WNDFLAGS_SUBWINDOW, x, y, w, h,
		NULL, &mb_ui_textview_repaint, inst)) == NULL) {
		LOG_PRINT_ERROR("Could not create widget window");
		free(inst);
		errno = EFAULT;
		return NULL;
	}

	/* allocate a copy of the text (if any) */
	if (text != NULL && (inst->text = strdup(text)) == NULL) {
		LOG_PRINT_ERROR("Could not allocate textview object. strdup() failed");
		avbox_window_destroy(inst->window);
		free(inst);
		errno = ENOMEM;
		return NULL;
	}

	return inst;
}


/**
 * Destroys a textview widget.
 */
void
mb_ui_textview_destroy(const struct mb_ui_textview *inst)
{
	if (inst->text != NULL) {
		free((void*) inst->text);
	}
	free((void*) inst);
}
