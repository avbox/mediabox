/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_UI_MENU_H__
#define __MB_UI_MENU_H__


struct mb_ui_menu;


typedef int (*mb_ui_enumitems_callback)(void *item, void *data);
typedef int (*mb_eol_callback)(struct mb_ui_menu *inst);



int
mb_ui_menu_seteolcallback(struct mb_ui_menu *inst, mb_eol_callback callback);


void
mb_ui_menu_removeitem(struct mb_ui_menu *inst, void *item);


int
mb_ui_menu_setitemtext(struct mb_ui_menu *inst, void *item, char *text);


void
mb_ui_menu_enumitems(struct mb_ui_menu *inst, mb_ui_enumitems_callback callback, void *callback_data);


void *
mb_ui_menu_getselected(struct mb_ui_menu *inst);


void
mb_ui_menu_clearitems(struct mb_ui_menu *inst);


int
mb_ui_menu_additem(struct mb_ui_menu *inst, char *name, void *data);

int
mb_ui_menu_showdialog(struct mb_ui_menu *inst);


struct mb_ui_menu*
mb_ui_menu_new(struct mbv_window *window);


void
mb_ui_menu_destroy(struct mb_ui_menu *inst);

#endif
