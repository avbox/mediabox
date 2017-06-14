/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define LOG_MODULE "ui-menu"

#include "../debug.h"
#include "../log.h"
#include "../linkedlist.h"
#include "../dispatch.h"
#include "video.h"
#include "input.h"
#include "listview.h"


#define FONT_PADDING (3)


/* Listable type for storing menuitem objects */
LISTABLE_STRUCT(avbox_listitem,
	struct avbox_window *window;
	char *name;
	int dirty;
	void *data;
);


/**
 * Structure used to represent an instance of the menu widget.
 */
struct avbox_listview
{
	struct avbox_window *window;
	struct avbox_window **item_windows;
	struct avbox_object *notify_object;
	struct avbox_object *dispatch_object;
	struct avbox_listitem *selected;
	int visible_items;
	int visible_window_offset;
	int dirty;
	int count;
	void *selection_changed_callback;
	void *eol_callback_context;
	avbox_listview_eol_fn end_of_list_callback;
	LIST_DECLARE(items);
};


/**
 * Gets the menuitem instance that corresponds to
 * a window
 */
static struct avbox_listitem *
avbox_listview_getwindowitem(struct avbox_listview *inst, struct avbox_window *window)
{
	struct avbox_listitem *item;
	LIST_FOREACH(struct avbox_listitem*, item, &inst->items) {
		if (item->window == window) {
			return item;
		}
	}
	return NULL;
}


/**
 * Paints the menu item.
 */
static int
avbox_listitem_paint(struct avbox_window * const window)
{
	struct avbox_listview * const inst = (struct avbox_listview*)
		avbox_window_getusercontext(window);
	struct avbox_listitem * const item = (struct avbox_listitem*)
		avbox_listview_getwindowitem(inst, window);
	struct avbox_rect rect;

	assert(inst != NULL);

	if (item == NULL || !item->dirty) {
		/* const int dirty = (item != NULL) ? item->dirty : 0;
		DEBUG_VPRINT("ui-menu", "Not painting clean window (item=0x%p dirty=%i)!",
			item, dirty); */
		return 0;
	}

	/* DEBUG_VPRINT("ui-menu", "avbox_listitem_paint(0x%p)",
		window); */

	/* get canvas size */
	rect.x = 0;
	rect.y = 0;
	avbox_window_getcanvassize(item->window,
		&rect.w, &rect.h);
	avbox_window_setbgcolor(item->window, MBV_DEFAULT_BACKGROUND);
	avbox_window_clear(item->window);

	if (inst->selected == item) {
		avbox_window_setbgcolor(item->window, AVBOX_COLOR(0xffffffff));
		avbox_window_roundrectangle(item->window, &rect, 0, 2);
		avbox_window_setcolor(item->window, AVBOX_COLOR(0x000000ff));
	} else {
		avbox_window_setcolor(item->window, MBV_DEFAULT_FOREGROUND);
	}

	/* paint the item and clear the dirty flag */
	avbox_window_drawstring(item->window, item->name, rect.w / 2, 5);
	item->dirty = 0;
	return 1;
}


/**
 * Changes the currently selected item.
 */
static int
avbox_listview_setselected(struct avbox_listview *inst, struct avbox_listitem *item)
{
	assert(inst != NULL);
	assert(item != NULL);

	/* check if already selected/nothing to do */
	if (inst->selected == item) {
		return 0;
	}

	if (inst->selected != NULL) {
		inst->selected->dirty = 1;
	}

	/* select the new item */
	inst->selected = item;
	inst->selected->dirty = 1;

	/* this is where we invoke the callback function. For now
	 * we just SIGABRT if it's set since it's not implemented yet. */
	if (inst->selection_changed_callback != NULL) {
		abort();
	}

	return 0;
}


int
avbox_listview_setitemtext(struct avbox_listview *inst, void *item, char *text)
{
	struct avbox_listitem *menuitem;

	assert(inst != NULL);
	assert(item != NULL);
	assert(text != NULL);

	LIST_FOREACH(struct avbox_listitem*, menuitem, &inst->items) {
		if (menuitem->data == item) {
			assert(menuitem->name != NULL);
			free(menuitem->name);
			menuitem->name = strdup(text);
			menuitem->dirty = 1;
			if (menuitem->name == NULL) {
				fprintf(stderr, "downloads: Out of memory\n");
				return -1;
			}
			return 0;
		}
	}
	return -1;
}


void
avbox_listview_enumitems(struct avbox_listview *inst, avbox_listview_enumitems_fn callback, void *callback_data)
{
	struct avbox_listitem *item;

	assert(inst != NULL);
	assert(callback != NULL);

	LIST_FOREACH_SAFE(struct avbox_listitem*, item, &inst->items, {
		if (callback(item->data, callback_data)) {
			break;
		}
	});
}


void *
avbox_listview_getselected(struct avbox_listview *inst)
{
	assert(inst != NULL);

	if (inst->selected == NULL) {
		return (void*) NULL;
	} else {
		return inst->selected->data;
	}
}


#define MB_UI_DIRECTION_NONE	(0)
#define MB_UI_DIRECTION_UP	(1)
#define MB_UI_DIRECTION_DOWN	(2)


static void
avbox_listview_scrollitems(struct avbox_listview *inst, int direction)
{
	int i, j;
	struct avbox_listitem *item;

	if (direction == MB_UI_DIRECTION_DOWN) {
		inst->visible_window_offset++;
	} else if (direction == MB_UI_DIRECTION_UP) {
		assert(inst->visible_window_offset > 0);
		inst->visible_window_offset--;
	}
	i = j = 0;
	LIST_FOREACH(struct avbox_listitem*, item, &inst->items) {
		if (i++ < inst->visible_window_offset) {
			item->window = NULL;
		} else if (j < inst->visible_items) {
			if (item->window != inst->item_windows[j]) {
				item->dirty = 1;
			}
			item->window = inst->item_windows[j++];
		} else {
			item->window = NULL;
		}
	}
}


/**
 * Adds a new item to a menu widget.
 */
int
avbox_listview_additem(struct avbox_listview *inst, char *name, void *data)
{
	struct avbox_listitem *item;

	assert(inst != NULL);
	assert(name != NULL);

	item = malloc(sizeof(struct avbox_listitem));
	if (item == NULL) {
		fprintf(stderr, "avbox_listview: Add item failed: Out of memory\n");
		return -1;
	}

	if (inst->count < inst->visible_items) {
		/* grab a preallocated subwindow */
		item->window = inst->item_windows[inst->count];
		assert(item->window != NULL);

		/* if there's no selected item make this one it */
		if (inst->selected == NULL) {
			inst->selected = item;
		}
	} else {
		item->window = NULL;
	}

	item->name = strdup(name);
	item->data = data;
	item->dirty = 1;

	if (item->name == NULL) {
		fprintf(stderr, "avbox_listview: Out of memory\n");
		free(item);
		return -1;
	}

	LIST_APPEND(&inst->items, item);

	inst->count++;

	return 0;
}


void
avbox_listview_removeitem(struct avbox_listview *inst, void *item)
{
	int next = 0;
	struct avbox_listitem *menuitem, *prev = NULL;
	LIST_FOREACH_SAFE(struct avbox_listitem*, menuitem, &inst->items, {
		if (menuitem->data == item) {
			LIST_REMOVE(menuitem);
			if (menuitem->window != NULL) {
				avbox_window_setbgcolor(menuitem->window, MBV_DEFAULT_BACKGROUND);
				avbox_window_clear(menuitem->window);
			}
			free(menuitem->name);
			free(menuitem);
			inst->count--;
			if (inst->selected == item) {
				if (prev != NULL) {
					inst->selected = prev->data;
				} else {
					inst->selected = NULL;
					next = 1;
				}
			}
		} else if (next) {
			inst->selected = menuitem->data;
			break;
		}
		prev = menuitem;
	});
}


void
avbox_listview_clearitems(struct avbox_listview * const inst)
{
	struct avbox_listitem* item;
	LIST_FOREACH_SAFE(struct avbox_listitem*, item, &inst->items, {
		LIST_REMOVE(item);
		if (item->window != NULL) {
			avbox_window_setbgcolor(item->window, MBV_DEFAULT_BACKGROUND);
			avbox_window_clear(item->window);
		}
		free(item->name);
		free(item);
	});
	inst->count = 0;
	inst->selected = NULL;
}


/**
 * Focus input on listview.
 */
int
avbox_listview_focus(struct avbox_listview *inst)
{
	int fd;

	if (!avbox_window_isvisible(inst->window)) {
		DEBUG_PRINT("ui-menu", "Not showing invisible window!");
		return -1;
	}

	/* grab the input device */
	if ((fd = avbox_input_grab(inst->dispatch_object)) == -1) {
		fprintf(stderr, "mbs_show() -- mbi_grab_input failed\n");
		return -1;
	}

	return 0;
}


/**
 * Release focus
 */
void
avbox_listview_releasefocus(struct avbox_listview * const inst)
{
	ASSERT(inst != NULL);
	ASSERT(inst->dispatch_object != NULL);
	avbox_input_release(inst->dispatch_object);
}


/**
 * Handles incoming messages.
 */
static int
avbox_listview_messagehandler(void *context, struct avbox_message *msg)
{
	struct avbox_listview *inst = context;

	switch (avbox_dispatch_getmsgtype(msg)) {
	case AVBOX_MESSAGETYPE_INPUT:
	{
		struct avbox_input_message *ev =
			avbox_dispatch_getmsgpayload(msg);

		switch (ev->msg) {
		case MBI_EVENT_BACK:
		{
			/* send dismiss message to parent */
			if (avbox_object_sendmsg(&inst->notify_object,
				AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
				LOG_VPRINT_ERROR("Could not send dismiss message: %s",
					strerror(errno));
			}
			break;
		}
		case MBI_EVENT_ENTER:
		{
			if (inst->selected != NULL) {
				/* send SELECTED message to parent */
				if (avbox_object_sendmsg(&inst->notify_object,
					AVBOX_MESSAGETYPE_SELECTED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
					LOG_VPRINT_ERROR("Could not send selected message: %s",
						strerror(errno));
				}
			}
			break;
		}
		case MBI_EVENT_ARROW_UP:
		{
			struct avbox_listitem *item, *prev = NULL, *selected = NULL;
			LIST_FOREACH(struct avbox_listitem*, item, &inst->items) {
				if (inst->selected == item) {
					selected = prev;
				} else {
					prev = item;
				}
			}
			if (selected != NULL) {
				if (selected->window == NULL) {
					avbox_listview_scrollitems(inst, MB_UI_DIRECTION_UP);
				}
				avbox_listview_setselected(inst, selected);
				avbox_window_update(inst->window);
			}
			break;
		}
		case MBI_EVENT_ARROW_DOWN:
		{
			struct avbox_listitem *item, *selected;
			int select_next;
start:
			selected = NULL;
			select_next = 0;
			LIST_FOREACH(struct avbox_listitem*, item, &inst->items) {
				if (select_next) {
					selected = item;
					select_next = 0;
				} else {
					if (inst->selected == item) {
						select_next = 1;
					}
				}
			}
			if (selected != NULL) {
				if (selected->window == NULL) {
					avbox_listview_scrollitems(inst, MB_UI_DIRECTION_DOWN);
				}
				avbox_listview_setselected(inst, selected);
				avbox_window_update(inst->window);
			} else {
				if (inst->end_of_list_callback) {
					if (inst->end_of_list_callback(inst, inst->eol_callback_context) == 0) {
						goto start;
					}
				}
			}
			break;
		}
		default:
			return AVBOX_DISPATCH_CONTINUE;
		}
		avbox_input_eventfree(ev);
		break;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		DEBUG_VPRINT("ui-menu", "Destroying listview %p", inst);
		ASSERT(inst != NULL);

		avbox_input_release(inst->dispatch_object);
		avbox_listview_clearitems(inst);

		DEBUG_VPRINT("ui-menu", "Destroying %i visible windows",
			inst->visible_items);
		for (int i = 0; i < inst->visible_items; i++) {
			avbox_window_destroy(inst->item_windows[i]);
		}
		break;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
		DEBUG_VPRINT("ui-menu", "Cleaning up listview %p", inst);
		free(inst->item_windows);
		free(inst);
		break;
	default:
		/* if it's an anycast message don't process it */
		return AVBOX_DISPATCH_CONTINUE;
	}
	return AVBOX_DISPATCH_OK;
}

/**
 * Create a new instance of the menu widget.
 */
struct avbox_listview*
avbox_listview_new(struct avbox_window *window, struct avbox_object *notify_object)
{
	int i, width, height;
	struct avbox_listview *inst;

	DEBUG_VPRINT("ui-menu", "avbox_listview_new(0x%p)",
		window);

	/* allocate memory for menu instance */
	inst = malloc(sizeof(struct avbox_listview));
	if (inst == NULL) {
		fprintf(stderr, "avbox_listview: Out of memory\n");
		return NULL;
	}

	/* create a dispatch object */
	if ((inst->dispatch_object = avbox_object_new(
		avbox_listview_messagehandler, inst)) == NULL) {
		LOG_PRINT_ERROR("Could not create dispatch object!");
		free(inst);
		return NULL;
	}

	/* initialize menu object */
	LIST_INIT(&inst->items);
	inst->notify_object = notify_object;
	inst->window = window;
	inst->visible_window_offset = 0;
	inst->selected = NULL;
	inst->selection_changed_callback = NULL;
	inst->end_of_list_callback = NULL;
	inst->count = 0;

	/* calculate item height */
	int itemheight = mbv_getdefaultfontheight();
	itemheight += FONT_PADDING;

	/* get the widget window size and calculate the
	 * number of visible items */
	avbox_window_getcanvassize(window, &width, &height);
	inst->visible_items = height / itemheight;

	DEBUG_VPRINT("ui-menu", "Preallocating %i items",
		inst->visible_items);

	/* allocate memory for an array of pointers to
	 * window objects for each visible item */
	inst->item_windows = malloc(sizeof(struct avbox_window*) *
		inst->visible_items);
	if (inst->item_windows == NULL) {
		fprintf(stderr, "avbox_listview: Out of memory\n");
		free(inst);
		return NULL;
	}

	/* preallocate a window for each visible item */
	DEBUG_VPRINT("ui-menu", "Creating %i child windows",
		inst->visible_items);
	for (i = 0; i < inst->visible_items; i++) {
		char windowid[16];
		snprintf(windowid, sizeof(windowid), "menuitem_%d", i + 1);
		inst->item_windows[i] = avbox_window_new(inst->window,
			windowid, AVBOX_WNDFLAGS_SUBWINDOW, 0,
			itemheight * i, -1, itemheight,
			NULL, &avbox_listitem_paint, inst);
		if (inst->item_windows[i] == NULL) {
			int j;

			DEBUG_PRINT("ui-menu", "Could not create preallocated window!");

			for (j = 0; j < i; j++) {
				avbox_window_destroy(inst->item_windows[j]);
			}
			free(inst->item_windows);
			free(inst);
			inst = NULL;
			break;
		}
		/* clear the window */
		avbox_window_clear(inst->item_windows[i]);
	}

	return inst;
}


int
avbox_listview_seteolcallback(struct avbox_listview *inst, avbox_listview_eol_fn callback, void *context)
{
	assert(inst != NULL);
	if (inst->end_of_list_callback != NULL) {
		fprintf(stderr, "ui-menu: Callback list not implemented yet\n");
		return -1;
	}
	inst->end_of_list_callback = callback;
	inst->eol_callback_context = context;
	return 0;
}


/**
 * Destroy an instance of the menu widget.
 */
void
avbox_listview_destroy(struct avbox_listview *inst)
{
	DEBUG_PRINT("listview", "Listview destructor called.");
	avbox_object_destroy(inst->dispatch_object);
}
