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

#define DELUGE_BIN "/usr/bin/deluge-console"


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
	DEBUG_VPRINT("downloads", "Updating listview item (name=%s)",
		ctx->name);
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
mbox_downloads_updateentry(struct mbox_downloads *inst, char *id, char *name)
{
	int found = 0;
	struct mbox_download *dl;
	struct mbox_downloads_update_context ctx;
	struct avbox_delegate *del;

	DEBUG_VPRINT("downloads", "Updating entry (id=%s, name=%s)",
		id, name);

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
	int process_id, exit_status = -1;
	FILE *f;
	size_t n = 0;;
	char *str = NULL, *name = NULL, *id = NULL, *progress = NULL, *progressbar = NULL;
	char buf[512];
	struct mbox_downloads * const inst = data;
	struct avbox_delegate * del;
	const char * const deluge_args[] =
	{
		"deluge-console",
		"connect",
		"127.0.0.1",
		"mediabox",
		"mediabox;",
		"info",
		NULL
	};

	DEBUG_PRINT("downloads", "Populating list (background)");

	/* run the deluge-console process */
	if ((process_id = avbox_process_start(DELUGE_BIN, deluge_args,
		AVBOX_PROCESS_NICE | AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_STDOUT_PIPE |
		AVBOX_PROCESS_WAIT, "deluge-console", NULL, NULL)) == -1) {
		LOG_PRINT_ERROR("Could not execute deluge-console");
		return (void*) (intptr_t) -1;
	}

	f = fdopen(avbox_process_openfd(process_id, STDOUT_FILENO), "r");
	if (f == NULL) {
		LOG_PRINT_ERROR("Could not open stream");
		return (void*) (intptr_t) -1;
	} else {
		while (getline(&str, &n, f) != -1) {
			if (str != NULL) {
				if (!memcmp(str, "Name: ", 6)) {
					str[strlen(str) - 1] = '\0';
					name = strdup(str + 6);
					if (name == NULL) {
						LOG_PRINT_ERROR("Out of memory");
					}
					/* fprintf(stderr, "Name: %s\n", name); */

				} else if (!memcmp("ID: ", str, 4)) {
					if (name != NULL) {
						str[strlen(str) - 1] = '\0';
						id = strdup(str + 4);
						if (id == NULL) {
							LOG_PRINT_ERROR("Out of memory");
							free(name);
							name = NULL;
						} else {
							/* fprintf(stderr, "ID: %s\n", id); */
						}
					}

				} else if (!memcmp("Progress: ", str, 10)) {
					if (id != NULL) {
						progress = str + 10;
						progressbar = progress;

						while (*progressbar != ' ') {
							progressbar++;
						}
						*progressbar++ = '\0';
						progressbar[strlen(progressbar) -1] = '\0';

						if ((progress = strdup(progress)) == NULL) {
							LOG_PRINT_ERROR("Out of memory");
							free(id);
							free(name);
							id = NULL;
							name = NULL;
						} else if ((progressbar = strdup(progressbar)) == NULL) {
							LOG_PRINT_ERROR("Out of memory");
							free(progress);
							free(id);
							free(name);
							progress = NULL;
							id = NULL;
							name = NULL;
						} else {
							snprintf(buf, 512, "%s (%s)",
								name, progress);

							mbox_downloads_updateentry(inst, id, buf);

							free(name);
							free(progress);
							free(progressbar);
							free(id);
							name = NULL;
							progress = NULL;
							progressbar = NULL;
							id = NULL;
						}
					}
				}
				free(str);
				str = NULL;
				n = 0;
			}
		}
		fclose(f);

		if (str != NULL) {
			free(str);
		}

		struct mbox_download *dl;
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
	}

	/* update the window from the main thread */
	if ((del = avbox_window_delegate(inst->window,
		mbox_downloads_updatewindow, inst->window)) == NULL) {
		LOG_VPRINT_ERROR("Could not update window: %s",
			strerror(errno));
	} else {
		avbox_delegate_wait(del, NULL);
	}

	/* wait for process to exit */
	if (avbox_process_wait(process_id, &exit_status) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "download", "avbox_process_wait() failed");
		return (void*) (intptr_t) -1;
	}

	DEBUG_PRINT("downloads", "List populated");
	return (void*) (intptr_t) ((exit_status == 0) ? 0 : -1);
}


/**
 * Manages the background thread that updates the list.
 */
static void
mbox_downloads_populatelist(int id, void *data)
{
	struct mbox_downloads * const inst = data;

	/* if the background thread is still running */
	if (inst->worker != NULL) {
		/* check if it finished and collect result */
		if (avbox_delegate_finished(inst->worker)) {
			void *result;
			DEBUG_PRINT("downloads", "Worker completed");
			avbox_delegate_wait(inst->worker, &result);
			inst->worker = NULL;
			if ((intptr_t) result != 0) {
				avbox_timer_cancel(inst->update_timer_id);
				inst->update_timer_id = -1;
			}
		}
	} else {
		DEBUG_PRINT("downloads", "Populating list");
		/* populate the list from a background thread */
		if ((inst->worker = avbox_thread_delegate(
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
		char *selected = avbox_listview_getselected(inst->menu);
		assert(selected != NULL);
		DEBUG_VPRINT("downloads", "Selected %s",
			selected);
		break;
	}
	case AVBOX_MESSAGETYPE_DISMISSED:
	{
		/* cancel the update timer */
		if (inst->update_timer_id != -1) {
			avbox_timer_cancel(inst->update_timer_id);
		}

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
		mbox_downloads_populatelist(timer_data->id, timer_data->data);
		free(timer_data);
		break;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		if (inst->update_timer_id != -1) {
			DEBUG_PRINT("downloads", "Cancelling update timer");
			avbox_timer_cancel(inst->update_timer_id);
		}

		/* if the worker is still running wait for it
		 * to exit */
		if (inst->worker != NULL) {
			DEBUG_PRINT("downloads", "Waiting for worker");
			avbox_delegate_wait(inst->worker, NULL);
		}

		if (avbox_window_isvisible(inst->window)) {
			avbox_listview_releasefocus(inst->menu);
			avbox_window_hide(inst->window);
		}
		if (inst->menu != NULL) {
			avbox_listview_enumitems(inst->menu, mbox_downloads_freeitems, NULL);
			avbox_listview_destroy(inst->menu);
		}
		break;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
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
	switch (xres) {
	case 1024: window_width = 800; break;
	case 1280: window_width = 1000; break;
	case 1920: window_width = 1200; break;
	case 640:
	default:   window_width = 600; break;
	}

	/* create a new window for the menu dialog */
	inst->window = avbox_window_new(NULL, "downloads",
		AVBOX_WNDFLAGS_DECORATED,
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height, mbox_downloads_messagehandler, NULL, inst);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create window!");
		free(inst);
		return NULL;
	}
	if (avbox_window_settitle(inst->window, "DOWNLOADS") == -1) {
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
		AVBOX_TIMER_TYPE_AUTORELOAD | AVBOX_TIMER_MESSAGE,
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


void
mbox_downloads_destroy(struct mbox_downloads * const inst)
{
	avbox_window_destroy(inst->window);
}
