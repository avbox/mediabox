/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_UI_MENU_H__
#define __MB_UI_MENU_H__
#include "../dispatch.h"
#include "video.h"


struct avbox_listview;


typedef int (*avbox_listview_enumitems_fn)(void *item, void *data);
typedef int (*avbox_listview_eol_fn)(struct avbox_listview *inst, void * context);


/**
 * Release focus
 */
void
avbox_listview_releasefocus(struct avbox_listview * const inst);


int
avbox_listview_seteolcallback(struct avbox_listview *inst, avbox_listview_eol_fn callback, void *context);


void
avbox_listview_removeitem(struct avbox_listview *inst, void *item);


int
avbox_listview_setitemtext(struct avbox_listview *inst, void *item, char *text);


void
avbox_listview_enumitems(struct avbox_listview *inst, avbox_listview_enumitems_fn callback, void *callback_data);


void *
avbox_listview_getselected(struct avbox_listview *inst);


void
avbox_listview_clearitems(struct avbox_listview *inst);


int
avbox_listview_additem(struct avbox_listview *inst, char *name, void *data);

int
avbox_listview_focus(struct avbox_listview *inst);


struct avbox_listview*
avbox_listview_new(struct avbox_window *window,
	struct avbox_object *notify_object);


void
avbox_listview_destroy(struct avbox_listview *inst);

#endif
