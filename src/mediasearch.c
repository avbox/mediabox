#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>


#include "video.h"
#include "input.h"
#include "ui-menu.h"
#include "url_util.h"
#include "linkedlist.h"
#include "downloads-backend.h"
#include "time_util.h"
#include "debug.h"


#define MB_MEDIASEARCH_STATE_NONE       (0)
#define MB_MEDIASEARCH_STATE_CATEGORIES (1)
#define MB_MEDIASEARCH_STATE_ITEMS      (2)


LISTABLE_TYPE(mb_searchresult,
	char *name;
	char *url;
);


static struct mbv_window *window = NULL;
static struct mb_ui_menu *menu = NULL;
static int state = MB_MEDIASEARCH_STATE_NONE;
static int items_count = 0;
static int input_quit = 0;
static int updater_quit = 0;
static char *terms = NULL;
static char *cat = NULL;
static size_t terms_sz = 0;
static pthread_mutex_t menu_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t updater;
static pthread_mutex_t updater_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t updater_signal = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t terms_lock = PTHREAD_MUTEX_INITIALIZER;


static int
mb_mediasearch_search(char *terms, unsigned int skip, unsigned int count)
{
	#define ITEMS_PER_PAGE (25)
	char *content = NULL;
	size_t content_length = 0;
	char *strptr, *end;
	char url[255];
	unsigned int page = 0, skipped = 0;

	static struct timespec tv = { 0, 0 };

	(void) terms;


	if (skip) {
		page = skip / ITEMS_PER_PAGE;
		skipped = page * ITEMS_PER_PAGE;
	}

	/* fprintf(stderr, "skip=%i, count=%i, page=%i, skipped=%i\n",
		skip, count, page, skipped); */

	while (count) {

		page++;

		if (terms != NULL && strcmp(terms, "")) {
			snprintf(url, 255-1, "https://kat.cr/usearch/%s category:%s/%i/",
				terms, cat, page);

		} else {
			snprintf(url, 255-1, "https://kat.cr/%s/%i/", cat, page);
		}

		fprintf(stderr, "mediasearch: Fetching page %s...\n", url);

		if (tv.tv_sec || tv.tv_nsec) {
			struct timespec nt;
			(void) clock_gettime(CLOCK_MONOTONIC, &nt);
			if (utimediff(&nt, &tv) < 1000L * 1000 * 1000 * 5) {
				sleep(5);
			}
		}

		if (mb_url_fetch2mem(url, (void**) &content, &content_length) == -1) {
			fprintf(stderr, "mediasearch: mb_url_fetch2mem() failed\n");
			return -1;
		}
	
		(void) clock_gettime(CLOCK_MONOTONIC, &tv);


		if (strstr(content, "Nothing found!") != NULL) {
			free(content);
			return 0;
		}

		if ((strptr = strstr(content, "<tr class=\"firstr\">")) != NULL) {
			end = strstr(strptr, "</table>");
			*end = '\0';

			while (count > 0 && (strptr = strstr(strptr + 1, "<tr")) != NULL) {
				size_t sz;
				char *buf, *data_start, *end, *name, *magnet;

				if (skipped < skip) {
					skipped++;
					continue;
				}
				
				if ((data_start = strstr(strptr, "data-sc-params=\"")) != NULL) {
					data_start += 16;
					if ((name = strstr(data_start, "'name': '")) != NULL) {
						name += 9;
						if ((end = strstr(name, "'")) != NULL) {
							sz = end - name;
							if ((buf = malloc(sz + 1)) != NULL) {
								memcpy(buf, name, sz);
								buf[sz] = '\0';
								urldecode(buf, buf);
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
						/* fprintf(stderr, "Added %s\n", name); */
						free(name);
						count--;
						items_count++;
						/* free(magnet) -- later */

					}
				}
			}
		} else {
			fprintf(stderr, "Invalid input\n");
			fprintf(stderr, "Content %s\n", content);
			return -1;
		}
		free(content);
	}

	mbv_window_update(window);

	return 0;
}


#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)

/**
 * mb_mediasearch_freeitems() -- Called back by mb_ui_menu_enumitems(). Used to free
 * item list entries
 */
static int
mb_mediasearch_freeitems(void *item, void *data)
{
	(void) data;
	free(item);
	return 0;
}


static void *
mb_mediasearch_resultsupdater(void *arg)
{
	char *last_terms = NULL, *tmp;

	MB_DEBUG_SET_THREAD_NAME("searchupdater");

	assert(terms != NULL);

	while (!updater_quit) {

		pthread_mutex_lock(&updater_lock);
		if (last_terms != NULL && !strcmp(last_terms, terms)) {
			/* fprintf(stderr, "mediasearch: Waiting\n"); */
			pthread_cond_wait(&updater_signal, &updater_lock);
			if (updater_quit) {
				pthread_mutex_unlock(&updater_lock);
				continue;
			}
		} else {
			/* fprintf(stderr, "mediasearch: last_terms='%s' terms='%s'\n",
				last_terms, terms); */
		}
		pthread_mutex_unlock(&updater_lock);

		pthread_mutex_lock(&terms_lock);
		if ((tmp = strdup(terms)) == NULL) {
			fprintf(stderr, "mediasearch: strdup() failed\n");
			pthread_mutex_unlock(&terms_lock);
			continue;
		}
		pthread_mutex_unlock(&terms_lock);

		pthread_mutex_lock(&menu_lock);

		mb_ui_menu_enumitems(menu, mb_mediasearch_freeitems, NULL);
		mb_ui_menu_clearitems(menu);

		items_count = 0;
		if (mb_mediasearch_search(tmp, items_count, 25) == 0) {
			mbv_window_update(window);
		} else {
			fprintf(stderr, "mediasearch: search() failed\n");
		}


		pthread_mutex_unlock(&menu_lock);

		if (last_terms != NULL) {
			free(last_terms);
		}
		if ((last_terms = strdup(tmp)) == NULL) {
			fprintf(stderr, "mediasearch: strdup() failed 2\n");
		}
		free(tmp);
	}
	if (last_terms != NULL) {
		free(last_terms);
	}
	/* fprintf(stderr, "updater exiting\n"); */
	return NULL;
}

static int
mb_mediasearch_appendtoterms(char *c)
{
	int ret = 0;

	assert(terms != NULL || terms_sz == 0);

	pthread_mutex_lock(&terms_lock);

	if (*c == '\b' && strlen(terms) > 0) {
		terms[strlen(terms) - 1] = '\0';
		goto end;
	}

	if (terms == NULL || terms_sz <= strlen(terms) + 2) {
		terms_sz += 25;
		char *newterms;
		if ((newterms = realloc(terms, terms_sz)) == NULL) {
			fprintf(stderr, "mediasearch: Out of memory\n");
			terms_sz -= 25;
			ret = -1;
			goto end;
		} else {
			terms = newterms;
		}	
	}
	strcat(terms, c);
end:
	pthread_mutex_unlock(&terms_lock);
	return ret;
}


static void *
mb_mediasearch_inputthread(void *arg)
{
	int fd;
	enum avbox_input_event e;

#define CASE_KBD(x) \
	case MBI_EVENT_KBD_ ## x: \
		mb_mediasearch_appendtoterms(STRINGIZE(x)); \
		istext = 1; \
		break;

	(void) arg;

	/* grab the input device */
	if ((fd = avbox_input_grabnonblock()) == -1) {
		fprintf(stderr, "mbs_show() -- mbi_grab_input failed\n");
		return NULL;
	}

	while (input_quit == 0 && avbox_input_getevent(fd, &e) != -1) {
		int istext = 0;

		switch (e) {
		case MBI_EVENT_CLEAR:
			mb_mediasearch_appendtoterms("\b");
			istext = 1;
			break;
		case MBI_EVENT_KBD_SPACE:
			mb_mediasearch_appendtoterms(" ");
			istext = 1;
			break;
		CASE_KBD(A)
		CASE_KBD(B)
		CASE_KBD(C)
		CASE_KBD(D)
		CASE_KBD(E)
		CASE_KBD(F)
		CASE_KBD(G)
		CASE_KBD(H)
		CASE_KBD(I)
		CASE_KBD(J)
		CASE_KBD(K)
		CASE_KBD(L)
		CASE_KBD(M)
		CASE_KBD(N)
		CASE_KBD(O)
		CASE_KBD(P)
		CASE_KBD(Q)
		CASE_KBD(R)
		CASE_KBD(S)
		CASE_KBD(T)
		CASE_KBD(U)
		CASE_KBD(V)
		CASE_KBD(W)
		CASE_KBD(X)
		CASE_KBD(Y)
		CASE_KBD(Z)
		default:
			break;
		}

		if (istext) {
			char *title;

			if ((title = malloc(strlen(terms) + 14 + 1)) != NULL) {
				snprintf(title, strlen(terms) + 14 + 1, "MEDIA SEARCH: %s",
					terms);
				mbv_window_settitle(window, title);
				mbv_window_update(window);

				fprintf(stderr, "mediasearch: signaling\n");
				pthread_mutex_lock(&updater_lock);
				pthread_cond_signal(&updater_signal);
				pthread_mutex_unlock(&updater_lock);
				free(title);
			}
		}
	}

	close(fd);

	return NULL;

#undef CASE_KBD
}


/**
 * mb_mediasearch_endoflist() -- Called by the menu widget when it reaches the end of the list.
 */
int
mb_mediasearch_endoflist(struct mb_ui_menu *inst)
{
	if (state == MB_MEDIASEARCH_STATE_ITEMS) {
		(void) inst;
		pthread_mutex_lock(&menu_lock);
		if (mb_mediasearch_search(terms, items_count, 25) == 0) {
			mbv_window_update(window);
			pthread_mutex_unlock(&menu_lock);
			return 0;
		} else {
			fprintf(stderr, "mediasearch: search() failed\n");
		}
		pthread_mutex_unlock(&menu_lock);
	}
	return -1;
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

	if ((terms = malloc(1)) == NULL) {
		fprintf(stderr, "mediasearch: Out of memory\n");
		return -1;
	}

	state = MB_MEDIASEARCH_STATE_NONE;
	terms_sz = 1;
	strcpy(terms, "");

	/* set height according to font size */
	mbv_window_getcanvassize(mbv_getrootwindow(), &xres, &yres);
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
	window = mbv_window_new("mediasearch", "FIND MEDIA",
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height, NULL);
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

	/* set the end of list callback function */
	mb_ui_menu_seteolcallback(menu, mb_mediasearch_endoflist);

	return 0;
}


int
mb_mediasearch_showdialog(void)
{
	pthread_t input_thread;
	char *selected;
	int quit = 0;

	/* show the menu window */
        mbv_window_show(window);

	mb_ui_menu_additem(menu, "Movies", "MOV");
	mb_ui_menu_additem(menu, "TV Shows", "TV");
	state = MB_MEDIASEARCH_STATE_CATEGORIES;
	mbv_window_update(window);

	while (!quit && mb_ui_menu_showdialog(menu) == 0) {

		selected = mb_ui_menu_getselected(menu);

		if (!strcmp(selected, "MOV")) {
			cat = "movies";
		} else if (!strcmp(selected, "TV")) {
			cat = "tv";
		} else {
			abort();
		}

		mb_ui_menu_clearitems(menu);
		state = MB_MEDIASEARCH_STATE_ITEMS;
		mbv_window_update(window);

		input_quit = 0;
		if (pthread_create(&input_thread, NULL, mb_mediasearch_inputthread, NULL) != 0) {
			fprintf(stderr, "mediasearch: Could not start input thread;\n");
			return -1;
		}

		updater_quit = 0;
		if (pthread_create(&updater, NULL, mb_mediasearch_resultsupdater, NULL) != 0) {
			fprintf(stderr, "mediasearch: Could not start updater thread\n");
			pthread_mutex_unlock(&updater_lock);
			input_quit = 1;
			avbox_input_dispatchevent(MBI_EVENT_NONE);
			pthread_join(input_thread, NULL);
			return -1;
		}

		/* show the menu widget and run it's input loop */
		if (mb_ui_menu_showdialog(menu) == 0) {
			selected = mb_ui_menu_getselected(menu);

			assert(selected != NULL);

			if (mb_downloadmanager_addurl(selected) == -1) {
				fprintf(stderr, "mediasearch: deluge_add() failed\n");
			}

			fprintf(stderr, "mediasearch: Selected %s\n",
				selected);
			quit = 1;
		}

		input_quit = 1;
		avbox_input_dispatchevent(MBI_EVENT_NONE);
		pthread_join(input_thread, NULL);
		updater_quit = 1;
		pthread_cond_signal(&updater_signal);
		pthread_join(updater, NULL);

		mb_ui_menu_enumitems(menu, mb_mediasearch_freeitems, NULL);
		mb_ui_menu_clearitems(menu);
		mb_ui_menu_additem(menu, "Movies", "MOV");
		mb_ui_menu_additem(menu, "TV Shows", "TV");
		state = MB_MEDIASEARCH_STATE_CATEGORIES;
		mbv_window_update(window);
	}

	return 0;
}


void
mb_mediasearch_destroy(void)
{
	/* fprintf(stderr, "mediasearch: Destroying instance\n"); */
	if (state == MB_MEDIASEARCH_STATE_ITEMS) {
		mb_ui_menu_enumitems(menu, mb_mediasearch_freeitems, NULL);
	}
	mb_ui_menu_destroy(menu);
	mbv_window_destroy(window);
	if (terms != NULL) {
		free(terms);
	}
}

