#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>


#include "video.h"
#include "input.h"
#include "ui-menu.h"
#include "library.h"
#include "su.h"
#include "shell.h"


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
	case 1280: window_width = 600; break;
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

	mbv_window_drawstring(window, "MEDIABOX " PACKAGE_VERSION, window_width / 2, 10);
	mbv_window_drawstring(window, "Copyright (c) 2016 - Fernando Rodriguez", window_width / 2, 60);
	mbv_window_drawstring(window, "All rights reserved", window_width / 2, 80);

	mbv_window_drawstring(window, "This software uses code of FFmpeg licensed", window_width / 2, 120);
	mbv_window_drawstring(window, "under the GPLv2.1", window_width / 2, 140);

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
	fprintf(stderr, "about: Destroying instance\n");

	mbv_window_destroy(window);
}

