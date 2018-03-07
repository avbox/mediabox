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

#define LOG_MODULE "downloads"

#include "lib/log.h"
#include "lib/debug.h"
#include "lib/dispatch.h"
#include "lib/process.h"
#include "lib/timers.h"
#include "lib/thread.h"
#include "lib/application.h"
#include "lib/ui/video.h"
#include "lib/ui/listview.h"
#include "lib/ui/input.h"
#include "lib/linkedlist.h"

#ifdef ENABLE_IONICE
#include "lib/ionice.h"
#include "lib/su.h"
#endif

#include "downloads-backend.h"


LISTABLE_STRUCT(mbox_download,
	char *id;
	char *name;
	int updated;
);

struct finditemdata
{
	int found;
	char *item;
	char *id;
};


struct mbox_downloads
{
	struct avbox_window *window;
	struct avbox_listview *menu;
	struct avbox_object *parent_object;
	struct avbox_delegate *worker;
	int update_timer_id;
	int destroying;
	LIST downloads;
};

struct mbox_downloads_update_context
{
	struct avbox_listview *list;
	char *id;
	char *name;
};


/**
 * Removes an item from the list from main thread.
 */
static void *
mbox_downloads_removeitem(void *arg)
{
	struct mbox_downloads_update_context * const ctx = arg;
	DEBUG_VPRINT("downloads", "Removing listview item %s",
		ctx->id);
	avbox_listview_removeitem(ctx->list, ctx->id);
	return NULL;
}


/**
 * Adds an entry to the list from the main thread.
 */
static void *
mbox_downloads_additem(void *arg)
{
	struct mbox_downloads_update_context * const ctx = arg;
	DEBUG_VPRINT("downloads", "Adding listview item (name=%s)",
		ctx->name);
	avbox_listview_additem(ctx->list, ctx->name, ctx->id);
	return NULL;
}


/**
 * Updates a listview entry from the main thread.
 */
static void *
mbox_downloads_updateitem(void *arg)
{
	struct mbox_downloads_update_context * const ctx = arg;
	avbox_listview_setitemtext(ctx->list, ctx->id, ctx->name);
	return NULL;
}


/**
 * Updates the window from the main thread.
 */
static void *
mbox_downloads_updatewindow(void *arg)
{
	struct avbox_window * const window = arg;
	avbox_window_update(window);
	return NULL;
}


/**
 * Updates an entry on the downloads list.
 * NOTE: This function is called by mbox_downloads_populatelistasync()
 * which is called from a background thread.
 */
static int
mbox_downloads_updateentry(struct mbox_downloads *inst, const char *id, char *name)
{
	int found = 0;
	struct mbox_download *dl;
	struct mbox_downloads_update_context ctx;
	struct avbox_delegate *del;

	LIST_FOREACH(struct mbox_download*, dl, &inst->downloads) {
		if (!strcmp(dl->id, id)) {
			found = 1;
			break;
		}
	}

	if (found) {
		assert(dl->name != NULL);
		free(dl->name);
		if ((dl->name = strdup(name)) == NULL) {
			fprintf(stderr, "downloads: Out of memory\n");
		}

		dl->updated = 1;

		/* update listview item from main thread */
		ctx.list = inst->menu;
		ctx.id = dl->id;
		ctx.name = name;
		if ((del = avbox_window_delegate(inst->window,
			mbox_downloads_updateitem, &ctx)) == NULL) {
			LOG_VPRINT_ERROR("Could not update item: %s",
				strerror(errno));
		} else {
			avbox_delegate_wait(del, NULL);
		}

	} else {
		if ((dl = malloc(sizeof(struct mbox_download))) == NULL) {
			fprintf(stderr, "downloads: Out of memory\n");
			return -1;
		}
		if ((dl->id = strdup(id)) == NULL) {
			fprintf(stderr, "downloads: Out of memory\n");
			free(dl);
			return -1;
		}
		if ((dl->name = strdup(name)) == NULL) {
			fprintf(stderr, "downloads: Out of memory\n");
			free(dl->id);
			free(dl);
			return -1;
		}

		dl->updated = 1;

		/* add item to in-memory list */
		LIST_ADD(&inst->downloads, dl);

		/* add entry to listview from main thread */
		ctx.list = inst->menu;
		ctx.name = dl->name;
		ctx.id = dl->id;
		if ((del = avbox_window_delegate(inst->window,
			mbox_downloads_additem, &ctx)) == NULL) {
			LOG_VPRINT_ERROR("Could not update entry: %s",
				strerror(errno));
		} else {
			avbox_delegate_wait(del, NULL);
		}
	}
	return 0;
}


/**
 * Free listview item.
 */
static int
mbox_downloads_freeitems(void *item, void *data)
{
	(void) data;
	free(item);
	return 0;
}


/**
 * Populates the downloads list from a background thread.
 */
static void*
mbox_downloads_populatelistasync(void *data)
{
	char buf[512];
	struct mbox_download *dl;
	struct mbox_downloads * const inst = data;
	struct avbox_delegate * del;
	struct mbox_dlman_download_item itemmem = {0};
	struct mbox_dlman_download_item *item = &itemmem;

	/* clear the updated flag on all items */
	LIST_FOREACH_SAFE(struct mbox_download*, dl, &inst->downloads, {
		dl->updated = 0;
	});

	/* update the in-memory list */
	while ((item = mbox_dlman_next(item)) != NULL) {
		snprintf(buf, 512, "%s (%i%%)",
			item->name, item->percent);
		mbox_downloads_updateentry(inst, item->id, buf);
		mbox_dlman_item_unref(item);
	}

	/* update all the items that where found and remove the
	 * rest */
	LIST_FOREACH_SAFE(struct mbox_download*, dl, &inst->downloads, {
		if (!dl->updated) {
			struct mbox_downloads_update_context ctx;
			ctx.list = inst->menu;
			ctx.id = dl->id;
			if ((del = avbox_window_delegate(inst->window,
				mbox_downloads_removeitem, &ctx)) == NULL) {
				LOG_VPRINT_ERROR("Could not remove entry: %s",
					strerror(errno));
			} else {
				avbox_delegate_wait(del, NULL);
			}
			LIST_REMOVE(dl);
			free(dl->id);
			free(dl->name);
			free(dl);
		} else {
			dl->updated = 0;
		}
	});

	/* update the window from the main thread */
	if ((del = avbox_window_delegate(inst->window,
		mbox_downloads_updatewindow, inst->window)) == NULL) {
		LOG_VPRINT_ERROR("Could not update window: %s",
			strerror(errno));
	} else {
		avbox_delegate_wait(del, NULL);
	}

	return 0;
}


/**
 * Manages the background thread that updates the list.
 */
static void
mbox_downloads_populatelist(int id, void *data)
{
	struct mbox_downloads * const inst = data;

	if (inst->destroying) {
		return;
	}

	/* if the background thread is still running */
	if (inst->worker != NULL) {
		/* check if it finished and collect result */
		if (avbox_delegate_finished(inst->worker)) {
			void *result;
			avbox_delegate_wait(inst->worker, &result);
			inst->worker = NULL;
			if ((intptr_t) result != 0) {
				avbox_timer_cancel(inst->update_timer_id);
				inst->update_timer_id = -1;
			}
		}
	} else {
		/* populate the list from a background thread */
		if ((inst->worker = avbox_workqueue_delegate(
			mbox_downloads_populatelistasync, data)) == NULL) {
			LOG_VPRINT_ERROR("Could not populate list: %s",
				strerror(errno));
		}
	}
}


/**
 * Handle incoming messages.
 */
static int
mbox_downloads_messagehandler(void *context, struct avbox_message *msg)
{
	struct mbox_downloads * const inst = context;

	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_SELECTED:
	{
#ifndef NDEBUG
		char *selected = avbox_listview_getselected(inst->menu);
		assert(selected != NULL);
		DEBUG_VPRINT("downloads", "Selected %s",
			selected);
#endif
		break;
	}
	case AVBOX_MESSAGETYPE_DISMISSED:
	{
		/* hide the downloads window */
		avbox_listview_releasefocus(inst->menu);
		avbox_window_hide(inst->window);

		/* send DISMISSED message */
		if (avbox_object_sendmsg(&inst->parent_object,
			AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
			LOG_VPRINT_ERROR("Could not send DISMISSED message: %s",
				strerror(errno));
		}

		break;
	}
	case AVBOX_MESSAGETYPE_TIMER:
	{
		struct avbox_timer_data * const timer_data =
			avbox_message_payload(msg);
		if (inst->update_timer_id == timer_data->id) {
			if (inst->destroying) {
				inst->update_timer_id = -1;
			} else {
				struct timespec tv;

				mbox_downloads_populatelist(timer_data->id, timer_data->data);

				/* register the update timer */
				tv.tv_sec = 2;
				tv.tv_nsec = 0;
				inst->update_timer_id = avbox_timer_register(&tv,
					AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
					avbox_window_object(inst->window), NULL, inst);
				if (inst->update_timer_id == -1) {
					LOG_VPRINT_ERROR("Could not re-register update timer: %s",
						strerror(errno));
				}
			}
		} else {
			LOG_VPRINT_ERROR("Invalid timer: %i", timer_data->id);
		}
		avbox_timers_releasepayload(timer_data);
		break;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		inst->destroying = 1;

		/* hide the window */
		if (avbox_window_isvisible(inst->window)) {
			avbox_listview_releasefocus(inst->menu);
			avbox_window_hide(inst->window);
		}

		/* if the update timer is still running wait for
		 * it to stop */
		if (inst->update_timer_id != -1) {
			return AVBOX_DISPATCH_CONTINUE;
		}

		/* if the worker is still running wait for it
		 * to exit */
		if (inst->worker != NULL) {
			DEBUG_PRINT("downloads", "Waiting for worker");
			if (!avbox_delegate_finished(inst->worker)) {
				return AVBOX_DISPATCH_CONTINUE;
			} else {
				avbox_delegate_wait(inst->worker, NULL);
			}
		}

		if (inst->menu != NULL) {
			avbox_listview_enumitems(inst->menu, mbox_downloads_freeitems, NULL);
			avbox_listview_destroy(inst->menu);
		}

		break;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		DEBUG_PRINT(LOG_MODULE, "Cleaning downloads window");
		free(inst);
		break;
	}
	default:
		return AVBOX_DISPATCH_CONTINUE;
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Initialize the MediaBox downloads list
 */
struct mbox_downloads*
mbox_downloads_new(struct avbox_object *parent)
{
	int xres, yres;
	int font_height;
	int window_height, window_width;
	int n_entries = 10;
	struct mbox_downloads *inst;

	/* allocate memory */
	if ((inst = malloc(sizeof(struct mbox_downloads))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	memset(inst, 0, sizeof(struct mbox_downloads));
	LIST_INIT(&inst->downloads);

	/* set height according to font size */
	avbox_window_getcanvassize(avbox_video_getrootwindow(0), &xres, &yres);
	font_height = mbv_getdefaultfontheight();
	window_height = 30 + font_height + ((font_height + 10) * n_entries);

	/* set width according to screen size */
	if (xres >= 1920) {
		window_width = 1200;
	} else if (xres >= 1280) {
		window_width = 1000;
	} else if (xres >= 1024) {
		window_width = 800;
	} else {
		window_width = 600;
	}

	/* create a new window for the menu dialog */
	inst->window = avbox_window_new(NULL, "downloads",
		AVBOX_WNDFLAGS_DECORATED | AVBOX_WNDFLAGS_ALPHABLEND,
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height, mbox_downloads_messagehandler, NULL, inst);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create window!");
		free(inst);
		return NULL;
	}
	if (avbox_window_settitle(inst->window, "FILE TRANSFERS") == -1) {
		LOG_VPRINT_ERROR("Could not set window title: %s",
			strerror(errno));
	}

	/* create a new menu widget inside main window */
	inst->menu = avbox_listview_new(inst->window, avbox_window_object(inst->window));
	if (inst->menu == NULL) {
		LOG_PRINT_ERROR("Could not create listview!");
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	/* initialize */
	inst->parent_object = parent;
	inst->update_timer_id = -1;
	inst->worker = NULL;
	inst->destroying = 0;
	return inst;
}


int
mbox_downloads_show(struct mbox_downloads * const inst)
{
	struct timespec tv;

	/* show the menu window */
        avbox_window_show(inst->window);

	/* populate the list */
	mbox_downloads_populatelist(0, inst);

	/* register the update timer */
	tv.tv_sec = 2;
	tv.tv_nsec = 0;
	inst->update_timer_id = avbox_timer_register(&tv,
		AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
		avbox_window_object(inst->window), NULL, inst);
	if (inst->update_timer_id == -1) {
		avbox_window_hide(inst->window);
		return -1;
	}

	/* show the menu widget and run it's input loop */
	if (avbox_listview_focus(inst->menu) == -1) {
		avbox_listview_releasefocus(inst->menu);
		avbox_window_hide(inst->window);
		return -1;
	}

	return 0;
}


/**
 * Gets the underlying window.
 */
struct avbox_window *
mbox_downloads_window(const struct mbox_downloads * const inst)
{
	return inst->window;
}
