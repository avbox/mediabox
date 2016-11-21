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
#include "linkedlist.h"
#include "timers.h"
#include "process.h"
#include "debug.h"
#include "log.h"

#ifdef ENABLE_IONICE
#include "ionice.h"
#include "su.h"
#endif

#define DELUGE_BIN "/usr/bin/deluge-console"


LISTABLE_TYPE(mb_download,
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


static struct mbv_window *window = NULL;
static struct mb_ui_menu *menu = NULL;

LIST_DECLARE_STATIC(downloads);


static int
mb_downloads_updateentry(char *id, char *name)
{
	int found = 0;
	mb_download *dl;
	LIST_FOREACH(mb_download*, dl, &downloads) {
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

		mb_ui_menu_setitemtext(menu, dl->id, name);

	} else {
		if ((dl = malloc(sizeof(mb_download))) == NULL) {
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

		mb_ui_menu_additem(menu, dl->name, dl->id);

		LIST_ADD(&downloads, dl);

	}
	return 0;
}


static int
mb_downloads_freeitems(void *item, void *data)
{
	(void) data;
	free(item);
	return 0;
}


/**
 * mb_downloads_populatelist() -- Populates the downloads list
 */
static enum mbt_result
mb_downloads_populatelist(int _id, void *data)
{
	int process_id, exit_status = -1;
	FILE *f;
	size_t n = 0;;
	char *str = NULL, *name = NULL, *id = NULL, *progress = NULL, *progressbar = NULL;
	char buf[512];
	char * const deluge_args[] =
	{
		"deluge-console",
		"connect",
		"127.0.0.1",
		"mediabox",
		"mediabox;",
		"info",
		NULL
	};


	(void) _id;
	(void) data;

	/* run the deluge-console process */
	if ((process_id = mb_process_start(DELUGE_BIN, deluge_args,
		MB_PROCESS_NICE | MB_PROCESS_SUPERUSER | MB_PROCESS_STDOUT_PIPE | MB_PROCESS_WAIT,
		"deluge-console", NULL, NULL)) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "downloads", "Could not execute deluge-console");
		return MB_TIMER_CALLBACK_RESULT_STOP;
	}

	f = fdopen(mb_process_openfd(process_id, STDOUT_FILENO), "r");
	if (f == NULL) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "download", "Could not open stream");
		return MB_TIMER_CALLBACK_RESULT_STOP;
	} else {
		while (getline(&str, &n, f) != -1) {
			if (str != NULL) {
				if (!memcmp(str, "Name: ", 6)) {
					str[strlen(str) - 1] = '\0';
					name = strdup(str + 6);
					if (name == NULL) {
						fprintf(stderr, "downloads: Out of memory\n");
					}
					/* fprintf(stderr, "Name: %s\n", name); */

				} else if (!memcmp("ID: ", str, 4)) {
					if (name != NULL) {
						str[strlen(str) - 1] = '\0';
						id = strdup(str + 4);
						if (id == NULL) {
							fprintf(stderr, "downloads: Out of memory\n");
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
							fprintf(stderr, "downloads: Out of memory\n");
							free(id);
							free(name);
							id = NULL;
							name = NULL;
						} else if ((progressbar = strdup(progressbar)) == NULL) {
							fprintf(stderr, "downloads: Out of memory\n");
							free(progress);
							free(id);
							free(name);
							progress = NULL;
							id = NULL;
							name = NULL;
						} else {
							snprintf(buf, 512, "%s (%s)",
								name, progress);

							mb_downloads_updateentry(id, buf);

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

		mb_download *dl;
		LIST_FOREACH_SAFE(mb_download*, dl, &downloads, {
			if (!dl->updated) {
				mb_ui_menu_removeitem(menu, dl->id);
				mbv_window_update(window);
				LIST_REMOVE(dl);
				free(dl->id);
				free(dl->name);
				free(dl);
			} else {
				dl->updated = 0;
			}
		});


	}

	mbv_window_update(window);

	/* wait for process to exit */
	if (mb_process_wait(process_id, &exit_status) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "download", "mb_process_wait() failed");
		return MB_TIMER_CALLBACK_RESULT_STOP;
	}

	return (exit_status == 0) ? MB_TIMER_CALLBACK_RESULT_CONTINUE
		: MB_TIMER_CALLBACK_RESULT_STOP;
}


/**
 * mb_downloads_init() -- Initialize the MediaBox downloads list
 */
int
mb_downloads_init(void)
{
	int xres, yres;
	int font_height;
	int window_height, window_width;
	int n_entries = 10;

	LIST_INIT(&downloads);

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
	window = mbv_window_new("DOWNLOADS",
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height, NULL);
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

	return 0;
}


int
mb_downloads_showdialog(void)
{
	struct timespec tv;
	int update_timer_id = -1;

	/* show the menu window */
        mbv_window_show(window);

	/* populate the list */
	mb_downloads_populatelist(0, NULL);

	/* register the update timer */
	tv.tv_sec = 2;
	tv.tv_nsec = 0;
	update_timer_id = mbt_register(&tv,
		MB_TIMER_TYPE_AUTORELOAD, -1, mb_downloads_populatelist, NULL);
	if (update_timer_id == -1) {
		mbv_window_hide(window);
		return -1;
	}

	/* show the menu widget and run it's input loop */
	if (mb_ui_menu_showdialog(menu) == 0) {
		char *selected = mb_ui_menu_getselected(menu);

		assert(selected != NULL);

		fprintf(stderr, "mb_mainmenu: Selected %s\n",
			selected);
	}

	/* cancel the update timer */
	mbt_cancel(update_timer_id);

	/* hide the mainmenu window */
	mbv_window_hide(window);

	return 0;
}


void
mb_downloads_destroy(void)
{
	fprintf(stderr, "downloads: Destroying instance\n");
	mb_ui_menu_enumitems(menu, mb_downloads_freeitems, NULL);
	mb_ui_menu_destroy(menu);
	mbv_window_destroy(window);
}

