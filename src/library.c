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

#include "video.h"
#include "input.h"
#include "ui-menu.h"
#include "shell.h"
#include "player.h"
#include "debug.h"
#include "log.h"
#include "library.h"


struct mb_library_playlist_item
{
	int isdir;
	union
	{
		const char *filepath;
		struct avbox_playlist_item *playlist_item;
	} data;
};


LIST_DECLARE_STATIC(playlist);

static struct mbv_window *window = NULL;
static struct mb_ui_menu *menu = NULL;
static char *dotdot = NULL;

#define LIBRARY_ROOT "/media/UPnP"


static struct avbox_playlist_item *
avbox_library_addtoplaylist(const char *file)
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

	LIST_ADD(&playlist, item);

	return item;
}


static void
avbox_library_freeplaylistitem(struct avbox_playlist_item *item)
{
	assert(item != NULL);

	if (item->filepath != NULL) {
		free((void*) item->filepath);
	}

	free(item);
}


static void
avbox_library_freeplaylist(void)
{
	struct avbox_playlist_item *item;

	LIST_FOREACH_SAFE(struct avbox_playlist_item*, item, &playlist, {
		LIST_REMOVE(item);
		avbox_library_freeplaylistitem(item);
	});
}


/**
 * mb_library_stripext() -- Strip a file extension IN PLACE
 * and return a pointer to the extension.
 */
static char *
avbox_library_stripext(char *filename)
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


static int
avbox_library_loadlist(const char *path)
{
	DIR *dir;
	int isroot;
	struct dirent *ent;
	char *rpath;
	const size_t path_len = strlen(path);
	size_t resolved_path_len;
	char resolved_path[PATH_MAX];

	assert(path != NULL);

	/* first free the playlist */
	avbox_library_freeplaylist();

	/* allocate memory for item path */
	rpath = malloc(sizeof(LIBRARY_ROOT) + path_len + 2);
	if (rpath == NULL) {
		LOG_VPRINT_ERROR("Could not load list: %s",
			strerror(errno));
		return -1;
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
		return -1;
	}

	isroot = (strcmp(LIBRARY_ROOT, resolved_path) == 0);
	resolved_path_len = strlen(resolved_path);
	free(rpath);

	if ((dir = opendir(resolved_path)) == NULL) {
		LOG_VPRINT_ERROR("Cannot open directory '%s': %s",
			resolved_path, strerror(errno));
		return -1;
	}

	if (dotdot != NULL) {
		free(dotdot);
		dotdot = NULL;
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
			closedir(dir);
			return -1;
		}

		/* strip the filename extension from the title */
		ext = avbox_library_stripext(title);

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
			closedir(dir);
			return -1;
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
			closedir(dir);
			return -1;
		}

		if (!strcmp(ent->d_name, "..")) {
			dotdot = filepathrel;
		} else {
			struct mb_library_playlist_item *library_item;

			if ((library_item = malloc(sizeof(struct mb_library_playlist_item))) == NULL) {
				LOG_PRINT_ERROR("Add to playlist failed");
				free(filepathrel);
				free(filepath);
				free(title);
				closedir(dir);
				return -1;
			}

			if (S_ISDIR(st.st_mode)) {
				library_item->isdir = 1;
				library_item->data.filepath = filepathrel;
			} else {
				library_item->isdir = 0;

				/* add item to playlist */
				if ((library_item->data.playlist_item = avbox_library_addtoplaylist(filepathrel)) == NULL) {
					LOG_PRINT_ERROR("Add to playlist failed");
					free(library_item);
					free(filepathrel);
					free(filepath);
					free(title);
					closedir(dir);
					return -1;
				}

				free(filepathrel);
			}

			assert(library_item != NULL);

			/* add item to menu */
			mb_ui_menu_additem(menu, title, library_item);
		}

		free(filepath);
		free(title);
	}
	closedir(dir);
	return 0;
}


/**
 * Called back by mb_ui_menu_enumitems(). Used to free
 * item list entries
 */
static int
avbox_library_freeitems(void *item, void *data)
{
	struct mb_library_playlist_item *playlist_item =
		(struct mb_library_playlist_item*) item;

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
			mb_library_freeplaylistitem(playlist_item->data.playlist_item);
		}
		*/
	}
	free(item);
	return 0;
}


/**
 * mbm_init() -- Initialize the MediaBox menu
 */
int
avbox_library_init(void)
{
	int resx, resy, width;
	const int height = 450;

	LIST_INIT(&playlist);

	mbv_window_getcanvassize(mbv_getrootwindow(), &resx, &resy);

	/* set width according to screen size */
	switch (resx) {
	case 1024: width =  800; break;
	case 1280: width = 1000; break;
	case 1920: width = 1280; break;
	case 640:
	default:   width = 300; break;
	}

	/* create a new window for the library dialog */
	window = mbv_window_new("library", "MEDIA LIBRARY",
		(resx / 2) - (width / 2),
		(resy / 2) - (height / 2),
		width, height, NULL);
	if (window == NULL) {
		LOG_PRINT_ERROR("Could not create library window!");
		return -1;
	}

	/* create a new menu widget inside main window */
	menu = mb_ui_menu_new(window);
	if (menu == NULL) {
		LOG_PRINT_ERROR("Could not create menu widget!");
		return -1;
	}

	/* populate the menu */
	avbox_library_loadlist("/");

	return 0;
}


int
avbox_library_showdialog(void)
{
	int ret = -1, quit = 0;

	/* show the menu window */
        mbv_window_show(window);

	/* show the menu widget and run it's input loop */
	while (!quit) {
		while (mb_ui_menu_showdialog(menu) == 0) {
			struct mb_library_playlist_item *selected =
				mb_ui_menu_getselected(menu);

			assert(selected != NULL);

			if (selected->isdir) {

				char *selected_copy = strdup(selected->data.filepath);
				if (selected_copy == NULL) {
					abort(); /* for now */
				}

				/* clear the list and load the next page */
				mb_ui_menu_enumitems(menu, avbox_library_freeitems, NULL);
				mb_ui_menu_clearitems(menu);
				avbox_library_loadlist(selected_copy);
				mbv_window_update(window);
				free(selected_copy);

			} else {
				struct avbox_player *player;
				struct avbox_playlist_item *playlist_item;

				playlist_item = selected->data.playlist_item;
				assert(selected->data.playlist_item != NULL);

				DEBUG_VPRINT("library", "Selected %s",
					selected->data.playlist_item->filepath);

				assert(LIST_SIZE(&playlist) > 0);

				/* get the active player instance */
				player = avbox_shell_getactiveplayer();
				if (player == NULL) {
					LOG_PRINT_ERROR("Could not get active player!");
					break;
				}

				mbv_window_hide(window);

				if (avbox_player_playlist(player, &playlist, playlist_item) == 0) {
					ret = 0;
					quit = 1;
					break;
				} else {
					mbv_window_show(window);

					LOG_PRINT_ERROR("Could not play item!");
					/* TODO: Display an error message */
				}
			}
		}
		if (!quit && dotdot != NULL) {
			/* clear the list and load the parent directory */
			mb_ui_menu_enumitems(menu, avbox_library_freeitems, NULL);
			mb_ui_menu_clearitems(menu);
			avbox_library_loadlist(dotdot);
			mbv_window_update(window);
		} else {
			break;
		}
	}

	/* hide the mainmenu window */
	mbv_window_hide(window);

	return ret;
}


void
avbox_library_shutdown(void)
{
	DEBUG_PRINT("library", "Shutdown library");
	if (dotdot != NULL) {
		free(dotdot);
		dotdot = NULL;
	}

	avbox_library_freeplaylist();
	mb_ui_menu_enumitems(menu, avbox_library_freeitems, NULL);
	mb_ui_menu_destroy(menu);
	mbv_window_destroy(window);
}
