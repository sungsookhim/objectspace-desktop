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
#define WPS_MI_OPEN_WITH      10
#define WPS_MI_ARRANGE        20
#define WPS_MI_DELETE         40    /* to the trash (asks first, as OS/2 does) */
#define WPS_MI_CREATE_ANOTHER 41    /* a new object of the same kind, beside it */
/* Added by the GUI to the desktop's own menu (G4). */
#define WPS_MI_WINDOW_LIST    50
#define WPS_MI_MINIMIZED      51
/* Added by the GUI to every folder's background menu (the desktop's too). */
#define WPS_MI_OPEN_PARENT    52
#define WPS_MI_FIND           53
#define WPS_MI_SORT           54    /* cascade: arrange the icons in an order */
#define WPS_MI_SORT_NAME      55
#define WPS_MI_SORT_TYPE      56
#define WPS_MI_SORT_SIZE      57
#define WPS_MI_SORT_DATE      58
#define WPS_MI_CLOSE          59    /* not the desktop's */
/* The Trash Can (v0.2): carried out by the GUI (it asks first). */
#define WPS_MI_EMPTY_TRASH    60
#define WPS_MI_RESTORE        61    /* an item in the trash: back where it was */
#define WPS_MI_DELETE_FOREVER 62
#define WPS_MI_MOUNT          63    /* drives (v0.2-design 5) */
#define WPS_MI_UNMOUNT        64
#define WPS_MI_EJECT          65
#define WPS_MI_POWER_OFF      66
#define WPS_MI_UNLOCK         67
#define WPS_MI_LOCK           68
/* Added by the GUI to the desktop's own menu (v0.3-design 2). */
#define WPS_MI_SYSTEM_SETUP   69    /* opens the System Setup folder */
#define WPS_MI_LOCKUP         70
#define WPS_MI_SHUTDOWN       71
/* Added by the GUI to an icon's menu in icon view (v0.3-design 5). */
#define WPS_MI_LOCK_POS       72
/* A data file's Open cascade (v0.3-design 7), from wpsd: its type's
 * programs (opened with "app_index" = id - WPS_MI_OPEN_APP), then Other
 * program... (a picker). */
#define WPS_MI_OPEN_OTHER    199
#define WPS_MI_OPEN_APP      200
#define WPS_MI_OPEN_APPS      48
/* New > on a folder's background menu (v0.3-design 9): the templates,
 * WPS_MI_NEW + k, by title (made at the click's spot). */
#define WPS_MI_NEW_MENU      299
#define WPS_MI_NEW           300
#define WPS_MI_NEWS           64
/* Added by the GUI itself to a folder window's background menu. */
#define WPS_MI_VIEW           30    /* cascade: switch this window's view */
#define WPS_MI_VIEW_ICON      31
#define WPS_MI_VIEW_TREE      32
#define WPS_MI_VIEW_DETAILS   33
#define WPS_MI_FIRST_CLASS  1000

#endif
