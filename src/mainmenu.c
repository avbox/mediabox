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

#define LOG_MODULE "mainmenu"

#include "lib/log.h"
#include "lib/debug.h"
#include "lib/dispatch.h"
#include "lib/ui/video.h"
#include "lib/ui/listview.h"
#include "lib/ui/input.h"
#include "library.h"
#include "lib/su.h"
#include "shell.h"
#include "about.h"
#include "downloads.h"
#include "mediasearch.h"


/**
 * Main menu.
 */
struct mbox_mainmenu
{
	struct avbox_window *window;
	struct avbox_dispatch_object *dispatch_object;
	struct avbox_dispatch_object *notify_object;
	struct avbox_listview *menu;
	struct mbox_library *library;
	struct mbox_about *about;
	struct mbox_downloads *downloads;
	struct mbox_mediasearch *search;
};


static void
mbox_mainmenu_dismiss(struct mbox_mainmenu *inst)
{
	avbox_listview_releasefocus(inst->menu);
	avbox_window_hide(inst->window);

	/* send dismissed message to parent */
	if (avbox_dispatch_sendmsg(-1, &inst->notify_object,
		AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
		LOG_VPRINT_ERROR("Could not send dismissed message: %s",
			strerror(errno));
	}
}


/**
 * Handle incoming messages.
 */
static int
mbox_mainmenu_messagehandler(void *context, struct avbox_message *msg)
{
	struct mbox_mainmenu * const inst = context;

	switch (avbox_dispatch_getmsgtype(msg)) {
	case AVBOX_MESSAGETYPE_SELECTED:
	{
		void * const payload =
			avbox_dispatch_getmsgpayload(msg);

		DEBUG_PRINT("mainmenu", "Received SELECTED message");

		if (payload == inst->menu) {
			char *selected = avbox_listview_getselected(inst->menu);

			assert(selected != NULL);

			if (!memcmp("LIB", selected, 4)) {
				if ((inst->library = mbox_library_new(inst->dispatch_object)) == NULL) {
					LOG_PRINT_ERROR("Could not initialize library!");
				} else {
					if (mbox_library_show(inst->library) == -1) {
						LOG_PRINT_ERROR("Could not show library!");
						mbox_library_destroy(inst->library);
						inst->library = NULL;
					}
				}
			} else if (!memcmp("REBOOT", selected, 6)) {
				mbox_shell_reboot();

			} else if (!memcmp("ABOUT", selected, 5)) {
				if (inst->about != NULL) {
					DEBUG_PRINT("mainmenu", "About dialog already visible!");
				} else {
					if ((inst->about = mbox_about_new(inst->dispatch_object)) == NULL) {
						LOG_PRINT_ERROR("Could not create about box!");
					} else {
						if (mbox_about_show(inst->about) == -1) {
							LOG_PRINT_ERROR("Could not show about box!");
							mbox_about_destroy(inst->about);
							inst->about = NULL;
						}
					}
				}
			} else if (!memcmp("DOWN", selected, 4)) {
				if (inst->downloads != NULL) {
					DEBUG_PRINT("mainmenu", "Downloads already visible!");
				} else {
					if ((inst->downloads = mbox_downloads_new(inst->dispatch_object)) == NULL) {
						LOG_PRINT_ERROR("Could not create downloads window!");
					} else {
						if (mbox_downloads_show(inst->downloads) == -1) {
							LOG_PRINT_ERROR("Could not show downloads window!");
							mbox_downloads_destroy(inst->downloads);
							inst->downloads = NULL;
						}
					}
				}
			} else if (!memcmp("MEDIASEARCH", selected, 11)) {
				if (inst->search != NULL) {
					DEBUG_PRINT("mainmenu", "Search already visible!");
				} else {
					if ((inst->search = mbox_mediasearch_new(inst->dispatch_object)) == NULL) {
						LOG_PRINT_ERROR("Could not create search window!");
					} else {
						if (mbox_mediasearch_show(inst->search) == -1) {
							LOG_PRINT_ERROR("Could not show search window!");
							mbox_mediasearch_destroy(inst->search);
							inst->search = NULL;
						}
					}
				}
			} else {
				DEBUG_VPRINT("mainmenu", "Selected %s", selected);
			}
		} else {
			DEBUG_VABORT("mainmenu", "Received SELECTED message with invalid payload: %p",
				payload);
		}

		break;
	}
	case AVBOX_MESSAGETYPE_DISMISSED:
	{
		DEBUG_PRINT("mainmenu", "Received DISMISSED message");

		void * const payload =
			avbox_dispatch_getmsgpayload(msg);

		assert(payload != NULL);

		if (payload == inst->menu) {
			mbox_mainmenu_dismiss(inst);
		} else {
			if (payload == inst->library) {
				assert(inst->library != NULL);
				mbox_library_destroy(inst->library);
				inst->library = NULL;
				mbox_mainmenu_dismiss(inst);

			} else if (payload == inst->about) {
				DEBUG_PRINT("mainmenu", "Destroying about box");
				assert(inst->about != NULL);
				mbox_about_destroy(inst->about);
				inst->about = NULL;
			} else if (payload == inst->downloads) {
				assert(inst->downloads != NULL);
				mbox_downloads_destroy(inst->downloads);
				inst->downloads = NULL;
			} else if (payload == inst->search) {
				assert(inst->search != NULL);
				mbox_mediasearch_destroy(inst->search);
				inst->search = NULL;
			} else {
				DEBUG_VABORT("mediasearch", "Unexpected DISMISSED message: %p",
					payload);
			}

			/* the window compositor is not complete yet
			 * and will not properly redraw this window after
			 * the child window has been dismissed so for now
			 * we just dismiss this one to */
			avbox_window_update(inst->window);

#if 0
			/* send dismissed message to parent */
			if (avbox_dispatch_sendmsg(-1, &inst->notify_object,
				AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
				LOG_VPRINT_ERROR("Could not send dismissed message: %s",
					strerror(errno));
			}
#endif
		}
		break;
	}
	default:
		return AVBOX_DISPATCH_CONTINUE;
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Initialize the MediaBox menu
 */
struct mbox_mainmenu *
mbox_mainmenu_new(struct avbox_dispatch_object *notify_object)
{
	struct mbox_mainmenu *inst;
	int xres, yres;
	int font_height;
	int window_height, window_width;
	int n_entries = 8;

	/* allocate memory for main menu */
	if ((inst = malloc(sizeof(struct mbox_mainmenu))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

#ifdef ENABLE_REBOOT
	if (mb_su_canroot()) {
		n_entries++;
	}
#endif

	/* set height according to font size */
	avbox_window_getcanvassize(avbox_video_getrootwindow(0), &xres, &yres);
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
	inst->window = avbox_window_new(NULL, "mainmenu",
		AVBOX_WNDFLAGS_DECORATED,
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height, NULL, NULL, NULL);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create new window!");
		free(inst);
		return NULL;
	}
	if (avbox_window_settitle(inst->window, "MAIN MENU") == -1) {
		LOG_VPRINT_ERROR("Could not set window title: %s",
			strerror(errno));
		avbox_window_destroy(inst->window);
		free(inst);
	}

	/* create dispatch object */
	if ((inst->dispatch_object = avbox_dispatch_createobject(
		mbox_mainmenu_messagehandler, 0, inst)) == NULL) {
		LOG_VPRINT_ERROR("Could not create dispatch object: %s",
			strerror(errno));
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	/* create a new menu widget inside main window */
	if ((inst->menu = avbox_listview_new(inst->window, inst->dispatch_object)) == NULL) {
		LOG_VPRINT_ERROR("Could not create menu widget (errno=%i)",
			errno);
		avbox_dispatch_destroyobject(inst->dispatch_object);
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	/* initialize instance object */
	inst->notify_object = notify_object;
	inst->library = NULL;
	inst->search = NULL;
	inst->about = NULL;
	inst->downloads = NULL;

	/* populate the list */
	if (avbox_listview_additem(inst->menu, "MEDIA LIBRARY", "LIB") == -1 ||
		avbox_listview_additem(inst->menu, "OPTICAL DISC", "DVD") == -1 ||
		avbox_listview_additem(inst->menu, "TV TUNNER", "DVR") == -1 ||
		avbox_listview_additem(inst->menu, "DOWNLOADS", "DOWN") == -1 ||
		avbox_listview_additem(inst->menu, "MEDIA SEARCH", "MEDIASEARCH") == -1 ||
		avbox_listview_additem(inst->menu, "GAMING CONSOLES", "CONSOLES") == -1 ||
		avbox_listview_additem(inst->menu, "SETTINGS", "SETTINGS") == -1 ||
		avbox_listview_additem(inst->menu, "ABOUT MEDIABOX", "ABOUT") == -1) {
		LOG_PRINT_ERROR("Could not populate list!");
		avbox_listview_destroy(inst->menu);
		avbox_dispatch_destroyobject(inst->dispatch_object);
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

#ifdef ENABLE_REBOOT
	if (mb_su_canroot()) {
		avbox_listview_additem(menu, "REBOOT", "REBOOT");
	}
#endif

	return inst;
}


/**
 * Show window.
 */
int
mbox_mainmenu_show(struct mbox_mainmenu * const inst)
{
	/* show the menu window */
        avbox_window_show(inst->window);

	/* give focus to the menu */
	if (avbox_listview_focus(inst->menu) == -1) {
		DEBUG_PRINT("mainmenu", "Could not show dialog!");
		return -1;
	}

	return 0;
}


/**
 * Destroy the main menu.
 */
void
mbox_mainmenu_destroy(struct mbox_mainmenu * const inst)
{
	DEBUG_PRINT("mainmenu", "Destroying object");

	if (inst->library != NULL) {
		mbox_library_destroy(inst->library);
	}
	if (inst->search != NULL) {
		mbox_mediasearch_destroy(inst->search);
	}
	if (inst->downloads != NULL) {
		mbox_downloads_destroy(inst->downloads);
	}
	if (inst->about != NULL) {
		mbox_about_destroy(inst->about);
	}

	avbox_listview_destroy(inst->menu);
	avbox_dispatch_destroyobject(inst->dispatch_object);
	avbox_window_destroy(inst->window);
	free(inst);
}
