#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>


#include "video.h"
#include "input.h"
#include "ui-menu.h"
#include "linkedlist.h"


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
static int mb_updater_quit = 0;

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
static int
mb_downloads_populatelist(void)
{
	pid_t pid;
	int pipefd[2];

	if (pipe(pipefd) == -1) {
		fprintf(stderr, "downloads: pipe() failed\n");
		return -1;
	}

	if ((pid = fork()) == -1) {
		fprintf(stderr, "downloads: fork() failed\n");
		return -1;

	} else if (pid != 0) { /* parent */
		int ret;
		FILE *f;
		size_t n = 0;;
		char *str = NULL, *name = NULL, *id = NULL, *progress = NULL, *progressbar = NULL;
		char buf[512];

		close(pipefd[1]);
		/* fprintf(stderr, "downloads: waiting for deluge-console...\n"); */

		f = fdopen(pipefd[0], "r");
		if (f == NULL) {
			fprintf(stderr, "download: Could not open stream.\n");
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
								name = NULL;
								progress = NULL;
								progressbar = NULL;
								id = NULL; /* freed later */
							}
						}
					}
					free(str);
					n = 0;
				}
			}
			fclose(f);

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

		while (waitpid(pid, &ret, 0) == -1) {
			if (errno == EINTR) {
				continue;
			}
		}

		/* fprintf(stderr, "downloads: deluge-console info returned %i\n", ret); */
		close(pipefd[0]);

		mbv_window_update(window);
		return 0;

	} else { /* child */
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
			fprintf(stderr, "downloads[child]: dup2() failed\n");
			exit(EXIT_FAILURE);
		}
		//10.10.0.130 fernan test; $@
		execv(DELUGE_BIN, (char * const[]) {
			strdup("deluge-console"),
			strdup("connect"),
			strdup("127.0.0.1"),
			strdup("mediabox"),
			strdup("mediabox;"),
			strdup("info"), NULL });
		exit(EXIT_FAILURE);
	}
	return 0;
}


static void *
mb_downloads_listupdater(void * arg)
{
	(void) arg;
	fprintf(stderr, "downloads: Worker thread running\n");
	while (!mb_updater_quit) {
		mb_downloads_populatelist();
	}
	return NULL;
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
	mb_updater_quit = 0;

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

	return 0;
}


int
mb_downloads_showdialog(void)
{
	pthread_t thread;

	/* show the menu window */
        mbv_window_show(window);

	#if 0
	/* populate the downloads list */
	if (mb_downloads_populatelist() == -1) {
		fprintf(stderr, "downloads: populatelist() failed\n");
		return -1;
	}
	#endif

	if (pthread_create(&thread, NULL, mb_downloads_listupdater, NULL) != 0) {
		fprintf(stderr, "downloads: Could not start updater thread\n");
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

	mb_updater_quit = 1;
	pthread_join(thread, NULL);

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

