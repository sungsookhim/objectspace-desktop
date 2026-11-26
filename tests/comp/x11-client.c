/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * An X11 test client for osd-compositor (XWayland): one window with a
 * title and a class, filled with a colour; with "popup", also an
 * override-redirect window (as an X11 menu is) 40 pixels inside it. It
 * logs the root's size at the start, its size when the window manager
 * configures it, where a button is pressed in it, and exits when asked
 * to close (WM_DELETE_WINDOW).
 *
 * With "menu": as an X11 menu (xterm's): an override-redirect window
 * mapped while a button is held in the window, unmapped when it is let go.
 *
 * usage: x11-client TITLE [RRGGBB] [popup|menu]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

static xcb_atom_t atom(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

int main(int argc, char **argv)
{
    const char *title = argc > 1 ? argv[1] : "X11 test";
    uint32_t color = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 16) : 0x308040;
    int popup = argc > 3 && !strcmp(argv[3], "popup"), menu = argc > 3 && !strcmp(argv[3], "menu");
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) { fprintf(stderr, "x11-client: no X display\n"); return 1; }
    xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t w = xcb_generate_id(c);
    fprintf(stderr, "x11-client: root %ux%u\n", scr->width_in_pixels, scr->height_in_pixels);
    uint32_t vals[] = { color, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_KEY_PRESS };
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, scr->root, 0, 0, 360, 240, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
    static const char cls[] = "x11-client\0X11Client";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof cls, cls);
    xcb_atom_t protocols = atom(c, "WM_PROTOCOLS"), del = atom(c, "WM_DELETE_WINDOW");
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, protocols, XCB_ATOM_ATOM, 32, 1, &del);
    uint32_t pid = (uint32_t)getpid();   /* as real programs do: what Terminate ends */
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom(c, "_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_map_window(c, w);
    if (popup) {
        xcb_window_t p = xcb_generate_id(c);
        uint32_t pv[] = { 0xe0e0e0, 1 };
        xcb_create_window(c, XCB_COPY_FROM_PARENT, p, scr->root, 0, 0, 120, 80, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          scr->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, pv);
        xcb_map_window(c, p);
    }
    xcb_window_t m = XCB_NONE;
    if (menu) {
        m = xcb_generate_id(c);
        uint32_t mv[] = { 0xf0f0f0, 1 };
        xcb_create_window(c, XCB_COPY_FROM_PARENT, m, scr->root, 0, 0, 100, 140, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          scr->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, mv);
    }
    xcb_flush(c);
    xcb_generic_event_t *e;
    int placed = 0;
    while ((e = xcb_wait_for_event(c))) {
        int type = e->response_type & 0x7f;
        if (type == XCB_CONFIGURE_NOTIFY) {
            xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *)e;
            if (ce->window == w) {
                fprintf(stderr, "x11-client: configured %ux%u+%d+%d\n", ce->width, ce->height, ce->x, ce->y);
                if (popup && !placed) placed = 1;
            }
        } else if (type == XCB_BUTTON_PRESS) {
            xcb_button_press_event_t *be = (xcb_button_press_event_t *)e;
            fprintf(stderr, "x11-client: pressed at %d,%d\n", be->event_x, be->event_y);
            if (m) {   /* the menu, at the pointer, while the button is held */
                uint32_t at[] = { (uint32_t)be->root_x, (uint32_t)be->root_y };
                xcb_configure_window(c, m, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, at);
                xcb_map_window(c, m);
                xcb_flush(c);
            }
        } else if (type == XCB_BUTTON_RELEASE) {
            if (m) { xcb_unmap_window(c, m); xcb_flush(c); fprintf(stderr, "x11-client: menu closed\n"); }
        } else if (type == XCB_KEY_PRESS) {
            xcb_key_press_event_t *ke = (xcb_key_press_event_t *)e;
            fprintf(stderr, "x11-client: key %u\n", ke->detail);
        } else if (type == XCB_CLIENT_MESSAGE) {
            xcb_client_message_event_t *cm = (xcb_client_message_event_t *)e;
            if (cm->type == protocols && cm->data.data32[0] == del) {
                fprintf(stderr, "x11-client: asked to close\n");
                free(e);
                break;
            }
        }
        free(e);
    }
    xcb_disconnect(c);
    return 0;
}
