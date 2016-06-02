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


#include "video.h"
#include "input.h"
#include "ui-menu.h"
#include "shell.h"
#include "player.h"


static struct mbv_window *window = NULL;
static struct mb_ui_menu *menu = NULL;

#define LIBRARY_ROOT "/media/UPnP"


/**
 * mb_library_stripext() -- Strip a file extension IN PLACE
 * and return a pointer to the extension.
 */
static char *
mb_library_stripext(char *filename)
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
mb_library_loadlist(const char *path)
{
	DIR *dir;
	int isroot;
	struct dirent *ent;
	char *rpath;
	const size_t path_len = strlen(path);
	size_t resolved_path_len;
	char resolved_path[PATH_MAX];

	assert(path != NULL);

	rpath = malloc(sizeof(LIBRARY_ROOT) + path_len + 2);
	if (rpath == NULL) {
		fprintf(stderr, "mb_library: Out of memory\n");
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
		fprintf(stderr, "mb_library: Invalid path\n");
		free(rpath);
		return -1;
	}

	isroot = (strcmp(LIBRARY_ROOT, resolved_path) == 0);
	resolved_path_len = strlen(resolved_path);
	free(rpath);

	if ((dir = opendir(resolved_path)) == NULL) {
		fprintf(stderr, "mb_library: opendir() failed\n");
		return -1;
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
			fprintf(stderr, "mb_library: Out of memory\n");
			closedir(dir);
			return -1;
		}

		/* strip the filename extension from the title */
		ext = mb_library_stripext(title);

		/* do not show subtitles */
		if (ext != NULL) {
			if (!strcasecmp("srt", ext) || !strcasecmp("sub", ext)) {
				free(title);
				continue;
			}
		}

		/* allocate mem for a copy of the filepath */
		filepath = malloc(resolved_path_len + strlen(ent->d_name) + 3);
		if (filepath == NULL) {
			fprintf(stderr, "mb_library: Out of memory\n");
			free(title);
			closedir(dir);
			return -1;
		}

		strcpy(filepath, resolved_path);
		strcat(filepath, "/");
		strcat(filepath, ent->d_name);

		if (stat(filepath, &st) == -1) {
			fprintf(stderr, "mb_library: stat() failed errno=%i\n", errno);
			free(filepath);
			free(title);
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			strcat(filepath, "/");
			/* This needs to be destroyed when it's no longer needed. The
			 * ref to it is on the menu widget's item list */
			filepathrel = strdup(filepath + sizeof(LIBRARY_ROOT) - 1);
		} else {
			filepathrel = strdup(filepath);
		}
		if (filepathrel == NULL) {
			fprintf(stderr, "mb_library: Out of memory\n");
			free(filepath);
			free(title);
			closedir(dir);
			return -1;
		}

		/* add item to menu */
		mb_ui_menu_additem(menu, title, filepathrel);

		free(filepath);
		free(title);
	}
	closedir(dir);
	return 0;
}


/**
 * mbm_init() -- Initialize the MediaBox menu
 */
int
mb_library_init(void)
{
	int resx, resy, width;
	const int height = 450;
	int width_percent = 60;

	mbv_getscreensize(&resx, &resy);
	if (resx == 640) {
		width_percent = 90;
	}
	width = (resx * width_percent) / 100;

	/* create a new window for the library dialog */
	window = mbv_window_new("MEDIA LIBRARY",
		(resx / 2) - (width / 2),
		(resy / 2) - (height / 2),
		width, height);
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
	mb_library_loadlist("/");

	return 0;
}


int
mb_library_showdialog(void)
{
	/* show the menu window */
        mbv_window_show(window);

	/* show the menu widget and run it's input loop */
	while (mb_ui_menu_showdialog(menu) == 0) {
		char *selected = mb_ui_menu_getselected(menu);

		assert(selected != NULL);

		if (selected[strlen(selected) - 1] == '/') {
			char *selected_copy = strdup(selected);
			if (selected_copy == NULL) {
				abort(); /* for now */
			}

			/* TODO: free strings first */
			mb_ui_menu_clearitems(menu);
			mb_library_loadlist(selected_copy);
			mbv_window_update(window);
	
		} else {
			struct mbp* player;

			fprintf(stderr, "mb_library: Selected %s\n",
				selected);

			/* get the active player instance */
			player = mbs_get_active_player();
			if (player == NULL) {
				fprintf(stderr, "mb_library: Could not get active player\n");
				break;
			}

			mbp_play(player, selected);
			break;
		}
	}

	/* hide the mainmenu window */
	mbv_window_hide(window);

	return 0;
}


void
mb_library_destroy(void)
{
	fprintf(stderr, "mb_library: Destroying instance\n");

	mb_ui_menu_destroy(menu);
	mbv_window_destroy(window);
}

