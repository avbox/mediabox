#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>


#include "video.h"
#include "input.h"
#include "ui-menu.h"
#include "url_util.h"
#include "linkedlist.h"


LISTABLE_TYPE(mb_searchresult,
	char *name;
	char *url;
);


static struct mbv_window *window = NULL;
static struct mb_ui_menu *menu = NULL;

static int
html_decode(char *dst, char *src)
{
	if (src == NULL) {
		src = dst;
	}

	if (dst == NULL) {
		return -1;
	}

	while (*src != '\0') {
		if (src[0] == '%' && src[1] == '2' && src[2] == '0') {
			*dst++ = ' ';
			src += 3; 
		} else {
			*dst++ = *src++;
		}
	}
	*dst = '\0';
	return 0;
}

static int
mb_mediasearch_search(void *provider, char *terms)
{
	char *content = NULL;
	size_t content_length = 0;
	char *strptr, *end;

	(void) provider;
	(void) terms;

	if (mb_url_fetch2mem("https://kat.cr/movies/", (void**) &content, &content_length) == -1) {
		fprintf(stderr, "mediasearch: mb_url_fetch2mem() failed\n");
		return -1;
	}

	if ((strptr = strstr(content, "<tr class=\"firstr\">")) != NULL) {
		end = strstr(strptr, "</table>");
		*end = '\0';

		while ((strptr = strstr(strptr + 1, "<tr")) != NULL) {
			size_t sz;
			char *buf, *data_start, *end, *name, *magnet;
			
			if ((data_start = strstr(strptr, "data-sc-params=\"")) != NULL) {
				data_start += 16;
				if ((name = strstr(data_start, "'name': '")) != NULL) {
					name += 9;
					if ((end = strstr(name, "'")) != NULL) {
						sz = end - name;
						if ((buf = malloc(sz + 1)) != NULL) {
							memcpy(buf, name, sz);
							buf[sz] = '\0';
							html_decode(buf, NULL);
							if ((name = strdup(buf)) == NULL) {
								fprintf(stderr, "mediasearch: Out of memory\n");
							}
							free(buf);
						} else {
							fprintf(stderr, "mediaseach: Out of memory\n");
							name = NULL;
						}
					} else {
						fprintf(stderr, "mediasearch: Invalid input\n");
						name = NULL;
					}
				}

				if ((magnet = strstr(data_start, "'magnet': '")) != NULL) {
					magnet += 11;
					if ((end = strstr(magnet, "'")) != NULL) {
						sz = end - magnet;
						if ((buf = malloc(sz + 1)) != NULL) {
							memcpy(buf, magnet, sz);
							buf[sz] = '\0';
							if ((magnet = strdup(buf)) == NULL) {
								fprintf(stderr, "mediasearch: Out of memory\n");
							}
							free(buf);
						} else {
							fprintf(stderr, "mediasearch: Out of memory\n");
							magnet = NULL;
						}
					} else {
						fprintf(stderr, "mediasearch: Invalid input\n");
						magnet = NULL;
					}
				}
				if (name != NULL && magnet != NULL) {
					mb_ui_menu_additem(menu, name, magnet);
					free(name);
					/* free(magnet) -- later */

				}
			}
		}
	} else {
		fprintf(stderr, "Invalid input\n");
	}


	if (content != NULL) {
		free(content);
	}

	mbv_window_update(window);

	return 0;
}


/**
 * mb_downloads_init() -- Initialize the MediaBox downloads list
 */
int
mb_mediasearch_init(void)
{
	int xres, yres;
	int font_height;
	int window_height, window_width;
	int n_entries = 10;

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
	window = mbv_window_new("FIND MEDIA",
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height);
	if (window == NULL) {
		fprintf(stderr, "mediasearch: Could not create new window!\n");
		return -1;
	}

	/* create a new menu widget inside main window */
	menu = mb_ui_menu_new(window);
	if (menu == NULL) {
		fprintf(stderr, "mediasearch: Could not create menu\n");
		return -1;
	}

	return 0;
}


int
mb_mediasearch_showdialog(void)
{
	/* show the menu window */
        mbv_window_show(window);

	mb_mediasearch_search(NULL, NULL);

	/* show the menu widget and run it's input loop */
	if (mb_ui_menu_showdialog(menu) == 0) {
		char *selected = mb_ui_menu_getselected(menu);

		assert(selected != NULL);

		if (mb_downloadmanager_addurl(selected) == -1) {
			fprintf(stderr, "mediasearch: deluge_add() failed\n");
		}

		fprintf(stderr, "mediasearch: Selected %s\n",
			selected);
	}

	return 0;
}


void
mb_mediasearch_destroy(void)
{
	fprintf(stderr, "mediasearch: Destroying instance\n");
	mb_ui_menu_destroy(menu);
	mbv_window_destroy(window);
}

