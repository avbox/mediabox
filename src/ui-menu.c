#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "video.h"
#include "input.h"
#include "linkedlist.h"
#include "ui-menu.h"


/* Listable type for storing menuitem objects */
LISTABLE_TYPE(mb_ui_menuitem,
	struct mbv_window *window;
	char *name;
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
	int count;
	void *selection_changed_callback;
	LIST_DECLARE(items);
};


static void
mb_ui_menuitem_update(struct mb_ui_menu *inst, mb_ui_menuitem *item)
{
	int canvas_width, canvas_height;

	assert(inst != NULL);
	assert(item != NULL);

	/* get canvas size */
	mbv_window_getcanvassize(item->window, &canvas_width, &canvas_height);

	if (inst->selected == item) {
		mbv_window_clear(inst->selected->window,  0xffffffff);
		mbv_window_setcolor(inst->selected->window, 0x000000ff);
		mbv_window_drawstring(inst->selected->window, inst->selected->name, canvas_width / 2, 5);
	} else {
		mbv_window_clear(item->window, 0x3349ffff);
		mbv_window_setcolor(item->window, 0xffffffff);
		mbv_window_drawstring(item->window, item->name, canvas_width / 2, 5);
	}
}



/**
 * mb_ui_menu_setselected() -- Changes the currently selected item.
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

	/* if there is a selected item then unselect it */
	if (inst->selected != NULL) {
		mb_ui_menuitem *selected = inst->selected;
		inst->selected = NULL;
		mb_ui_menuitem_update(inst, selected);
	}

	/* select the new item */
	inst->selected = item;
	mb_ui_menuitem_update(inst, inst->selected);

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
			mb_ui_menuitem_update(inst, menuitem);
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
			item->window = inst->item_windows[j++];
			mb_ui_menuitem_update(inst, item);
		} else {
			item->window = NULL;
		}
	}
	mbv_window_update(inst->window);
}


/**
 * mb_ui_menu_additem() -- Adds a new item to a menu widget.
 */
int
mb_ui_menu_additem(struct mb_ui_menu *inst, char *name, void *data)
{
	mb_ui_menuitem *item;
	int canvas_width, canvas_height;

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

		mbv_window_getcanvassize(item->window, &canvas_width, &canvas_height);

		/* if there's no selected item make this one it */
		if (inst->selected == NULL) {
			inst->selected = item;
			mbv_window_clear(item->window, 0xFFFFFFFF);
			mbv_window_setcolor(item->window, 0x000000FF);
		} else {
			mbv_window_clear(item->window, 0x3349ffff);
			mbv_window_setcolor(item->window, 0xffffffff);
		}

		/* draw the menu item */
		mbv_window_drawstring(item->window, name, canvas_width / 2, 5);
	} else {
		item->window = NULL;
	}

	item->name = strdup(name);
	item->data = data;

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
				mbv_window_clear(menuitem->window, 0x3349ffff);
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
mb_ui_menu_clearitems(struct mb_ui_menu *inst)
{
	mb_ui_menuitem* item;
	LIST_FOREACH_SAFE(mb_ui_menuitem*, item, &inst->items, {
		LIST_REMOVE(item);
		if (item->window != NULL) {
			mbv_window_clear(item->window, 0x3349ffff);
		}
		free(item->name);
		free(item);
	});
	mbv_window_update(inst->window);
	inst->count = 0;
	inst->selected = NULL;
}


/**
 * mb_ui_menu_showdialog() -- Show the menu and run it's
 * message loop
 */
int
mb_ui_menu_showdialog(struct mb_ui_menu *inst)
{
	int fd, quit = 0, ret = 0;
	mbi_event e;

	/* grab the input device */
	if ((fd = mbi_grab_input()) == -1) {
		fprintf(stderr, "mbs_show() -- mbi_grab_input failed\n");
		return -1;
	}

	/* run the message loop */
	while (!quit && read_or_eof(fd, &e, sizeof(mbi_event)) != 0) {
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
			mb_ui_menuitem *item, *selected = NULL;
			int select_next = 0;
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
			}
			break;
		}
		default:
			fprintf(stderr, "mb_ui_menu: Received event %i\n", (int) e);
		}
	}
	return ret;
}


/**
 * mb_ui_menu_new() -- Create a new instance of the menu widget.
 */
struct mb_ui_menu*
mb_ui_menu_new(struct mbv_window *window)
{
	int i, width, height;
	struct mb_ui_menu *inst;

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
	inst->count = 0;

	/* calculate item height */
	int itemheight = mbv_getdefaultfontheight();
	itemheight += 10;


	/* get the widget window size and calculate the
	 * number of visible items */
	mbv_window_getcanvassize(window, &width, &height);
	inst->visible_items = height / itemheight;

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
	for (i = 0; i < inst->visible_items; i++) {
		inst->item_windows[i] = mbv_window_getchildwindow(inst->window, 0,
			itemheight * i, -1, itemheight);
		if (inst->item_windows[i] == NULL) {
			int j;
			for (j = 0; j < i; j++) {
				mbv_window_destroy(inst->item_windows[j]);
			}
			free(inst->item_windows);
			free(inst);
			break;
		}
	}

	return inst;
}


/**
 * mb_ui_menu_destroy() -- Destroy an instance of the menu widget.
 */
void
mb_ui_menu_destroy(struct mb_ui_menu *inst)
{
	int i;

	assert(inst != NULL);

	mb_ui_menu_clearitems(inst);

	for (i = 0; i < inst->visible_items; i++) {
		mbv_window_destroy(inst->item_windows[i]);
	}

	free(inst->item_windows);
	free(inst);
}

