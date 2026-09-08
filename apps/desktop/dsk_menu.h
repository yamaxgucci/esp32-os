/*
 * ArgonOS DESKTOP - the menu bar, and what drops out of it.
 *
 * One bar, along the top of the desktop, as Program Manager had.  There is no
 * right button on this system worth designing for - Windows 3.11 had no
 * context menus either, and a touchscreen has no second button at all - so
 * everything the shell can do has to be reachable from here.
 *
 * The menu is drawn over whatever is under it and remembers nothing about it:
 * closing damages the rectangle it covered and the ordinary repaint puts the
 * desktop and its windows back.  That is cheaper than a saved bitmap and it is
 * the only way that stays correct when a window moves underneath.
 *
 * Pure C, like the window manager, so the host tests can walk it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_MENU_H
#define ARGON_DSK_MENU_H

#include "dsk.h"

#define DSK_MENU_MAX       6  /* titles on the bar                        */
#define DSK_MENU_ITEMS_MAX 16 /* items under one title                    */

/* id 0 means "does nothing", which is what a separator and a heading are. */
typedef struct {
    const char *label;
    uint16_t    id;
    bool        separator;
    bool        enabled;
    bool        checked;
} dsk_menu_item_t;

typedef struct {
    const char      *title;
    dsk_menu_item_t  items[DSK_MENU_ITEMS_MAX];
    uint8_t          n;
} dsk_menu_t;

/*
 * `chose` is called with the id of the item picked, once, after the menu has
 * closed - so a handler is free to open a window or start a drag without
 * fighting the menu that is still on screen.
 */
void dsk_menu_init(const dsk_metrics_t *m, void (*damage)(dsk_rect_t r),
                   void (*chose)(uint16_t id));

/* The bar's contents.  The array must outlive the menu; nothing is copied. */
void dsk_menu_set(dsk_menu_t *menus, int n);
/* Items change between openings (a window list, a greyed-out entry). */
dsk_menu_t *dsk_menu_get(int which);

bool dsk_menu_open(void); /* is a drop-down showing? */
void dsk_menu_close(void);

/* Where a title sits on the bar, for hit-testing and for drawing. */
dsk_rect_t dsk_menu_title_rect(int which);
/* Where the drop-down for a title goes, and where one of its items is. */
dsk_rect_t dsk_menu_drop_rect(int which);
dsk_rect_t dsk_menu_item_rect(int which, int item);

/* -1 when the point is not on the bar. */
int dsk_menu_title_at(int16_t x, int16_t y);
/* -1 when the point is not on an item of the open drop-down. */
int dsk_menu_item_at(int16_t x, int16_t y);

void dsk_menu_draw_bar(dsk_rect_t area);
void dsk_menu_draw_open(dsk_rect_t area);

/* True when the menu used the event up. */
bool dsk_menu_pointer(dsk_ptr_t type, int16_t x, int16_t y);
bool dsk_menu_key(uint16_t keycode, uint32_t unicode, uint16_t mods);

#endif /* ARGON_DSK_MENU_H */
