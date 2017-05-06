#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#define LOG_MODULE "mediasearch"

#include "lib/ui/video.h"
#include "lib/ui/listview.h"
#include "lib/ui/input.h"
#include "lib/url_util.h"
#include "lib/linkedlist.h"
#include "lib/time_util.h"
#include "lib/debug.h"
#include "lib/dispatch.h"
#include "lib/thread.h"
#include "lib/application.h"
#include "downloads-backend.h"


#define AVBOX_MEDIASEARCH_STATE_NONE       (0)
#define AVBOX_MEDIASEARCH_STATE_CATEGORIES (1)
#define AVBOX_MEDIASEARCH_STATE_ITEMS      (2)


struct mbox_mediasearch
{
	struct avbox_window *window;
	struct avbox_listview *menu;
	struct avbox_dispatch_object *dispatch_object;
	struct avbox_dispatch_object *parent_object;
	int state;
	char *terms;
	char *last_terms;
	size_t terms_sz;
	int items_count;
	int updater_quit;
	char *cat;
	pthread_mutex_t terms_lock;
};


struct avbox_additem_args
{
	struct avbox_listview *inst;
	char *name;
	char *url;
};


/**
 * Called back by avbox_listview_enumitems(). Used to free
 * item list entries
 */
static int
mbox_mediasearch_freeitems(void *item, void *data)
{
	(void) data;
	free(item);
	return 0;
}


/**
 * Clear the list.
 */
static void *
mbox_mediasearch_clearlist(void *arg)
{
	struct mbox_mediasearch * const inst = arg;
	avbox_listview_enumitems(inst->menu, mbox_mediasearch_freeitems, NULL);
	avbox_listview_clearitems(inst->menu);
	inst->items_count = 0;
	return NULL;
}


static void *
mbox_mediasearch_additem(void *arg)
{
	struct avbox_additem_args * const args = arg;
	avbox_listview_additem(args->inst, args->name, args->url);
	return NULL;
}


static int
mbox_mediasearch_search(struct mbox_mediasearch *inst,
	char *terms, unsigned int skip, unsigned int count)
{
	#define ITEMS_PER_PAGE (25)
	char *content = NULL;
	size_t content_length = 0;
	char *strptr, *end;
	char url[255];
	unsigned int page = 0, skipped = 0;

	static struct timespec tv = { 0, 0 };

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
				terms, inst->cat, page);

		} else {
			snprintf(url, 255-1, "https://kat.cr/%s/%i/", inst->cat, page);
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
						struct avbox_delegate *del;
						struct avbox_additem_args args;
						args.inst = inst->menu;
						args.name = name;
						args.url = magnet;

						/* add the item to the list from the main thread */
						if ((del = avbox_application_delegate(
							mbox_mediasearch_additem, &args)) == NULL) {
							LOG_VPRINT_ERROR("Could not delegate call 'additem' to main thread: %s",
								strerror(errno));
						} else {
							avbox_delegate_wait(del, NULL);
						}

						free(name);
						count--;
						inst->items_count++;
						/* free(magnet) -- later */

					}
				}
			}
		} else {
			LOG_VPRINT_ERROR("Invalid input (content=\"%s\")",
				content);
			return -1;
		}
		free(content);
	}

	avbox_window_update(inst->window);

	return 0;
}


#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)

static void *
mbox_mediasearch_dosearch(void *arg)
{
	char *tmp;
	struct mbox_mediasearch * const inst = arg;

	DEBUG_PRINT("mediasearch", "Performing search");

	pthread_mutex_lock(&inst->terms_lock);
	if ((tmp = strdup(inst->terms)) == NULL) {
		LOG_PRINT_ERROR("strdup() failed!");
		return NULL;
	}
	pthread_mutex_unlock(&inst->terms_lock);

	/* clear the list */
	struct avbox_delegate *del;
	if ((del = avbox_application_delegate(
		mbox_mediasearch_clearlist, inst)) == NULL) {
		LOG_VPRINT_ERROR("Could not delegate call 'clearlist' to main thread: %s",
			strerror(errno));
	} else {
		avbox_delegate_wait(del, NULL);
	}

	/* search with the new terms */
	if (mbox_mediasearch_search(inst, tmp, inst->items_count, 25) == 0) {
		avbox_window_update(inst->window);
	} else {
		LOG_PRINT_ERROR("Search failed!");
	}

	if (inst->last_terms != NULL) {
		free(inst->last_terms);
	}
	if ((inst->last_terms = strdup(tmp)) == NULL) {
		LOG_PRINT_ERROR("Out of memory");
	}
	free(tmp);
	return NULL;
}


static int
mbox_mediasearch_appendtoterms(struct mbox_mediasearch * const inst, char *c)
{
	int ret = 0;

	assert(inst->terms != NULL || inst->terms_sz == 0);

	pthread_mutex_lock(&inst->terms_lock);

	if (*c == '\b' && strlen(inst->terms) > 0) {
		inst->terms[strlen(inst->terms) - 1] = '\0';
		goto end;
	}

	if (inst->terms == NULL || inst->terms_sz <= strlen(inst->terms) + 2) {
		inst->terms_sz += 25;
		char *newterms;
		if ((newterms = realloc(inst->terms, inst->terms_sz)) == NULL) {
			LOG_PRINT_ERROR("Out of memory");
			inst->terms_sz -= 25;
			ret = -1;
			assert(errno == ENOMEM);
			goto end;
		} else {
			inst->terms = newterms;
		}	
	}
	strcat(inst->terms, c);
end:
	pthread_mutex_unlock(&inst->terms_lock);
	return ret;
}


/**
 * Called by the menu widget when it reaches the end of the list.
 */
int
mbox_mediasearch_endoflist(struct avbox_listview *inst, void *context)
{
	struct mbox_mediasearch * const me = context;

	if (me->state == AVBOX_MEDIASEARCH_STATE_ITEMS) {
		(void) inst;
		if (mbox_mediasearch_search(me, me->terms, me->items_count, 25) == 0) {
			avbox_window_update(me->window);
			return 0;
		} else {
			LOG_PRINT_ERROR("Search failed!");
		}
	}
	return -1;
}


static int
mbox_mediasearch_msghandler(void *context, struct avbox_message *msg)
{
	struct mbox_mediasearch * const inst = context;
#define CASE_KBD(x) \
	case MBI_EVENT_KBD_ ## x: \
		mbox_mediasearch_appendtoterms(inst, STRINGIZE(x)); \
		istext = 1; \
		break;

	switch (avbox_dispatch_getmsgtype(msg)) {
	case AVBOX_MESSAGETYPE_INPUT:
	{
		int istext = 0;
		struct avbox_input_message * const ev =
			avbox_dispatch_getmsgpayload(msg);
		switch (ev->msg) {
		case MBI_EVENT_CLEAR:
			mbox_mediasearch_appendtoterms(inst, "\b");
			istext = 1;
			break;
		case MBI_EVENT_KBD_SPACE:
			mbox_mediasearch_appendtoterms(inst, " ");
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
			return AVBOX_DISPATCH_CONTINUE;
		}

		if (istext) {
			char *title;

			if ((title = malloc(strlen(inst->terms) + 14 + 1)) != NULL) {
				snprintf(title, strlen(inst->terms) + 14 + 1, "MEDIA SEARCH: %s",
					inst->terms);
				avbox_window_settitle(inst->window, title);
				avbox_window_update(inst->window);

				struct avbox_delegate *del;
				if ((del = avbox_thread_delegate(mbox_mediasearch_dosearch, inst)) == NULL) {
					LOG_PRINT_ERROR("Could not call search delegate!");
				} else {
					avbox_delegate_wait(del, NULL);
				}
				free(title);
			}
		}

		avbox_input_eventfree(ev);
		break;
	}
	case AVBOX_MESSAGETYPE_DISMISSED:
	{
		break;
	}
	case AVBOX_MESSAGETYPE_SELECTED:
	{
		char * selected = avbox_listview_getselected(inst->menu);

		switch (inst->state) {
		case AVBOX_MEDIASEARCH_STATE_CATEGORIES:
		{
			if (!strcmp(selected, "MOV")) {
				inst->cat = "movies";
			} else if (!strcmp(selected, "TV")) {
				inst->cat = "tv";
			} else {
				DEBUG_ABORT("mediasearch", "list state corrupted!");
			}

			avbox_listview_clearitems(inst->menu);
			inst->state = AVBOX_MEDIASEARCH_STATE_ITEMS;
			avbox_window_update(inst->window);
			break;
		}
		case AVBOX_MEDIASEARCH_STATE_ITEMS:
		{
			char *selected = avbox_listview_getselected(inst->menu);

			assert(selected != NULL);

			/* send url to download manager */
			if (mb_downloadmanager_addurl(selected) == -1) {
				LOG_VPRINT_ERROR("Could not add '%s' to downloads list!",
					selected);
			}

			DEBUG_VPRINT("mediasearch", "Downloading '%s'",
				selected);

			/* hide the window */
			avbox_window_hide(inst->window);

			/* reset state */
			inst->state = AVBOX_MEDIASEARCH_STATE_CATEGORIES;
			avbox_listview_enumitems(inst->menu, mbox_mediasearch_freeitems, NULL);
			avbox_listview_clearitems(inst->menu);
			avbox_listview_additem(inst->menu, "Movies", "MOV");
			avbox_listview_additem(inst->menu, "TV Shows", "TV");
			avbox_window_update(inst->window);

			/* send DISMISSED message */
			if (avbox_dispatch_sendmsg(-1, &inst->parent_object,
				AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
				LOG_PRINT_ERROR("Could not send DISMISSED message!");
			}

			break;
		}
		default:
			DEBUG_ABORT("mediasearch", "Invalid state!");
		}
	}
	default:
		abort();
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Initialize the MediaBox mediasearch tool.
 */
struct mbox_mediasearch*
mbox_mediasearch_new(struct avbox_dispatch_object *parent)
{
	int xres, yres;
	int font_height;
	int window_height, window_width;
	int n_entries = 10;
	struct mbox_mediasearch *inst;

	/* allocate memory for instance */
	if ((inst = malloc(sizeof(struct mbox_mediasearch))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	/* allocate memory for search terms */
	if ((inst->terms = malloc(1)) == NULL) {
		assert(errno == ENOMEM);
		free(inst);
		return NULL;
	}

	/* initialize context */
	inst->state = AVBOX_MEDIASEARCH_STATE_NONE;
	inst->parent_object = parent;
	inst->cat = NULL;
	inst->updater_quit = 0;
	inst->terms_sz = 1;
	strcpy(inst->terms, "");

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
	inst->window = avbox_window_new(NULL, "mediasearch",
		AVBOX_WNDFLAGS_DECORATED,
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height, NULL, NULL, NULL);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create new window!");
		free(inst->terms);
		free(inst);
		return NULL;
	}
	if (avbox_window_settitle(inst->window, "MEDIA SEARCH") == -1) {
		LOG_VPRINT_ERROR("Could not set window title: %s",
			strerror(errno));
	}

	/* create dispatch object */
	if ((inst->dispatch_object = avbox_dispatch_createobject(
		mbox_mediasearch_msghandler, 0, inst)) == NULL) {
		LOG_VPRINT_ERROR("Could not create dispatch object: %s",
			strerror(errno));
		avbox_window_destroy(inst->window);
		free(inst->terms);
		free(inst);
	}

	/* create a new menu widget inside main window */
	inst->menu = avbox_listview_new(inst->window, inst->dispatch_object);
	if (inst->menu == NULL) {
		LOG_PRINT_ERROR("Could not create listview!");
		avbox_window_destroy(inst->window);
		free(inst->terms);
		free(inst);
		return NULL;
	}

	/* initialize listview */
	avbox_listview_additem(inst->menu, "Movies", "MOV");
	avbox_listview_additem(inst->menu, "TV Shows", "TV");
	avbox_listview_seteolcallback(inst->menu, mbox_mediasearch_endoflist, inst);

	/* initialize context */
	inst->terms = NULL;
	inst->last_terms = NULL;
	inst->state = AVBOX_MEDIASEARCH_STATE_CATEGORIES;

	return 0;
}


int
mbox_mediasearch_show(struct mbox_mediasearch * const inst)
{
	/* show the menu window */
        avbox_window_show(inst->window);

	if (avbox_listview_focus(inst->menu) == -1) {
		LOG_PRINT_ERROR("Could not show listview!");
		return -1;
	}

	return 0;
}


void
mbox_mediasearch_destroy(struct mbox_mediasearch * const inst)
{
	if (inst->state == AVBOX_MEDIASEARCH_STATE_ITEMS) {
		avbox_listview_enumitems(inst->menu, mbox_mediasearch_freeitems, NULL);
	}

	/* free resources */
	avbox_listview_destroy(inst->menu);
	avbox_window_destroy(inst->window);
	if (inst->terms != NULL) {
		free(inst->terms);
	}
}
