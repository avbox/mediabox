/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pango/pangocairo.h>

#define LOG_MODULE "about"

#include "lib/debug.h"
#include "lib/dispatch.h"
#include "lib/ui/video.h"
#include "lib/ui/input.h"


struct mbox_about
{
	struct avbox_window *window;
	struct avbox_dispatch_object *dispatch_object;
	struct avbox_dispatch_object *parent_object;
	int w;
	int h;
	int dirty;
};


/**
 * Handles manual drawing of the about window
 */
static int
mbox_about_draw(struct avbox_window * const window)
{
	cairo_t *context;
	PangoLayout *layout;
	struct mbox_about * const inst =
		avbox_window_getusercontext(window);

	assert(inst != NULL);

	if (!inst->dirty) {
		return 0;
	}

	avbox_window_clear(window);

	if ((context = avbox_window_cairo_begin(window)) != NULL) {

		cairo_translate(context, 0, 20);

		if ((layout = pango_cairo_create_layout(context)) != NULL) {
			const char *text = "MEDIABOX v" PACKAGE_VERSION "\n\n"
				"Copyright (c) 2016 - Fernando Rodriguez\n"
				"All rights reserved.\n\n"
				"This software uses code of FFmpeg licensed "
				"under the LGPLv2.1";
			pango_layout_set_font_description(layout, mbv_getdefaultfont());
			pango_layout_set_width(layout, inst->w * PANGO_SCALE);
			pango_layout_set_height(layout, inst->h * PANGO_SCALE);
			pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
			pango_layout_set_text(layout, text, -1);

			cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
			pango_cairo_update_layout(context, layout);
			pango_cairo_show_layout(context, layout);
			g_object_unref(layout);
		}
		avbox_window_cairo_end(window);
	} else {
		DEBUG_PRINT("about", "Could not get cairo context");
	}
	inst->dirty = 0;
	return 1;
}


static int
mbox_about_msghandler(void *context, struct avbox_message *msg)
{
	struct mbox_about * const inst = context;

	switch (avbox_dispatch_getmsgtype(msg)) {
	case AVBOX_MESSAGETYPE_INPUT:
	{
		struct avbox_input_message * const ev =
			avbox_dispatch_getmsgpayload(msg);

		DEBUG_PRINT("about", "Hiding window");

		/* hide the window */
		avbox_input_release(inst->dispatch_object);
		avbox_window_hide(inst->window);

		/* send DISMISSED message */
		if (avbox_dispatch_sendmsg(-1, &inst->parent_object,
			AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
			LOG_VPRINT_ERROR("Could not send dismissed message: %s",
				strerror(errno));
		}

		/* free input event */
		avbox_input_eventfree(ev);
		break;
	}
	default:
		DEBUG_PRINT("about", "Unexpected message!");
		return AVBOX_DISPATCH_CONTINUE;
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Initialize the MediaBox about box.
 */
struct mbox_about*
mbox_about_new(struct avbox_dispatch_object *parent)
{
	int xres, yres;
	int font_height;
	struct mbox_about *inst;

	/* allocate memory */
	if ((inst = malloc(sizeof(struct mbox_about))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	/* set height according to font size */
	avbox_window_getcanvassize(avbox_video_getrootwindow(0), &xres, &yres);
	font_height = mbv_getdefaultfontheight();
	inst->h = 30 + font_height + ((font_height + 10) * 6);
	inst->dirty = 0;

	/* set width according to screen size */
	switch (xres) {
	case 1024: inst->w = 500; break;
	case 1280: inst->w = 900; break;
	case 1920: inst->w = 700; break;
	case 640:
	default:   inst->w = 400; break;
	}

	/* create a new window for the menu dialog */
	inst->window = avbox_window_new(NULL, "about",
		AVBOX_WNDFLAGS_NONE,
		(xres / 2) - (inst->w / 2),
		(yres / 2) - (inst->h / 2),
		inst->w, inst->h, NULL, &mbox_about_draw, inst);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create window!");
		free(inst);
		return NULL;
	}

	/* create a dispatch object */
	if ((inst->dispatch_object = avbox_dispatch_createobject(
		mbox_about_msghandler, 0, inst)) == NULL) {
		LOG_VPRINT_ERROR("Could not create dispatch object: %s",
			strerror(errno));
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	/* initialize object fields */
	inst->dirty = 1;
	inst->parent_object = parent;
	return inst;
}


int
mbox_about_show(struct mbox_about * const inst)
{
	/* show the window */
        avbox_window_show(inst->window);

	/* grab focus */
	if (avbox_input_grab(inst->dispatch_object) == -1) {
		LOG_PRINT_ERROR("avbox_input_grab() failed!");
		return -1;
	}

	return 0;
}


/**
 * Destroy the about box.
 */
void
mbox_about_destroy(struct mbox_about * const inst)
{
	if (avbox_window_isvisible(inst->window)) {
		avbox_input_release(inst->dispatch_object);
		avbox_window_hide(inst->window);
	}
	avbox_dispatch_destroyobject(inst->dispatch_object);
	avbox_window_destroy(inst->window);
	free(inst);
}
