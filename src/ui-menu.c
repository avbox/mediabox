/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "video.h"
#include "input.h"
#include "linkedlist.h"
#include "ui-menu.h"


#define FONT_PADDING (3)


/* Listable type for storing menuitem objects */
LISTABLE_TYPE(mb_ui_menuitem,
	struct mbv_window *window;
	char *name;
	int dirty;;
	void *data;
);


/**
 * Structure used to represent an instance of the menu widget.
 */
struct mb_ui_menu
{
	struct mbv_window *window;
	struct mbv_window **item_windows;
	mb_ui_menuitem *selected;
	int visible_items;
	int visible_window_offset;
	int dirty;
	int count;
	void *selection_changed_callback;
	mb_eol_callback end_of_list_callback;
	LIST_DECLARE(items);
};


/**
 * Gets the menuitem instance that corresponds to
 * a window
 */
static mb_ui_menuitem *
mb_ui_menu_getwindowitem(struct mb_ui_menu *inst, struct mbv_window *window)
{
	mb_ui_menuitem *item;
	LIST_FOREACH(mb_ui_menuitem*, item, &inst->items) {
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
mb_ui_menuitem_paint(struct mbv_window * const window)
{
	int canvas_width, canvas_height;
	struct mb_ui_menu * const inst = (struct mb_ui_menu*)
		mbv_window_getusercontext(window);
	mb_ui_menuitem * const item = (mb_ui_menuitem*)
		mb_ui_menu_getwindowitem(inst, window);

	assert(inst != NULL);

	if (item == NULL || !item->dirty) {
		/* const int dirty = (item != NULL) ? item->dirty : 0;
		DEBUG_VPRINT("ui-menu", "Not painting clean window (item=0x%p dirty=%i)!",
			item, dirty); */
		return 0;
	}

	/* DEBUG_VPRINT("ui-menu", "mb_ui_menuitem_paint(0x%p)",
		window); */

	/* get canvas size */
	mbv_window_getcanvassize(item->window,
		&canvas_width, &canvas_height);

	if (inst->selected == item) {
		mbv_window_setbgcolor(item->window, 0xffffffff);
		mbv_window_setcolor(item->window, 0x000000ff);
	} else {
		mbv_window_setbgcolor(item->window, MBV_DEFAULT_BACKGROUND);
		mbv_window_setcolor(item->window, MBV_DEFAULT_FOREGROUND);
	}

	/* paint the item and clear the dirty flag */
	mbv_window_clear(item->window);
	mbv_window_drawstring(item->window, item->name, canvas_width / 2, 5);
	item->dirty = 0;
	return 1;
}


/**
 * Changes the currently selected item.
 */
static int
mb_ui_menu_setselected(struct mb_ui_menu *inst, mb_ui_menuitem *item)
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
mb_ui_menu_setitemtext(struct mb_ui_menu *inst, void *item, char *text)
{
	mb_ui_menuitem *menuitem;

	assert(inst != NULL);
	assert(item != NULL);
	assert(text != NULL);

	LIST_FOREACH(mb_ui_menuitem*, menuitem, &inst->items) {
		if (menuitem->data == item) {
			assert(menuitem->name != NULL);
			free(menuitem->name);
			menuitem->name = strdup(text);
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
mb_ui_menu_enumitems(struct mb_ui_menu *inst, mb_ui_enumitems_callback callback, void *callback_data)
{
	mb_ui_menuitem *item;

	assert(inst != NULL);
	assert(callback != NULL);

	LIST_FOREACH_SAFE(mb_ui_menuitem*, item, &inst->items, {
		if (callback(item->data, callback_data)) {
			break;
		}
	});
}

void *
mb_ui_menu_getselected(struct mb_ui_menu *inst)
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
mb_ui_menu_scrollitems(struct mb_ui_menu *inst, int direction)
{
	int i, j;
	mb_ui_menuitem *item;

	if (direction == MB_UI_DIRECTION_DOWN) {
		inst->visible_window_offset++;
	} else if (direction == MB_UI_DIRECTION_UP) {
		assert(inst->visible_window_offset > 0);
		inst->visible_window_offset--;
	}
	i = j = 0;
	LIST_FOREACH(mb_ui_menuitem*, item, &inst->items) {
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
mb_ui_menu_additem(struct mb_ui_menu *inst, char *name, void *data)
{
	mb_ui_menuitem *item;

	assert(inst != NULL);
	assert(name != NULL);

	item = malloc(sizeof(mb_ui_menuitem));
	if (item == NULL) {
		fprintf(stderr, "mb_ui_menu: Add item failed: Out of memory\n");
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
		fprintf(stderr, "mb_ui_menu: Out of memory\n");
		free(item);
		return -1;
	}

	LIST_APPEND(&inst->items, item);

	inst->count++;

	return 0;
}


void
mb_ui_menu_removeitem(struct mb_ui_menu *inst, void *item)
{
	int next = 0;
	mb_ui_menuitem *menuitem, *prev = NULL;
	LIST_FOREACH_SAFE(mb_ui_menuitem*, menuitem, &inst->items, {
		if (menuitem->data == item) {
			LIST_REMOVE(menuitem);
			if (menuitem->window != NULL) {
				mbv_window_setbgcolor(menuitem->window, MBV_DEFAULT_BACKGROUND);
				mbv_window_clear(menuitem->window);
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
mb_ui_menu_clearitems(struct mb_ui_menu * const inst)
{
	mb_ui_menuitem* item;
	LIST_FOREACH_SAFE(mb_ui_menuitem*, item, &inst->items, {
		LIST_REMOVE(item);
		if (item->window != NULL) {
			mbv_window_setbgcolor(item->window, MBV_DEFAULT_BACKGROUND);
			mbv_window_clear(item->window);
		}
		free(item->name);
		free(item);
	});
	inst->count = 0;
	inst->selected = NULL;
}


/**
 * Show the menu and run it's message loop.
 */
int
mb_ui_menu_showdialog(struct mb_ui_menu *inst)
{
	int fd, quit = 0, ret = 0;
	enum avbox_input_event e;

	if (!mbv_window_isvisible(inst->window)) {
		DEBUG_PRINT("ui-menu", "Not showing invisible window!");
		return -1;
	}

	/* grab the input device */
	if ((fd = avbox_input_grab()) == -1) {
		fprintf(stderr, "mbs_show() -- mbi_grab_input failed\n");
		return -1;
	}

	/* run the message loop */
	while (!quit && avbox_input_getevent(fd, &e) != -1) {
		switch (e) {
		case MBI_EVENT_BACK:
		{
			close(fd);	/* relinquish input */
			quit = 1;	/* break out of loop */
			ret = 1; 	/* return failure since user backed out */
			break;
		}
		case MBI_EVENT_ENTER:
		{
			/* same as above but return success since the user
			 * selected an item */
			close(fd);
			quit = 1;
			ret = 0;
			break;
		}
		case MBI_EVENT_ARROW_UP:
		{
			mb_ui_menuitem *item, *prev = NULL, *selected = NULL;
			LIST_FOREACH(mb_ui_menuitem*, item, &inst->items) {
				if (inst->selected == item) {
					selected = prev;
				} else {
					prev = item;
				}
			}
			if (selected != NULL) {
				if (selected->window == NULL) {
					mb_ui_menu_scrollitems(inst, MB_UI_DIRECTION_UP);
				}
				mb_ui_menu_setselected(inst, selected);
				mbv_window_update(inst->window);
			}
			break;
		}
		case MBI_EVENT_ARROW_DOWN:
		{
			mb_ui_menuitem *item, *selected;
			int select_next;
start:
			selected = NULL;
			select_next = 0;
			LIST_FOREACH(mb_ui_menuitem*, item, &inst->items) {
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
					mb_ui_menu_scrollitems(inst, MB_UI_DIRECTION_DOWN);
				}
				mb_ui_menu_setselected(inst, selected);
				mbv_window_update(inst->window);
			} else {
				if (inst->end_of_list_callback) {
					if (inst->end_of_list_callback(inst) == 0) {
						goto start;
					}
				}
			}
			break;
		}
		default:
			/* fprintf(stderr, "mb_ui_menu: Received event %i\n", (int) e);*/
			break;
		}
	}
	return ret;
}


/**
 * Create a new instance of the menu widget.
 */
struct mb_ui_menu*
mb_ui_menu_new(struct mbv_window *window)
{
	int i, width, height;
	struct mb_ui_menu *inst;

	DEBUG_VPRINT("ui-menu", "mb_ui_menu_new(0x%p)",
		window);

	/* allocate memory for menu instance */
	inst = malloc(sizeof(struct mb_ui_menu));
	if (inst == NULL) {
		fprintf(stderr, "mb_ui_menu: Out of memory\n");
		return NULL;
	}

	/* initialize menu object */
	LIST_INIT(&inst->items);
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
	mbv_window_getcanvassize(window, &width, &height);
	inst->visible_items = height / itemheight;

	DEBUG_VPRINT("ui-menu", "Preallocating %i items",
		inst->visible_items);

	/* allocate memory for an array of pointers to
	 * window objects for each visible item */
	inst->item_windows = malloc(sizeof(struct mbv_window*) *
		inst->visible_items);
	if (inst->item_windows == NULL) {
		fprintf(stderr, "mb_ui_menu: Out of memory\n");
		free(inst);
		return NULL;
	}

	/* preallocate a window for each visible item */
	DEBUG_VPRINT("ui-menu", "Creating %i child windows",
		inst->visible_items);
	for (i = 0; i < inst->visible_items; i++) {
		inst->item_windows[i] = mbv_window_getchildwindow(inst->window,
			"menuitem", 0,
			itemheight * i, -1, itemheight, &mb_ui_menuitem_paint, inst);
		if (inst->item_windows[i] == NULL) {
			int j;

			DEBUG_PRINT("ui-menu", "Could not create preallocated window!");

			for (j = 0; j < i; j++) {
				mbv_window_destroy(inst->item_windows[j]);
			}
			free(inst->item_windows);
			free(inst);
			inst = NULL;
			break;
		}
		/* clear the window */
		mbv_window_clear(inst->item_windows[i]);
	}

	return inst;
}


int
mb_ui_menu_seteolcallback(struct mb_ui_menu *inst, mb_eol_callback callback)
{
	assert(inst != NULL);
	if (inst->end_of_list_callback != NULL) {
		fprintf(stderr, "ui-menu: Callback list not implemented yet\n");
		return -1;
	}
	inst->end_of_list_callback = callback;
	return 0;
}


/**
 * mb_ui_menu_destroy() -- Destroy an instance of the menu widget.
 */
void
mb_ui_menu_destroy(struct mb_ui_menu *inst)
{
	int i;

	DEBUG_VPRINT("ui-menu", "Destroying menu %p", inst);

	assert(inst != NULL);

	mb_ui_menu_clearitems(inst);

	DEBUG_VPRINT("ui-menu", "Destroying %i visible windows",
		inst->visible_items);
	for (i = 0; i < inst->visible_items; i++) {
		mbv_window_destroy(inst->item_windows[i]);
	}

	free(inst->item_windows);
	free(inst);
}

