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

#define LOG_MODULE "a2dp"

#include "lib/debug.h"
#include "lib/dispatch.h"
#include "lib/bluetooth.h"
#include "lib/process.h"
#include "lib/ui/video.h"
#include "lib/ui/input.h"

#define BT_A2DP_UUID	"0000110a-0000-1000-8000-00805f9b34fb"
#define BLUEALSA_APLAY  "/usr/bin/bluealsa-aplay"


struct mbox_a2dp
{
	struct avbox_window *window;
	struct avbox_object *parent_object;
	struct avbox_btdev *device;
	int player_process_id;
	int w;
	int h;
	int dirty;
};


/**
 * Handles manual drawing of the a2dp window
 */
static int
mbox_a2dp_draw(struct avbox_window * const window)
{
	cairo_t *context;
	PangoLayout *layout;
	struct mbox_a2dp * const inst =
		avbox_window_getusercontext(window);
	char msg[2048];

	assert(inst != NULL);

	if (!inst->dirty) {
		return 0;
	}

	if (inst->device != NULL) {
		snprintf(msg, sizeof(msg), "Connected to %s",
			inst->device->name);
	} else {
		snprintf(msg, sizeof(msg), "No device connected");
	}

	avbox_window_clear(window);

	if ((context = avbox_window_cairo_begin(window)) != NULL) {

		cairo_translate(context, 0, 20);

		if ((layout = pango_cairo_create_layout(context)) != NULL) {
			char text[2048];
			snprintf(text, sizeof(text),
				"MEDIABOX v" PACKAGE_VERSION "\n\n"
				"Bluetooth Speaker (A2DP) Activated\n\n%s",
				msg);

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
		DEBUG_PRINT("a2dp", "Could not get cairo context");
	}
	inst->dirty = 0;
	return 1;
}


static int
mbox_a2dp_msghandler(void *context, struct avbox_message *msg)
{
	struct mbox_a2dp * const inst = context;

	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_INPUT:
	{
		struct avbox_input_message * const ev =
			avbox_message_payload(msg);

		DEBUG_PRINT("a2dp", "Hiding window");

		switch (ev->msg) {
		case MBI_EVENT_BACK:
		{
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

			return AVBOX_DISPATCH_OK;
		}
		default:
			return AVBOX_DISPATCH_CONTINUE;
		}

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
		DEBUG_PRINT("a2dp", "Unexpected message!");
		return AVBOX_DISPATCH_CONTINUE;
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Initialize the MediaBox a2dp dialog.
 */
struct mbox_a2dp*
mbox_a2dp_new(struct avbox_object *parent)
{
	int xres, yres;
	int font_height;
	struct mbox_a2dp *inst;

	/* allocate memory */
	if ((inst = malloc(sizeof(struct mbox_a2dp))) == NULL) {
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
	inst->window = avbox_window_new(NULL, "a2dp",
		AVBOX_WNDFLAGS_INPUT,
		(xres / 2) - (inst->w / 2),
		(yres / 2) - (inst->h / 2),
		inst->w,
		inst->h,
		mbox_a2dp_msghandler,
		mbox_a2dp_draw,
		inst);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create window!");
		free(inst);
		return NULL;
	}

	/* initialize object fields */
	inst->dirty = 1;
	inst->parent_object = parent;
	inst->device = NULL;
	inst->player_process_id = -1;
	return inst;
}


/**
 * Show window
 */
int
mbox_a2dp_show(struct mbox_a2dp * const inst)
{
	struct avbox_btdev **devices, **dev;

	DEBUG_PRINT("a2dp", "Showing window");

	/* show the window */
        avbox_window_show(inst->window);

	/* get all the devices that implement the
	 * a2dp profile */
	devices = dev = avbox_bluetooth_getdevices(BT_A2DP_UUID);

	/* iterate through the list until we find a
	 * device that is connected. */
	while (*dev != NULL) {
		if ((*dev)->connected) {
			DEBUG_VPRINT("mainmenu", "%s (%s) Connected: %s",
				(*dev)->name, (*dev)->address,
				(*dev)->connected ? "yes" : "no");
			if (inst->device == NULL) {
				inst->device = *dev;
			} else {
				avbox_bluetooth_freedev(*dev);
			}
		} else {
			avbox_bluetooth_freedev(*dev);
		}
		dev++;
	}
	free(devices);

	/* if there is a connected A2DP device then
	 * start playing from it */
	if (inst->device != NULL) {
		const char * const args[] =
		{
			BLUEALSA_APLAY,
			inst->device->address,
			NULL
		};

		/* update the window */
		inst->dirty = 1;
		avbox_window_update(inst->window);

		/* start the bluealsa-aplay process.
		 * TODO: Use the built-in media player instead */
		if ((inst->player_process_id = avbox_process_start(BLUEALSA_APLAY, args,
			AVBOX_PROCESS_AUTORESTART | AVBOX_PROCESS_NICE | AVBOX_PROCESS_IONICE_IDLE |
			AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_SIGKILL,
			"bluealsa-play", NULL, NULL)) == -1) {
			LOG_VPRINT_ERROR("Could not start bluealsa-aplay!",
				strerror(errno));
			return -1;
		}
	}

	return 0;
}


/**
 * Destroy the a2dp box.
 */
void
mbox_a2dp_destroy(struct mbox_a2dp * const inst)
{
	avbox_window_destroy(inst->window);
	if (inst->device != NULL) {
		avbox_bluetooth_freedev(inst->device);
	}
	if (inst->player_process_id != -1) {
		avbox_process_stop(inst->player_process_id);
	}
}
