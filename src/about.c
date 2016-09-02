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


static struct mbv_window *window = NULL;
static int window_height, window_width;


/**
 * mb_about_init() -- Initialize the MediaBox about box.
 */
int
mb_about_init(void)
{
	int xres, yres;
	int font_height;
	int n_entries = 6;

	if (mb_su_canroot()) {
		n_entries++;
	}

	/* set height according to font size */
	mbv_getscreensize(&xres, &yres);
	font_height = mbv_getdefaultfontheight();
	window_height = 30 + font_height + ((font_height + 10) * n_entries);

	/* set width according to screen size */
	switch (xres) {
	case 1024: window_width = 500; break;
	case 1280: window_width = 900; break;
	case 1920: window_width = 700; break;
	case 640:
	default:   window_width = 400; break;
	}

	/* create a new window for the menu dialog */
	window = mbv_window_new(NULL,
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height);
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
	mbi_event e;
	cairo_t *context;

	PangoLayout *layout;
	PangoFontDescription *font_desc;

	if ((context = mbv_window_cairo_begin(window)) != NULL) {

		cairo_translate(context, 0, 20);

		if ((layout = pango_cairo_create_layout(context)) != NULL) {
			if ((font_desc = pango_font_description_from_string("Sans Bold 36px")) != NULL) {
				const char *text = "MEDIABOX v" PACKAGE_VERSION "\n\n"
					"Copyright (c) 2016 - Fernando Rodriguez\n"
					"All rights reserved.\n\n"
					"This software uses code of FFmpeg licensed "
					"under the LGPLv2.1";
				pango_layout_set_font_description(layout, font_desc);
				pango_font_description_free(font_desc);
				pango_layout_set_width(layout, window_width * PANGO_SCALE);
				pango_layout_set_height(layout, window_height * PANGO_SCALE);
				pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
				pango_layout_set_text(layout, text, -1);

				cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
				pango_cairo_update_layout(context, layout);
				pango_cairo_show_layout(context, layout);
			} else {
				DEBUG_PRINT("about", "Could not create font descriptor");
			}
			g_object_unref(layout);
		}
		mbv_window_cairo_end(window);
	} else {
		DEBUG_PRINT("about", "Could not get cairo context");
	}

	/* show the menu window */
        mbv_window_show(window);

	/* grab the input device */
	if ((fd = mbi_grab_input()) == -1) {
		fprintf(stderr, "mbs_about_showdialog() -- mbi_grab_input failed\n");
		return -1;
	}

	/* wait for any input */
	(void) read_or_eof(fd, &e, sizeof(mbi_event));

	/* hide the mainmenu window */
	mbv_window_hide(window);
	close(fd);

	return 0;
}


void
mb_about_destroy(void)
{
	/* fprintf(stderr, "about: Destroying instance\n"); */

	mbv_window_destroy(window);
}

