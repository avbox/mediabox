#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "video.h"
#include "input.h"
#include "linkedlist.h"


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
	int count;
	void *selection_changed_callback;
	LIST_DECLARE(items);
};


/**
 * mb_ui_menu_setselected() -- Changes the currently selected item.
 */
static int
mb_ui_menu_setselected(struct mb_ui_menu *inst, mb_ui_menuitem *item)
{
	int canvas_width, canvas_height;

	assert(inst != NULL);
	assert(item != NULL);

	/* check if already selected/nothing to do */
	if (inst->selected == item) {
		return 0;
	}

	/* get canvas size */
	mbv_window_getcanvassize(item->window, &canvas_width, &canvas_height);

	/* if there is a selected item then unselect it */
	if (inst->selected != NULL) {
		mbv_window_clear(inst->selected->window,  0x33, 0x49, 0xff, 0xFF);
		mbv_window_setcolor(inst->selected->window, 0xffffffff);
		mbv_window_drawstring(inst->selected->window, inst->selected->name, canvas_width / 2, 5);
	}

	/* select the new item */
	mbv_window_clear(item->window, 0xff, 0xff, 0xff, 0xff);
	mbv_window_setcolor(item->window, 0x000000ff);
	mbv_window_drawstring(item->window, item->name, canvas_width / 2, 5);

	/* store it */
	inst->selected = item;

	/* this is where we invoke the callback function. For now
	 * we just SIGABRT if it's set since it's not implemented yet. */
	if (inst->selection_changed_callback != NULL) {
		abort();
	}

	return 0;
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

	/* create a subwindow for the item */
	item->window = mbv_window_getchildwindow(inst->window, 0, 25 * inst->count, -1, 25);
	if (item->window == NULL) {
		fprintf(stderr, "mb_ui_menu: Could not create child window\n");
		free(item);
		return -1;
	}

	mbv_window_getcanvassize(item->window, &canvas_width, &canvas_height);

	/* if there's no selected item make this one it */
	if (inst->selected == NULL) {
		inst->selected = item;
		mbv_window_clear(item->window, 0xFF, 0xFF, 0xFF, 0xFF);
		mbv_window_setcolor(item->window, 0x000000FF);
	}

	/* draw the menu item */
	mbv_window_drawstring(item->window, name, canvas_width / 2, 5);

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


/**
 * mb_ui_menu_showdialog() -- Show the menu and run it's
 * message loop
 */
int
mb_ui_menu_showdialog(struct mb_ui_menu *inst)
{
	int fd, quit = 0, ret = 0;
	mbi_event e;
	mbv_window_show(inst->window);

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
			fprintf(stderr, "mb_ui_menu: BACK button pressed\n");
			close(fd);	/* relinquish input */
			quit = 1;	/* break out of loop */
			ret = 1; 	/* return failure since user backed out */
			break;
		}
		case MBI_EVENT_ENTER:
		{
			/* same as above but return success since the user
			 * selected an item */
			fprintf(stderr, "mb_ui_menu: ENTER button pressed\n");
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
				mb_ui_menu_setselected(inst, selected);
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
				mb_ui_menu_setselected(inst, selected);
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
	inst->selected = NULL;
	inst->selection_changed_callback = NULL;
	inst->count = 0;

	return inst;
}


/**
 * mb_ui_menu_destroy() -- Destroy an instance of the menu widget.
 */
void
mb_ui_menu_destroy(struct mb_ui_menu *inst)
{
	mb_ui_menuitem *item;

	assert(inst != NULL);

	LIST_FOREACH_SAFE(mb_ui_menuitem*, item, &inst->items, {
		LIST_REMOVE(item);
		mbv_window_destroy(item->window);
		free(item->name);
		free(item);
	});

	free(inst);
}

