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


#include "video.h"
#include "input.h"
#include "ui-menu.h"
#include "library.h"
#include "su.h"
#include "shell.h"
#include "about.h"
#include "downloads.h"
#include "mediasearch.h"
#include "debug.h"
#include "log.h"


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
	int n_entries = 8;

#ifdef ENABLE_REBOOT
	if (mb_su_canroot()) {
		n_entries++;
	}
#endif

	/* set height according to font size */
	mbv_window_getcanvassize(mbv_getrootwindow(), &xres, &yres);
	font_height = mbv_getdefaultfontheight();
	window_height = 3 + font_height + ((font_height + 3) * n_entries);

	DEBUG_VPRINT("mainmenu", "Default font size: %i", font_height);

	/* set width according to screen size */
	switch (xres) {
	case 1024: window_width =  400; break;
	case 1280: window_width =  800; break;
	case 1920: window_width = 1024; break;
	case 640:
	default:   window_width = 300; break;
	}

	/* create a new window for the menu dialog */
	window = mbv_window_new("mainmenu", "MAIN MENU",
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height, NULL);
	if (window == NULL) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "mainmenu",
			"Could not create new window!");
		return -1;
	}

	/* create a new menu widget inside main window */
	menu = mb_ui_menu_new(window);
	if (menu == NULL) {
		LOG_VPRINT(MB_LOGLEVEL_ERROR, "mainmenu",
			"Could not create menu widget (errno=%i)", errno);
		return -1;
	}

	/* populate the menu */
	mb_ui_menu_additem(menu, "MEDIA LIBRARY", "LIB");
	mb_ui_menu_additem(menu, "OPTICAL DISC", "DVD");
	mb_ui_menu_additem(menu, "TV TUNNER", "DVR");
	mb_ui_menu_additem(menu, "DOWNLOADS", "DOWN");
	mb_ui_menu_additem(menu, "MEDIA SEARCH", "MEDIASEARCH");
	mb_ui_menu_additem(menu, "GAMING CONSOLES", "CONSOLES");
	mb_ui_menu_additem(menu, "SETTINGS", "SETTINGS");
	mb_ui_menu_additem(menu, "ABOUT MEDIABOX", "ABOUT");

#ifdef ENABLE_REBOOT
	if (mb_su_canroot()) {
		mb_ui_menu_additem(menu, "REBOOT", "REBOOT");
	}
#endif

	return 0;
}


int
mb_mainmenu_showdialog(void)
{
	int quit = 0;

	DEBUG_PRINT("mainmenu", "Showing dialog");

	/* show the menu window */
        mbv_window_show(window);

	while (!quit) {
		/* show the menu widget and run it's input loop */
		if (mb_ui_menu_showdialog(menu) == 0) {
			char *selected = mb_ui_menu_getselected(menu);

			assert(selected != NULL);

			if (!memcmp("LIB", selected, 4)) {
				mb_library_init();
				mbv_window_hide(window);
				if (mb_library_showdialog() == 0) {
					quit = 1;
					mb_library_destroy();
					break;
				} else {
					mb_library_destroy();
				}

			} else if (!memcmp("REBOOT", selected, 6)) {
				mbv_window_hide(window);
				avbox_shell_reboot();

			} else if (!memcmp("ABOUT", selected, 5)) {
				mb_about_init();
				mbv_window_hide(window);
				mb_about_showdialog();
				mb_about_destroy();

			} else if (!memcmp("DOWN", selected, 4)) {
				mb_downloads_init();
				mbv_window_hide(window);
				mb_downloads_showdialog();
				mb_downloads_destroy();

			} else if (!memcmp("MEDIASEARCH", selected, 11)) {
				mb_mediasearch_init();
				mbv_window_hide(window);
				mb_mediasearch_showdialog();
				mb_mediasearch_destroy();

			} else {
				DEBUG_VPRINT("mainmenu", "Selected %s", selected);
			}
		} else {
			break;
		}
	}

	/* hide the mainmenu window */
	mbv_window_hide(window);

	return 0;
}


void
mb_mainmenu_destroy(void)
{
	DEBUG_PRINT("mainmenu", "Destroying object");

	mb_ui_menu_destroy(menu);
	mbv_window_destroy(window);
}

