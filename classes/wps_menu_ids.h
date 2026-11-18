/*
 * Well-known menu item IDs. Classes put these items in their menus; the
 * GUI carries them out itself (opening views, the settings notebook), and
 * sends every other item to the service as menu_select. Class and plug-in
 * items use IDs from 1000 up.
 */
#ifndef WPS_MENU_IDS_H
#define WPS_MENU_IDS_H

#define WPS_MI_OPEN            1    /* default view, or a cascade of views */
#define WPS_MI_PROPERTIES      2    /* the settings notebook */
#define WPS_MI_OPEN_ICON      11
#define WPS_MI_OPEN_TREE      12
#define WPS_MI_OPEN_DETAILS   13
#define WPS_MI_ARRANGE        20
#define WPS_MI_FIRST_CLASS  1000

#endif
