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


#include "video.h"
#include "input.h"
#include "ui-menu.h"
#include "library.h"
#include "su.h"
#include "shell.h"
#include "debug.h"


static struct mbv_window *window;
static int window_height, window_width;
static int dirty;


/**
 * Handles manual drawing of the about window
 */
static int
mb_about_paint(struct mbv_window * const window)
{
	cairo_t *context;
	PangoLayout *layout;

	if (!dirty) {
		return 0;
	}

	mbv_window_clear(window);

	if ((context = mbv_window_cairo_begin(window)) != NULL) {

		cairo_translate(context, 0, 20);

		if ((layout = pango_cairo_create_layout(context)) != NULL) {
			const char *text = "MEDIABOX v" PACKAGE_VERSION "\n\n"
				"Copyright (c) 2016 - Fernando Rodriguez\n"
				"All rights reserved.\n\n"
				"This software uses code of FFmpeg licensed "
				"under the LGPLv2.1";
			pango_layout_set_font_description(layout, mbv_getdefaultfont());
			pango_layout_set_width(layout, window_width * PANGO_SCALE);
			pango_layout_set_height(layout, window_height * PANGO_SCALE);
			pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
			pango_layout_set_text(layout, text, -1);

			cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
			pango_cairo_update_layout(context, layout);
			pango_cairo_show_layout(context, layout);
			g_object_unref(layout);
		}
		mbv_window_cairo_end(window);
	} else {
		DEBUG_PRINT("about", "Could not get cairo context");
	}
	dirty = 0;
	return 1;
}


/**
 * mb_about_init() -- Initialize the MediaBox about box.
 */
int
mb_about_init(void)
{
	int xres, yres;
	int font_height;

	/* set height according to font size */
	mbv_window_getcanvassize(mbv_getrootwindow(), &xres, &yres);
	font_height = mbv_getdefaultfontheight();
	window_height = 30 + font_height + ((font_height + 10) * 6);
	dirty = 0;

	/* set width according to screen size */
	switch (xres) {
	case 1024: window_width = 500; break;
	case 1280: window_width = 900; break;
	case 1920: window_width = 700; break;
	case 640:
	default:   window_width = 400; break;
	}

	/* create a new window for the menu dialog */
	window = mbv_window_new("about", NULL,
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height, &mb_about_paint);
	if (window == NULL) {
		fprintf(stderr, "mb_mainmenu: Could not create new window!\n");
		return -1;
	}

	return 0;
}


int
mb_about_showdialog(void)
{
	int fd;
	enum avbox_input_event e;
	/* show the menu window */
	dirty = 1;
        mbv_window_show(window);

	/* grab the input device */
	if ((fd = avbox_input_grab()) == -1) {
		fprintf(stderr, "mbs_about_showdialog() -- mbi_grab_input failed\n");
		return -1;
	}

	/* wait for any input */
	avbox_input_getevent(fd, &e);

	/* hide the mainmenu window */
	mbv_window_hide(window);
	close(fd);

	return 0;
}


void
mb_about_destroy(void)
{
	mbv_window_destroy(window);
}

