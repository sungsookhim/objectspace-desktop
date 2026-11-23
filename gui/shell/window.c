/* wps-shell: A folder window: its search field, drawing, layout, pointer and keys, closing. */
#define _GNU_SOURCE
#include "shell.h"

static ui_rect find_row(struct folder *f)
{
    ui_rect c = ui_frame_client(f->win);
    return (ui_rect){ c.x, c.y, c.w, FIND_H };
}

static ui_rect find_entry_rect(struct folder *f) { ui_rect r = find_row(f); return (ui_rect){ r.x + 68, r.y + 4, r.w - 68 - 44, FIND_H - 9 }; }

static ui_rect find_x_rect(struct folder *f) { ui_rect r = find_row(f); return (ui_rect){ r.x + r.w - 36, r.y + 4, 28, FIND_H - 9 }; }

static void find_draw(struct folder *f, cairo_t *cr)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect r = find_row(f);
    ui_fill(cr, r, k->face);
    ui_fill(cr, (ui_rect){ r.x, r.y + r.h - 1, r.w, 1 }, k->shadow);
    /* a magnifier, and "Find:" */
    ui_set_color(cr, k->text);
    cairo_set_line_width(cr, 2);
    cairo_arc(cr, r.x + 14, r.y + 13, 5, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 3);
    cairo_move_to(cr, r.x + 18, r.y + 17); cairo_line_to(cr, r.x + 23, r.y + 22);
    cairo_stroke(cr);
    ui_text_draw(cr, UI_FONT_REGULAR, r.x + 28, r.y + (FIND_H + ui_text_ascent()) / 2 - 1, "Find:", 40);
    ui_rect er = find_entry_rect(f);
    ui_entry_draw(f->find_entry, cr, er, f->find_focus);
    if (!*ui_entry_text(f->find_entry)) {
        ui_set_color(cr, k->disabled_text);
        ui_text_draw(cr, UI_FONT_REGULAR, er.x + 5, er.y + (er.h + ui_text_ascent()) / 2 - 1,
                     "Type to show only the objects whose names contain it", er.w - 10);
    }
    ui_rect x = find_x_rect(f);
    ui_fill(cr, x, k->face);
    ui_bevel(cr, x, !f->find_x_down);
    ui_set_color(cr, k->text);
    cairo_set_line_width(cr, 2);
    double cx = x.x + x.w / 2 + f->find_x_down, cy = x.y + x.h / 2 + f->find_x_down;
    cairo_move_to(cr, cx - 4, cy - 4); cairo_line_to(cr, cx + 4, cy + 4);
    cairo_move_to(cr, cx + 4, cy - 4); cairo_line_to(cr, cx - 4, cy + 4);
    cairo_stroke(cr);
}

void find_focus(struct folder *f, bool on)
{
    f->find_focus = on;
    ui_container_set_focused(f->cont, !on);
    ui_text_input_focus(f->win, on ? f->find_entry : NULL, find_entry_rect(f));
    ui_surface_damage(f->win, find_row(f));
}

/* The filter as typed: the window's own listing, read again. */
static void find_apply(struct folder *f)
{
    const char *t = ui_entry_text(f->find_entry);
    if (!strcmp(t, f->filter)) return;
    snprintf(f->filter, sizeof f->filter, "%s", t);
    /* icons: the matches flow from the top left; all again, their places */
    layout_reset(f->lay, cols_now(f));
    f->arranged_by_name = false;   /* (arranged by name again once read; then stored places, if not filtered) */
    reload(f);
    damage_status(f);
}

static void find_show(struct folder *f)
{
    if (!f->find_entry) f->find_entry = ui_entry_new("");
    if (!f->find_on) {
        f->find_on = true;
        layout(f);
        ui_surface_damage_all(f->win);
    }
    ui_entry_select_all(f->find_entry);
    find_focus(f, true);
}

static void find_hide(struct folder *f)
{
    if (!f->find_on) return;
    f->find_on = false;
    ui_entry_set_text(f->find_entry, "");
    find_focus(f, false);
    find_apply(f);
    layout(f);
    ui_surface_damage_all(f->win);
}

static bool find_key(struct folder *f, const ui_key_event *e)
{
    if (e->sym == XKB_KEY_Escape) { find_hide(f); return true; }
    if (e->sym == XKB_KEY_Return || e->sym == XKB_KEY_KP_Enter || e->sym == XKB_KEY_Down || e->sym == XKB_KEY_Tab) {
        find_focus(f, false);   /* to the objects (the filter stays) */
        return true;
    }
    ui_entry_result r = ui_entry_key(f->find_entry, e);
    if (r == UI_ENTRY_CHANGED) find_apply(f);   /* as typed */
    if (r != UI_ENTRY_NONE) ui_surface_damage(f->win, find_row(f));
    return true;
}

/* A press in the row: the entry (focused), x (all again); elsewhere the
 * entry lets go of the keyboard. */
static bool find_pointer(struct folder *f, const ui_pointer_event *e)
{
    ui_rect er = find_entry_rect(f), x = find_x_rect(f), row = find_row(f);
    if (e->kind == UI_RELEASE && f->find_x_down) {
        f->find_x_down = false;
        ui_surface_damage(f->win, row);
        if (ui_rect_has(x, e->x, e->y)) find_hide(f);
        return true;
    }
    if (f->find_focus && (e->kind == UI_MOTION || e->kind == UI_RELEASE)) {
        if (ui_entry_pointer(f->find_entry, e, er) != UI_ENTRY_NONE) ui_surface_damage(f->win, row);
    }
    if (e->kind != UI_PRESS) return false;
    if (ui_rect_has(x, e->x, e->y)) { f->find_x_down = true; ui_surface_damage(f->win, row); return true; }
    if (ui_rect_has(er, e->x, e->y)) {
        if (!f->find_focus) find_focus(f, true);
        ui_entry_pointer(f->find_entry, e, er);
        ui_surface_damage(f->win, row);
        return true;
    }
    if (ui_rect_has(row, e->x, e->y)) return true;
    if (f->find_focus) find_focus(f, false);
    return false;
}

void win_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    struct folder *f = ui_surface_data(s);
    {
        static bool first_empty, first_items;
        if (!first_empty) { first_empty = true; TIMING(f, "first frame (%zu objects)", f->m.total); }
        if (!first_items && f->m.total) { first_items = true; TIMING(f, "first frame with objects"); }
    }
    const ui_scheme *k = &ui_scheme_now;
    char t[400];
    snprintf(t, sizeof t, "%s - %s", f->title, view_name(ui_container_view(f->cont)));
    ui_frame_draw(s, cr, clip, t);
    ui_container_draw(f->cont, cr, clip);
    if (f->desktop) return;   /* no status line */
    if (f->find_on) find_draw(f, cr);
    ui_rect c = ui_frame_client(s);
    ui_rect st = { c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    if (!ui_rect_meets(clip, st)) return;
    ui_fill(cr, st, k->face);
    ui_bevel(cr, (ui_rect){ st.x + 1, st.y + 1, st.w - 2, st.h - 2 }, false);
    char msg[512];
    size_t nsel = ui_container_selected_count(f->cont);
    if (f->error[0]) snprintf(msg, sizeof msg, "%s", f->error);
    else if (is_tree(f)) snprintf(msg, sizeof msg, "%zu folders shown", f->ntvis);
    else if (f->filter[0]) snprintf(msg, sizeof msg, "%zu objects match \"%s\"%s   (Esc: all again)", f->m.total, f->filter,
                                    f->m.complete ? "" : " (reading...)");
    else if (nsel) snprintf(msg, sizeof msg, "%zu objects, %zu selected%s", f->m.total, nsel, f->m.complete ? "" : " (reading...)");
    else snprintf(msg, sizeof msg, "%zu objects%s", f->m.total, f->m.complete ? "" : " (reading...)");
    ui_set_color(cr, k->text);
    cairo_save(cr);   /* inside its bevel, whatever the font */
    cairo_rectangle(cr, st.x + 2, st.y + 2, st.w - 4, st.h - 4);
    cairo_clip(cr);
    ui_text_draw(cr, UI_FONT_REGULAR, st.x + 6, st.y + (STATUS_H + ui_text_ascent()) / 2 - 1, msg, st.w - 12);
    cairo_restore(cr);
}

void layout(struct folder *f)
{
    ui_rect c = ui_frame_client(f->win);
    if (!f->desktop) c.h -= STATUS_H;
    if (f->find_on) { c.y += FIND_H; c.h -= FIND_H; }
    ui_container_layout(f->cont, c);
}

void win_resized(ui_surface *s, double w, double h)
{
    (void)w; (void)h;
    struct folder *f = ui_surface_data(s);
    if (f->desktop) wallpaper_set_size(w, h, ui_surface_scale(s));
    layout(f);
    ui_surface_damage_all(s);
}

void win_pointer(ui_surface *s, const ui_pointer_event *e)
{
    struct folder *f = ui_surface_data(s);
    /* The window-list button (button 3 by default, as on OS/2) on the desktop. */
    if (f->desktop && e->kind == UI_PRESS && ui_mouse_is(UI_ACT_WINDOW_LIST, e)) { winlist_open(false); return; }
    size_t before = ui_container_selected_count(f->cont);
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    bool used = false;
    if (e->kind != UI_PRESS || h != UI_HIT_CLIENT) used = ui_frame_pointer(s, e);
    if (!used && f->find_on && find_pointer(f, e)) return;
    if (!used || e->kind == UI_RELEASE || e->kind == UI_MOTION) ui_container_pointer(f->cont, e);
    if (ui_container_selected_count(f->cont) != before) damage_status(f);
}

void set_view(struct folder *f, ui_view v)
{
    if (v == UI_VIEW_TREE) tree_start(f);
    ui_container_set_view(f->cont, v);
    if (v == UI_VIEW_TREE) tree_rebuild(f, f->troot);
    update_title(f);
    damage_status(f);
}

void win_key(ui_surface *s, const ui_key_event *e)
{
    struct folder *f = ui_surface_data(s);
    size_t before = ui_container_selected_count(f->cont);
    if ((e->mods & UI_MOD_CTRL) && (e->sym == XKB_KEY_w || e->sym == XKB_KEY_W)) { win_close(s); return; }
    /* Find in this folder and its subfolders (G4): Ctrl+Shift+F; Ctrl+F:
     * the search field, this folder only (v0.3-design 6) */
    if ((e->mods & UI_MOD_CTRL) && (e->sym == XKB_KEY_f || e->sym == XKB_KEY_F)) {
        if ((e->mods & UI_MOD_SHIFT) || f->desktop || is_tree(f)) findwin_open(f->path, NULL);
        else if (f->find_on) find_hide(f);   /* (again: the row goes, as Escape) */
        else find_show(f);
        return;
    }
    if (f->find_on && f->find_focus && find_key(f, e)) return;
    /* Escape with the row up but the keyboard on the objects (a click came
     * back into the window): the row goes too, unless a title is edited */
    if (f->find_on && e->sym == XKB_KEY_Escape && !ui_container_renaming(f->cont)) { find_hide(f); return; }
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_1) { set_view(f, UI_VIEW_ICON); return; }
    /* In the order the View menu lists them (OS/2's): icon, tree, details. */
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_2) { set_view(f, UI_VIEW_TREE); return; }
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_3) { set_view(f, UI_VIEW_DETAILS); return; }
    if (e->sym == XKB_KEY_Delete && !ui_container_renaming(f->cont)) {
        /* the selection, or the item with the cursor (Shift: for good) */
        size_t total = is_tree(f) ? f->ntvis : f->m.total, at = SIZE_MAX;
        for (size_t k = 0; k < total && at == SIZE_MAX; k++)
            if (ui_container_is_selected(f->cont, k)) at = k;
        if (at == SIZE_MAX) at = ui_container_cursor(f->cont);
        if (at != SIZE_MAX && at < total) delete_request(f, at, (e->mods & UI_MOD_SHIFT) != 0 || is_trash_dir(f->path));
        return;
    }
    ui_container_key(f->cont, e);
    if (ui_container_selected_count(f->cont) != before) damage_status(f);
}

void win_focus(ui_surface *s, bool on)
{
    (void)on;
    ui_frame_damage(s);
}

void free_folder(struct folder *f)
{
    for (struct folder **pp = &folders; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    if (f->sub) svc_unsubscribe(f->sub);
    if (f->bg_sub) svc_unsubscribe(f->bg_sub);
    if (f->bg_img) cairo_surface_destroy(f->bg_img);
    if (f->find_entry) ui_entry_free(f->find_entry);
    changes_clear(f);
    free(f->changes);
    tree_free(f);
    layout_free(f->lay);
    for (size_t j = 0; j < f->nspot_new; j++) free(f->spot_new[j]);
    free(f->spot_new);
    model_free(&f->m);
    if (f->sh) { model_free(f->sh); free(f->sh); }
    ui_container_free(f->cont);
    ui_surface_destroy(f->win);
    free(f);
}

/* Closing happens on the next loop turn: the request comes from inside
 * this window's own event handling (its close button), which must not
 * find the window freed under it. */
static void close_later(void *arg)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    raise_in(f, "window.close", NULL, "folder");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", f->path);
    free_folder(f);
    notify_in_use(path);   /* its icon elsewhere is no longer open */
}

void win_close(ui_surface *s)
{
    struct folder *f = ui_surface_data(s);
    if (f->closing || f->desktop) return;   /* the desktop stays */
    f->closing = true;
    if (f->bench_secs > 0) report(f);
    ui_app_post(app, close_later, as_arg(f));
}

/* Benchmark: a fling, then jumps as if the scroll bar thumb were dragged. */
/* WPS_SHELL_SELFTEST=menus: open, re-open and toggle menus in quick
 * succession (the orders that broke Wayland's popup rules), then quit. */
void selftest_chosen(void *arg, int64_t id) { (void)arg; (void)id; }
