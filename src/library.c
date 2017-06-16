/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
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

#define LOG_MODULE "library"

#include "lib/log.h"
#include "lib/debug.h"
#include "lib/dispatch.h"
#include "lib/thread.h"
#include "lib/application.h"
#include "lib/ui/video.h"
#include "lib/ui/listview.h"
#include "lib/ui/input.h"
#include "lib/ui/player.h"
#include "library.h"
#include "shell.h"


struct mbox_library_playlist_item
{
	int isdir;
	union
	{
		const char *filepath;
		struct avbox_playlist_item *playlist_item;
	} data;
};


struct mbox_library
{
	LIST playlist;
	struct avbox_window *window;
	struct avbox_listview *menu;
	struct avbox_object *parent_obj;
	char *dotdot;
};


struct mbox_library_loadlist_context
{
	struct mbox_library *inst;
	char *path;
};


struct mbox_library_additem_context
{
	struct mbox_library *inst;
	struct mbox_library_playlist_item *item;
	char *title;
};


#define LIBRARY_ROOT "/media/UPnP"


static struct avbox_playlist_item *
mbox_library_addtoplaylist(struct mbox_library *inst, const char *file)
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
mbox_library_freeplaylistitem(struct avbox_playlist_item *item)
{
	assert(item != NULL);

	if (item->filepath != NULL) {
		free((void*) item->filepath);
	}

	free(item);
}


static void
mbox_library_freeplaylist(struct mbox_library *inst)
{
	struct avbox_playlist_item *item;

	LIST_FOREACH_SAFE(struct avbox_playlist_item*, item, &inst->playlist, {
		LIST_REMOVE(item);
		mbox_library_freeplaylistitem(item);
	});
}


/**
 * Strip a file extension IN PLACE
 * and return a pointer to the extension.
 */
static char *
mbox_library_stripext(char *filename)
{
	char *p;

	assert(filename != NULL);

	if (filename[0] == '.' && filename[1] == '.' && filename[2] == '\0') {
		return NULL;
	}

	p = filename + strlen(filename);
	assert(*p == '\0');

	while (p != filename && *p != '.') {
		p--;
	}

	if (*p == '.') {
		*p = '\0';
		return ++p;
	}
	return NULL;
}


/**
 * Update the window from the main thread.
 */
static void *
mbox_library_updatewindow(void *ctx)
{
	struct mbox_library * const inst = ctx;
	avbox_window_update(inst->window);
	return NULL;
}


/**
 * Add a list item from the main thread.
 */
static void *
mbox_library_additem(void *ctx)
{
	struct mbox_library * const inst = ((struct mbox_library_additem_context*)ctx)->inst;
	struct mbox_library_playlist_item * const library_item =
		((struct mbox_library_additem_context*)ctx)->item;
	char * const title = ((struct mbox_library_additem_context*)ctx)->title;
	avbox_listview_additem(inst->menu, title, library_item);
	return NULL;
}


/**
 * Populate the list from a background thread.
 */
static void *
__mbox_library_loadlist(void *ctx)
{
	struct mbox_library * const inst =
		((struct mbox_library_loadlist_context*) ctx)->inst;
	const char * const path = ((struct mbox_library_loadlist_context*) ctx)->path;
	DIR *dir = NULL;
	int isroot, ret = -1;
	struct dirent *ent;
	char *rpath;
	const size_t path_len = strlen(path);
	size_t resolved_path_len;
	char resolved_path[PATH_MAX];
	struct avbox_delegate *del;

	assert(path != NULL);

	/* first free the playlist */
	mbox_library_freeplaylist(inst);

	/* allocate memory for item path */
	rpath = malloc(sizeof(LIBRARY_ROOT) + path_len + 2);
	if (rpath == NULL) {
		LOG_VPRINT_ERROR("Could not load list: %s",
			strerror(errno));
		goto end;
	}

	/* concatenate the library root with the given path
	 * adding a trailing slash if necessary */
	strcpy(rpath, LIBRARY_ROOT);
	strcat(rpath, path);
	if (path[path_len - 1] != '/') {
		strcat(rpath, "/");
	}

	if (realpath(rpath, resolved_path) == NULL) {
		DEBUG_VPRINT("library", "Invalid path %s",
			rpath);
		free(rpath);
		goto end;
	}

	isroot = (strcmp(LIBRARY_ROOT, resolved_path) == 0);
	resolved_path_len = strlen(resolved_path);
	free(rpath);

	if ((dir = opendir(resolved_path)) == NULL) {
		LOG_VPRINT_ERROR("Cannot open directory '%s': %s",
			resolved_path, strerror(errno));
		goto end;
	}

	if (inst->dotdot != NULL) {
		free(inst->dotdot);
		inst->dotdot = NULL;
	}

	while ((ent = readdir(dir)) != NULL) {
		char *filepath, *filepathrel, *title, *ext;
		struct stat st;

		/* do not show dot directories except .. */
		if (ent->d_name[0] == '.') {
			if (strcmp(ent->d_name, "..") || isroot) {
				continue;
			}
		}

		/* do not show the dot directory */
		if (ent->d_name[0] == '.' && ent->d_name[1] == '\0') {
			continue;
		}

		/* get a copy of the filename (this will be the title) */
		title = strdup(ent->d_name);
		if (title == NULL) {
			LOG_VPRINT_ERROR("Could not load list: %s",
				strerror(errno));
			goto end;
		}

		/* strip the filename extension from the title */
		ext = mbox_library_stripext(title);

		/* do not show subtitles */
		if (ext != NULL) {
			if (!strcasecmp("srt", ext) || !strcasecmp("sub", ext) || !strcasecmp("idx", ext)) {
				free(title);
				continue;
			}
		}

		/* allocate mem for a copy of the filepath */
		filepath = malloc(resolved_path_len + strlen(ent->d_name) + 3);
		if (filepath == NULL) {
			LOG_VPRINT_ERROR("Could not load list: %s",
				strerror(errno));
			free(title);
			goto end;
		}

		strcpy(filepath, resolved_path);
		strcat(filepath, "/");
		strcat(filepath, ent->d_name);

		if (stat(filepath, &st) == -1) {
			LOG_VPRINT_ERROR("Could not stat '%s': %s",
				strerror(errno));
			free(filepath);
			free(title);
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			strcat(filepath, "/");
			filepathrel = strdup(filepath + sizeof(LIBRARY_ROOT) - 1);
		} else {
			filepathrel = strdup(filepath);
		}
		if (filepathrel == NULL) {
			LOG_VPRINT_ERROR("Could not load list: %s",
				strerror(errno));
			free(filepath);
			free(title);
			goto end;
		}

		if (!strcmp(ent->d_name, "..")) {
			inst->dotdot = filepathrel;
		} else {
			struct mbox_library_playlist_item *library_item;

			if ((library_item = malloc(sizeof(struct mbox_library_playlist_item))) == NULL) {
				LOG_PRINT_ERROR("Add to playlist failed");
				free(filepathrel);
				free(filepath);
				free(title);
				goto end;
			}

			if (S_ISDIR(st.st_mode)) {
				library_item->isdir = 1;
				library_item->data.filepath = filepathrel;
			} else {
				library_item->isdir = 0;

				/* add item to playlist */
				if ((library_item->data.playlist_item =
					mbox_library_addtoplaylist(inst, filepathrel)) == NULL) {
					LOG_PRINT_ERROR("Add to playlist failed");
					free(library_item);
					free(filepathrel);
					free(filepath);
					free(title);
					goto end;
				}

				free(filepathrel);
			}

			assert(library_item != NULL);

			/* add item to menu */
			struct mbox_library_additem_context addctx;
			addctx.inst = inst;
			addctx.title = title;
			addctx.item = library_item;
			if ((del = avbox_application_delegate(mbox_library_additem, &addctx)) == NULL) {
				LOG_VPRINT_ERROR("Could not add item. "
					"avbox_application_delegate() failed: %s",
					strerror(errno));
			} else {
				avbox_delegate_wait(del, NULL);
			}
		}

		free(filepath);
		free(title);
	}

	/* update the library window */
	if ((del = avbox_application_delegate(mbox_library_updatewindow, inst)) == NULL) {
		LOG_VPRINT_ERROR("Could not update window!: %s",
			strerror(errno));
	} else {
		avbox_delegate_wait(del, NULL);
	}

	ret = 0;
end:
	if (ret != 0) {
		DEBUG_VPRINT("library", "Loadlist baling with status %i",
			ret);
	}
	if (dir != NULL) {
		closedir(dir);
	}
	if (path != NULL) {
		free(((struct mbox_library_loadlist_context*)ctx)->path);
	}
	free(ctx);

	return (void*) (intptr_t) ret;
}


/**
 * Called back by avbox_listview_enumitems(). Used to free
 * item list entries
 */
static int
mbox_library_freeitems(void *item, void *data)
{
	struct mbox_library_playlist_item *playlist_item =
		(struct mbox_library_playlist_item*) item;

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
mbox_library_loadlist(struct mbox_library * const inst, const char * const path)
{
	struct mbox_library_loadlist_context *ctx;
	struct avbox_delegate *del;
	char *selected_copy;

	/* allocate memory for context */
	if ((selected_copy = strdup(path)) == NULL) {
		abort(); /* for now */
	}
	if ((ctx = malloc(sizeof(struct mbox_library_loadlist_context))) == NULL) {
		abort();
	}

	/* clear the list and load the next page */
	avbox_listview_enumitems(inst->menu, mbox_library_freeitems, NULL);
	avbox_listview_clearitems(inst->menu);

	/* populate the list from a background thread */
	ctx->inst = inst;
	ctx->path = selected_copy;
	if ((del = avbox_thread_delegate(__mbox_library_loadlist, ctx)) == NULL) {
		LOG_VPRINT_ERROR("Could not delegate to main thread: %s",
			strerror(errno));
		free(ctx);
		free(selected_copy);
	} else {
		avbox_delegate_dettach(del);
	}
}


/**
 * Handle incoming messages.
 */
static int
mbox_library_messagehandler(void *context, struct avbox_message *msg)
{
	struct mbox_library * const inst = context;

	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_SELECTED:
	{
		ASSERT(avbox_message_payload(msg) == inst->menu);
		struct mbox_library_playlist_item *selected =
			avbox_listview_getselected(inst->menu);

		ASSERT(selected != NULL);

		if (selected->isdir) {
			DEBUG_VPRINT("library", "Selected directory: %s",
				selected->data.filepath);
			mbox_library_loadlist(inst, selected->data.filepath);
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

			if (avbox_player_playlist(player, &inst->playlist, playlist_item) == 0) {
				DEBUG_PRINT("library", "Play succeeded. Closing");

				/* hide window */
				avbox_listview_releasefocus(inst->menu);
				avbox_window_hide(inst->window);

				/* send dismissed message */
				if (avbox_object_sendmsg(&inst->parent_obj,
					AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
					LOG_VPRINT_ERROR("Could not send DISMISSED message: %s",
						strerror(errno));
				}
				break;
			} else {
				LOG_PRINT_ERROR("Could not play item!");
				/* TODO: Display an error message */
			}
		}

		break;
	}
	case AVBOX_MESSAGETYPE_DISMISSED:
	{
		DEBUG_ASSERT("library", avbox_message_payload(msg) == inst->menu,
			"Invalid message payload!");

		if (inst->dotdot != NULL) {
			mbox_library_loadlist(inst, inst->dotdot);
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
		DEBUG_PRINT("library", "Shutdown library");

		if (inst->dotdot != NULL) {
			free(inst->dotdot);
			inst->dotdot = NULL;
		}

		if (avbox_window_isvisible(inst->window)) {
			avbox_window_hide(inst->window);
		}

		mbox_library_freeplaylist(inst);
		if (inst->menu != NULL) {
			avbox_listview_enumitems(inst->menu, mbox_library_freeitems, NULL);
			avbox_listview_destroy(inst->menu);
		}
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
		free(inst);
		break;
	default:
		abort();
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Initialize the MediaBox menu
 */
struct mbox_library *
mbox_library_new(struct avbox_object *parent)
{
	int resx, resy, width;
	const int height = 450;
	struct mbox_library *inst;

	/* allocate memory */
	if ((inst = malloc(sizeof(struct mbox_library))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	memset(inst, 0, sizeof(struct mbox_library));
	LIST_INIT(&inst->playlist);

	avbox_window_getcanvassize(avbox_video_getrootwindow(0), &resx, &resy);

	/* set width according to screen size */
	switch (resx) {
	case 1024: width =  800; break;
	case 1280: width = 1000; break;
	case 1920: width = 1280; break;
	case 640:
	default:   width = 300; break;
	}

	/* create a new window for the library dialog */
	inst->window = avbox_window_new(NULL, "library",
		AVBOX_WNDFLAGS_DECORATED,
		(resx / 2) - (width / 2),
		(resy / 2) - (height / 2),
		width, height,
		mbox_library_messagehandler, NULL, inst);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create library window!");
		free(inst);
		return NULL;
	}
	if (avbox_window_settitle(inst->window, "MEDIA LIBRARY") == -1) {
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

	/* populate the menu */
	mbox_library_loadlist(inst, "/");

	return inst;
}


int
mbox_library_show(struct mbox_library * const inst)
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
 * Destroy the library object.
 */
void
mbox_library_destroy(struct mbox_library * const inst)
{
	avbox_window_destroy(inst->window);
}
