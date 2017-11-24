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


#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#define LOG_MODULE "browser"

#include "lib/log.h"
#include "lib/debug.h"
#include "lib/dispatch.h"
#include "lib/thread.h"
#include "lib/application.h"
#include "lib/timers.h"
#include "lib/ui/video.h"
#include "lib/ui/listview.h"
#include "lib/ui/input.h"
#include "lib/ui/player.h"
#include "browser.h"
#include "library.h"
#include "shell.h"


struct mbox_browser_playlist_item
{
	int isdir;
	union
	{
		const char *filepath;
		struct avbox_playlist_item *playlist_item;
	} data;
};


struct mbox_browser
{
	LIST playlist;
	struct avbox_window *window;
	struct avbox_listview *menu;
	struct avbox_object *parent_obj;
	struct avbox_delegate *worker;
	int destroying;
	int select_timer_id;
	int dismiss_timer_id;
	int abort;
	char *dotdot;
};


struct mbox_browser_loadlist_context
{
	struct mbox_browser *inst;
	char *path;
};


struct mbox_browser_additem_context
{
	struct mbox_browser *inst;
	struct mbox_browser_playlist_item *item;
	char *title;
};


#define LIBRARY_ROOT "/media/UPnP"


static struct avbox_playlist_item *
mbox_browser_addtoplaylist(struct mbox_browser * const inst, const char *file)
{
	struct avbox_playlist_item *item;

	/* check that the file is valid */
	if (file == NULL || strlen(file) == 0) {
		LOG_PRINT_ERROR("Could not add to playlist. Invalid arguments");
		errno = ENOENT;
		return NULL;
	}

	/* allocate memory */
	if ((item = malloc(sizeof(struct avbox_playlist_item))) == NULL) {
		LOG_PRINT_ERROR("Could not add to playlist. Out of memory");
		return NULL;
	}

	/* copy filepath */
	if ((item->filepath = strdup(file)) == NULL) {
		LOG_PRINT_ERROR("Could not add to playlist. Out of memory (2)");
		free(item);
		return NULL;
	}

	LIST_ADD(&inst->playlist, item);

	return item;
}


static void
mbox_browser_freeplaylistitem(struct avbox_playlist_item *item)
{
	assert(item != NULL);

	if (item->filepath != NULL) {
		free((void*) item->filepath);
	}

	free(item);
}


static void
mbox_browser_freeplaylist(struct mbox_browser *inst)
{
	struct avbox_playlist_item *item;

	LIST_FOREACH_SAFE(struct avbox_playlist_item*, item, &inst->playlist, {
		LIST_REMOVE(item);
		mbox_browser_freeplaylistitem(item);
	});
}


/**
 * Update the window from the main thread.
 */
static void *
mbox_browser_updatewindow(void *ctx)
{
	struct mbox_browser * const inst = ctx;
	avbox_window_update(inst->window);
	return NULL;
}


/**
 * Add a list item from the main thread.
 */
static void *
mbox_browser_additem(void *ctx)
{
	struct mbox_browser * const inst = ((struct mbox_browser_additem_context*)ctx)->inst;
	struct mbox_browser_playlist_item * const library_item =
		((struct mbox_browser_additem_context*)ctx)->item;
	char * const title = ((struct mbox_browser_additem_context*)ctx)->title;
	avbox_listview_additem(inst->menu, title, library_item);
	return NULL;
}


/**
 * Populate the list from a background thread.
 */
static void *
__mbox_browser_loadlist(void *ctx)
{
	struct mbox_browser * const inst =
		((struct mbox_browser_loadlist_context*) ctx)->inst;
	const char * const path = ((struct mbox_browser_loadlist_context*) ctx)->path;
	struct mbox_library_dir *dir = NULL;
	struct mbox_library_dirent *ent;
	int ret = -1;
	struct avbox_delegate *del;

	DEBUG_VPRINT(LOG_MODULE, "Loading list: %s", path);
	ASSERT(path != NULL);

	/* first free the playlist */
	mbox_browser_freeplaylist(inst);

	if ((dir = mbox_library_opendir(path)) == NULL) {
		LOG_VPRINT_ERROR("Cannot open library directory '%s': %s",
			path, strerror(errno));
		goto end;
	}

	if (inst->dotdot != NULL) {
		free(inst->dotdot);
		inst->dotdot = NULL;
	}

	while (!inst->abort) {
		if (!(errno = 0) && (ent = mbox_library_readdir(dir)) == NULL) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == 0) {
				break;
			} else {
				LOG_VPRINT_ERROR("mbox_library_readdir() error: %s",
					strerror(errno));
				break;
			}
		}
		if (!strcmp(ent->name, "..")) {
			if ((inst->dotdot = strdup(ent->path)) == NULL) {
				LOG_VPRINT_ERROR("Could not set BACK directory: %s",
					strerror(errno));
			}
		} else {
			struct mbox_browser_playlist_item *library_item;

			if ((library_item = malloc(sizeof(struct mbox_browser_playlist_item))) == NULL) {
				LOG_PRINT_ERROR("Add to playlist failed");
				mbox_library_freedirentry(ent);
				goto end;
			}

			if (ent->isdir) {
				library_item->isdir = 1;
				library_item->data.filepath = strdup(ent->path);
				if (library_item->data.filepath == NULL) {
					LOG_VPRINT_ERROR("Could not populate list: %s",
						strerror(errno));
					mbox_library_freedirentry(ent);
					free(library_item);
					goto end;
				}
			} else {
				library_item->isdir = 0;

				/* add item to playlist */
				if ((library_item->data.playlist_item =
					mbox_browser_addtoplaylist(inst, ent->path)) == NULL) {
					LOG_PRINT_ERROR("Add to playlist failed");
					free(library_item);
					mbox_library_freedirentry(ent);
					goto end;
				}
			}

			/* add item to menu */
			struct mbox_browser_additem_context addctx;
			addctx.inst = inst;
			addctx.title = ent->name;
			addctx.item = library_item;
			if ((del = avbox_application_delegate(mbox_browser_additem, &addctx)) == NULL) {
				LOG_VPRINT_ERROR("Could not add item. "
					"avbox_application_delegate() failed: %s",
					strerror(errno));
			} else {
				avbox_delegate_wait(del, NULL);
			}
		}
		mbox_library_freedirentry(ent);
	}

	/* update the library window */
	if ((del = avbox_application_delegate(mbox_browser_updatewindow, inst)) == NULL) {
		LOG_VPRINT_ERROR("Could not update window!: %s",
			strerror(errno));
	} else {
		avbox_delegate_wait(del, NULL);
	}

	ret = 0;
end:
	if (ret != 0) {
		DEBUG_VPRINT(LOG_MODULE, "Loadlist baling with status %i",
			ret);
	}
	if (dir != NULL) {
		mbox_library_closedir(dir);
	}
	if (path != NULL) {
		free(((struct mbox_browser_loadlist_context*)ctx)->path);
	}
	free(ctx);

	return (void*) (intptr_t) ret;
}


/**
 * Called back by avbox_listview_enumitems(). Used to free
 * item list entries
 */
static int
mbox_browser_freeitems(void *item, void *data)
{
	struct mbox_browser_playlist_item *playlist_item =
		(struct mbox_browser_playlist_item*) item;

	if (playlist_item->isdir) {
		if (playlist_item->data.filepath != NULL) {
			free((void*) playlist_item->data.filepath);
		}
	} else {
		/* NOTE: We don't need to free the playlist item because
		 * it belongs to the global playlist and it will be freed
		 * once the list gets reloaded or the library object gets
		 * destroyed */

		/*
		if (playlist_item->data.playlist_item != NULL) {
			mbox_library_freeplaylistitem(playlist_item->data.playlist_item);
		}
		*/
	}
	free(item);
	return 0;
}


/**
 * Populate the list on a background thread.
 */
static void
mbox_browser_loadlist(struct mbox_browser * const inst, const char * const path)
{
	struct mbox_browser_loadlist_context *ctx;
	char *selected_copy;

	ASSERT(inst->worker == NULL);

	if (inst->destroying) {
		return;
	}

	/* allocate memory for context */
	if ((selected_copy = strdup(path)) == NULL) {
		abort(); /* for now */
	}
	if ((ctx = malloc(sizeof(struct mbox_browser_loadlist_context))) == NULL) {
		abort();
	}

	/* clear the list and load the next page */
	avbox_listview_enumitems(inst->menu, mbox_browser_freeitems, NULL);
	avbox_listview_clearitems(inst->menu);

	/* populate the list from a background thread */
	ctx->inst = inst;
	ctx->path = selected_copy;
	inst->abort = 0;
	if ((inst->worker = avbox_workqueue_delegate(__mbox_browser_loadlist, ctx)) == NULL) {
		LOG_VPRINT_ERROR("Could not delegate to main thread: %s",
			strerror(errno));
		free(ctx);
		free(selected_copy);
	}
}


/**
 * Handle incoming messages.
 */
static int
mbox_browser_messagehandler(void *context, struct avbox_message *msg)
{
	struct mbox_browser * const inst = context;

	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_TIMER:
	{
		struct avbox_timer_data * const timer_data =
			avbox_message_payload(msg);
		if (timer_data->id == inst->select_timer_id) {
			struct avbox_object *object =
				avbox_window_object(inst->window);
			/* resend the SELECTED message */
			if (avbox_object_sendmsg(&object,
				AVBOX_MESSAGETYPE_SELECTED, AVBOX_DISPATCH_UNICAST, inst->menu) == NULL) {
				LOG_VPRINT_ERROR("Could not re-send SELECTED message: %s",
					strerror(errno));
			}
			free(timer_data);
			inst->select_timer_id = -1;

		} else if (timer_data->id == inst->dismiss_timer_id) {
			struct avbox_object *object =
				avbox_window_object(inst->window);
			/* resend the SELECTED message */
			if (avbox_object_sendmsg(&object,
				AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst->menu) == NULL) {
				LOG_VPRINT_ERROR("Could not re-send SELECTED message: %s",
					strerror(errno));
			}
			free(timer_data);
			inst->dismiss_timer_id = -1;

		} else {
			DEBUG_VPRINT(LOG_MODULE, "Invalid timer: %i",
				timer_data->id);
		}

		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_SELECTED:
	{
		ASSERT(avbox_message_payload(msg) == inst->menu);

		struct mbox_browser_playlist_item *selected =
			avbox_listview_getselected(inst->menu);

		if (selected == NULL) {
			return AVBOX_DISPATCH_OK;
		}

		/* if there's already a SELECTED message pending
		 * ignore this one */
		if (inst->select_timer_id != -1 || inst->dismiss_timer_id != -1) {
			return AVBOX_DISPATCH_OK;
		}

		if (selected->isdir) {
			if (inst->worker != NULL) {
				if (!avbox_delegate_finished(inst->worker)) {
					struct timespec tv;

					/* signal the worker to abort */
					inst->abort = 1;

					/* Set a timer to invoke the SELECTED message again
					 * after a delay */
					tv.tv_sec = 0;
					tv.tv_nsec = 100L * 1000L;
					inst->select_timer_id = avbox_timer_register(&tv,
						AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
						avbox_window_object(inst->window), NULL, NULL);

					/* if the timer fails for any reason (it shouldn't) then
					 * we need to send the SELECTED message again immediately */
					if (inst->select_timer_id == -1) {
						struct avbox_object *object =
							avbox_window_object(inst->window);
						LOG_VPRINT_ERROR("Could not register destructor timer: %s",
							strerror(errno));
						if (avbox_object_sendmsg(&object,
							AVBOX_MESSAGETYPE_SELECTED, AVBOX_DISPATCH_UNICAST, inst->menu) == NULL) {
							LOG_VPRINT_ERROR("Could not send CLEANUP message: %s",
								strerror(errno));
						}
						/* yield the CPU so the other threads can do their work */
						sched_yield();
					}
					return AVBOX_DISPATCH_OK;
				}
				avbox_delegate_wait(inst->worker, NULL);
				inst->worker = NULL;
			}
			DEBUG_VPRINT(LOG_MODULE, "Selected directory: %s",
				selected->data.filepath);
			mbox_browser_loadlist(inst, selected->data.filepath);
		} else {
			struct avbox_player *player;
			struct avbox_playlist_item *playlist_item;

			playlist_item = selected->data.playlist_item;

			ASSERT(selected->data.playlist_item != NULL);
			ASSERT(LIST_SIZE(&inst->playlist) > 0);

			/* get the active player instance */
			player = mbox_shell_getactiveplayer();
			if (player == NULL) {
				LOG_PRINT_ERROR("Could not get active player!");
				break;
			}

			/* send the play command */
			avbox_player_playlist(player,
				&inst->playlist, playlist_item);
		}

		break;
	}
	case AVBOX_MESSAGETYPE_DISMISSED:
	{
		DEBUG_ASSERT(LOG_MODULE, avbox_message_payload(msg) == inst->menu,
			"Invalid message payload!");

		/* if there's already a SELECTED or DISMISSED message pending
		 * ignore this one */
		if (inst->select_timer_id != -1 || inst->dismiss_timer_id != -1) {
			return AVBOX_DISPATCH_OK;
		}

		if (inst->dotdot != NULL) {
			if (inst->worker != NULL) {
				if (!avbox_delegate_finished(inst->worker)) {
					struct timespec tv;

					/* signal the worker to abort */
					inst->abort = 1;

					/* Set a timer to invoke the SELECTED message again
					 * after a delay */
					tv.tv_sec = 0;
					tv.tv_nsec = 100L * 1000L;
					inst->dismiss_timer_id = avbox_timer_register(&tv,
						AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
						avbox_window_object(inst->window), NULL, NULL);

					/* if the timer fails for any reason (it shouldn't) then
					 * we need to send the SELECTED message again immediately */
					if (inst->dismiss_timer_id == -1) {
						struct avbox_object *object =
							avbox_window_object(inst->window);
						LOG_VPRINT_ERROR("Could not register destructor timer: %s",
							strerror(errno));
						if (avbox_object_sendmsg(&object,
							AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst->menu) == NULL) {
							LOG_VPRINT_ERROR("Could not send CLEANUP message: %s",
								strerror(errno));
						}
						/* yield the CPU so the other threads can do their work */
						sched_yield();
					}
					return AVBOX_DISPATCH_OK;
				}
				avbox_delegate_wait(inst->worker, NULL);
				inst->worker = NULL;
			}
			mbox_browser_loadlist(inst, inst->dotdot);
		} else {
			/* hide window */
			avbox_listview_releasefocus(inst->menu);
			avbox_window_hide(inst->window);

			/* send DISMISSED message */
			if (avbox_object_sendmsg(&inst->parent_obj,
				AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
				LOG_VPRINT_ERROR("Could not send DISMISSED message: %s",
					strerror(errno));
			}
		}

		break;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		DEBUG_PRINT(LOG_MODULE, "Shutdown library");

		inst->destroying = 1;

		if (avbox_window_isvisible(inst->window)) {
			avbox_window_hide(inst->window);
		}

		if (inst->select_timer_id != -1 || inst->dismiss_timer_id != -1) {
			DEBUG_VPRINT(LOG_MODULE, "Delaying DESTROY. Timer pending select=%i dismiss=%i",
				inst->select_timer_id, inst->dismiss_timer_id);
			return AVBOX_DISPATCH_CONTINUE;
		}

		if (inst->worker != NULL) {
			inst->abort = 1;
			if (!avbox_delegate_finished(inst->worker)) {
				DEBUG_PRINT(LOG_MODULE, "Delaying destroy. Worker not finished");
				return AVBOX_DISPATCH_CONTINUE;
			}
			avbox_delegate_wait(inst->worker, NULL);
		}

		if (inst->dotdot != NULL) {
			free(inst->dotdot);
			inst->dotdot = NULL;
		}

		mbox_browser_freeplaylist(inst);

		if (inst->menu != NULL) {
			avbox_listview_enumitems(inst->menu, mbox_browser_freeitems, NULL);
			avbox_listview_destroy(inst->menu);
		}
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		free(inst);
		break;
	}
	default:
		abort();
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Initialize the MediaBox menu
 */
struct mbox_browser *
mbox_browser_new(struct avbox_object *parent)
{
	int resx, resy, width;
	const int height = 450;
	struct mbox_browser *inst;

	/* allocate memory */
	if ((inst = malloc(sizeof(struct mbox_browser))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	memset(inst, 0, sizeof(struct mbox_browser));
	LIST_INIT(&inst->playlist);

	avbox_window_getcanvassize(avbox_video_getrootwindow(0), &resx, &resy);

	/* set width according to screen size */
	switch (resx) {
	case 800:  width =  700; break;
	case 1024: width =  800; break;
	case 1280: width = 1000; break;
	case 1920: width = 1280; break;
	case 640:
	default:   width = 300; break;
	}

	/* create a new window for the library dialog */
	inst->window = avbox_window_new(NULL, "browser",
		AVBOX_WNDFLAGS_DECORATED,
		(resx / 2) - (width / 2),
		(resy / 2) - (height / 2),
		width, height,
		mbox_browser_messagehandler, NULL, inst);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create library window!");
		free(inst);
		return NULL;
	}
	if (avbox_window_settitle(inst->window, "BROWSE MEDIA") == -1) {
		assert(errno == ENOMEM);
		LOG_PRINT_ERROR("Could not set window title");
		avbox_window_destroy(inst->window);
		return NULL;
	}

	/* create a new menu widget inside main window */
	inst->menu = avbox_listview_new(inst->window, avbox_window_object(inst->window));
	if (inst->menu == NULL) {
		LOG_PRINT_ERROR("Could not create menu widget!");
		avbox_window_destroy(inst->window);
		return NULL;
	}

	/* initialize context */
	inst->parent_obj = parent;
	inst->dotdot = NULL;
	inst->worker = NULL;
	inst->select_timer_id = -1;
	inst->dismiss_timer_id = -1;

	/* populate the menu */
	mbox_browser_loadlist(inst, "/");

	return inst;
}


int
mbox_browser_show(struct mbox_browser * const inst)
{
	/* show the menu window */
        avbox_window_show(inst->window);

	if (avbox_listview_focus(inst->menu) == -1) {
		LOG_PRINT_ERROR("Could not show menu!");
		return -1;
	}

	return 0;
}


/**
 * Gets the underlying object.
 */
struct avbox_window *
mbox_browser_window(struct mbox_browser * const inst)
{
	return inst->window;
}
