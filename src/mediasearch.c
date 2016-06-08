#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>


#include "video.h"
#include "input.h"
#include "ui-menu.h"


static struct mbv_window *window = NULL;
static struct mb_ui_menu *menu = NULL;


/**
 * mb_downloads_init() -- Initialize the MediaBox downloads list
 */
int
mb_mediasearch_init(void)
{
	int xres, yres;
	int font_height;
	int window_height, window_width;
	int n_entries = 10;

	/* set height according to font size */
	mbv_getscreensize(&xres, &yres);
	font_height = mbv_getdefaultfontheight();
	window_height = 30 + font_height + ((font_height + 10) * n_entries);

	/* set width according to screen size */
	switch (xres) {
	case 1024: window_width = 800; break;
	case 1280: window_width = 1000; break;
	case 1920: window_width = 1200; break;
	case 640:
	default:   window_width = 600; break;
	}

	/* create a new window for the menu dialog */
	window = mbv_window_new("FIND MEDIA",
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height);
	if (window == NULL) {
		fprintf(stderr, "mediasearch: Could not create new window!\n");
		return -1;
	}

	/* create a new menu widget inside main window */
	menu = mb_ui_menu_new(window);
	if (menu == NULL) {
		fprintf(stderr, "mediasearch: Could not create menu\n");
		return -1;
	}

	return 0;
}


int
mb_mediasearch_showdialog(void)
{
	/* show the menu window */
        mbv_window_show(window);

	/* show the menu widget and run it's input loop */
	if (mb_ui_menu_showdialog(menu) == 0) {
		char *selected = mb_ui_menu_getselected(menu);

		assert(selected != NULL);

		fprintf(stderr, "mediasearch: Selected %s\n",
			selected);
	}

	return 0;
}


void
mb_mediasearch_destroy(void)
{
	fprintf(stderr, "mediasearch: Destroying instance\n");
	mb_ui_menu_destroy(menu);
	mbv_window_destroy(window);
}

