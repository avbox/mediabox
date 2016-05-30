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


static struct mbv_window *window = NULL;
static struct mb_ui_menu *menu = NULL;


/**
 * mbm_init() -- Initialize the MediaBox menu
 */
int
mb_mainmenu_init(void)
{
	int xres, yres;
	int font_height;
	int window_height, window_width;

	/* set height according to font size */
	mbv_getscreensize(&xres, &yres);
	font_height = mbv_getdefaultfontheight();
	window_height = 30 + font_height + ((font_height + 10) * 4);

	/* set width according to screen size */
	switch (xres) {
	case 640:  window_width = 300; break;
	case 1024: window_width = 400; break;
	case 1280: window_width = 500; break;
	case 1920: window_width = 600; break;
	}

	/* create a new window for the menu dialog */
	window = mbv_window_new("MAIN MENU",
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height);
	if (window == NULL) {
		fprintf(stderr, "mb_mainmenu: Could not create new window!\n");
		return -1;
	}

	/* create a new menu widget inside main window */
	menu = mb_ui_menu_new(window);
	if (menu == NULL) {
		fprintf(stderr, "mb_mainmenu: Could not create menu\n");
		return -1;
	}

	/* populate the menu */
	mb_ui_menu_additem(menu, "MEDIA LIBRARY", "LIB");
	mb_ui_menu_additem(menu, "DVD PLAYER", "DVD");
	mb_ui_menu_additem(menu, "DIGITAL VIDEO RECORDER", "DVR");
	mb_ui_menu_additem(menu, "PIRATE BOX", "PIR");

	return 0;
}


int
mb_mainmenu_showdialog(void)
{
	/* show the menu window */
        mbv_window_show(window);

	/* show the menu widget and run it's input loop */
	if (mb_ui_menu_showdialog(menu) == 0) {
		char *selected = mb_ui_menu_getselected(menu);

		assert(selected != NULL);

		if (!memcmp("LIB", selected, 4)) {
			mb_library_init();
			mbv_window_hide(window);
			mb_library_showdialog();
			mbv_window_show(window);
			mb_library_destroy();
		} else {
			fprintf(stderr, "mb_mainmenu: Selected %s\n",
				selected);
		}
	}

	/* hide the mainmenu window */
	mbv_window_hide(window);

	return 0;
}


void
mb_mainmenu_destroy(void)
{
	fprintf(stderr, "mb_mainmenu: Destroying instance\n");

	mb_ui_menu_destroy(menu);
	mbv_window_destroy(window);
}

