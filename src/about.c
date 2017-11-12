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
	struct avbox_object *parent_object;
	int w;
	int h;
	int dirty;
};


/**
 * Handles manual drawing of the about window
 */
static int
mbox_about_draw(struct avbox_window * const window, void * ctx)
{
	cairo_t *context;
	PangoLayout *layout;
	struct mbox_about * const inst = (struct mbox_about *) ctx;

	ASSERT(inst != NULL);

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

	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_INPUT:
	{
		struct avbox_input_message * const ev =
			avbox_message_payload(msg);

		DEBUG_PRINT("about", "Hiding window");

		/* hide the window */
		avbox_window_hide(inst->window);

		/* send DISMISSED message */
		if (avbox_object_sendmsg(&inst->parent_object,
			AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
			LOG_VPRINT_ERROR("Could not send dismissed message: %s",
				strerror(errno));
		}

		/* free input event */
		avbox_input_eventfree(ev);
		break;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
		if (avbox_window_isvisible(inst->window)) {
			avbox_window_hide(inst->window);
		}
		break;
	case AVBOX_MESSAGETYPE_CLEANUP:
		free(inst);
		break;
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
mbox_about_new(struct avbox_object * const parent)
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
		AVBOX_WNDFLAGS_INPUT,
		(xres / 2) - (inst->w / 2),
		(yres / 2) - (inst->h / 2),
		inst->w,
		inst->h,
		mbox_about_msghandler,
		mbox_about_draw,
		inst);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create window!");
		free(inst);
		return NULL;
	}

	/* initialize object fields */
	inst->dirty = 1;
	inst->parent_object = parent;
	return inst;
}


/**
 * Get the underlying window.
 */
struct avbox_window *
mbox_about_window(const struct mbox_about * const inst)
{
	return inst->window;
}
