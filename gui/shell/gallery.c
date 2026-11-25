/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * The Gallery view (0.6 step 5; v0.6-design 4): one object shown big,
 * the folder's others in a strip of pictures beneath. The strip is the
 * window's own list in the Icon view (pictures in place of icons, as with
 * Show pictures), laid out a row high; the big one is the object with the
 * cursor (or the first selected): its picture, else its icon, with its
 * title, size and last write. Left and Right move along the strip; Enter
 * opens.
 */
#define _GNU_SOURCE
#include "shell.h"

#include <math.h>
#include <time.h>

const char *folder_view_name(struct folder *f)
{
    if (f->gallery) return "Gallery View";
    return view_name(ui_container_view(f->cont));
}

void gallery_set(struct folder *f, bool on)
{
    if (on && is_columns(f)) columns_stop(f);
    if (on && ui_container_view(f->cont) != UI_VIEW_ICON) set_view(f, UI_VIEW_ICON);
    f->gallery = on;
    ui_container_changed(f->cont, 0, SIZE_MAX);   /* (pictures, or icons again) */
    update_title(f);
    layout(f);
    ui_surface_damage_all(f->win);
}

/* The strip: a row of cells (the folder's icon set's) and room for its
 * scroll bar, at the bottom; the rest is the big one's. */
ui_rect gallery_layout(struct folder *f, ui_rect area)
{
    double strip = folder_icons(f)->cell_h + 30;
    if (strip > area.h / 2) strip = floor(area.h / 2);
    f->gal_area = (ui_rect){ area.x, area.y, area.w, area.h - strip };
    return (ui_rect){ area.x, area.y + area.h - strip, area.w, strip };
}

void gallery_damage(struct folder *f)
{
    if (f->gallery) ui_surface_damage(f->win, f->gal_area);
}

/* The one shown: the cursor's, else the first selected, else the first. */
static size_t shown(struct folder *f)
{
    size_t c = ui_container_cursor(f->cont);
    if (c != SIZE_MAX && c < f->m.total) return c;
    for (size_t i = 0; i < f->m.total; i++) if (ui_container_is_selected(f->cont, i)) return i;
    return f->m.total ? 0 : SIZE_MAX;
}

void gallery_draw(struct folder *f, cairo_t *cr, ui_rect clip)
{
    if (!f->gallery || !ui_rect_meets(clip, f->gal_area)) return;
    const ui_scheme *k = &ui_scheme_now;
    ui_rect a = f->gal_area;
    ui_fill(cr, a, k->client_bg);
    ui_fill(cr, (ui_rect){ a.x, a.y + a.h - 1, a.w, 1 }, k->shadow);
    size_t i = shown(f);
    struct row *rw = i != SIZE_MAX ? row_at(f, i) : NULL;
    if (!rw) return;
    double lh = ui_font_line_h(UI_FONT_REGULAR) + 2, text_h = 2 * lh + 16;
    ui_rect pic = { a.x + 12, a.y + 12, a.w - 24, a.h - 24 - text_h };
    if (pic.w < 32 || pic.h < 32) return;
    char path[PATH_MAX];
    if ((rw->shadow || rw->link) && rw->target) snprintf(path, sizeof path, "%s", rw->target);
    else snprintf(path, sizeof path, "%s/%s", strcmp(f->path, "/") ? f->path : "", rw->name);
    double sc = ui_surface_scale(f->win);
    int want = (int)(fmax(pic.w, pic.h) * sc);
    cairo_surface_t *im = rw->kind == UI_KIND_FILE && path[0] == '/' ? thumbs_get(path, rw->mtime, want > 512 ? 512 : want) : NULL;
    if (im) {   /* its picture, as large as it fits (not larger than made) */
        double iw = cairo_image_surface_get_width(im), ih = cairo_image_surface_get_height(im);
        double s = fmin(pic.w / iw, pic.h / ih);
        if (s > 2.5) s = 2.5;
        cairo_save(cr);
        cairo_translate(cr, pic.x + (pic.w - iw * s) / 2, pic.y + (pic.h - ih * s) / 2);
        cairo_scale(cr, s, s);
        cairo_set_source_surface(cr, im, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {   /* none (yet): its icon, large */
        ui_item it = { 0 };
        src_item(f, i, &it);
        double big = fmin(fmin(pic.w, pic.h), 128);
        it.image = icons_item(f->icon_theme, rw->icon, rw->kind, rw->target && rw->target[0] ? rw->target : rw->name, (int)(big * sc + 0.5));
        ui_draw_item_icon(cr, &it, pic.x + (pic.w - big) / 2, pic.y + (pic.h - big) / 2, big);
    }
    const char *title = rw->title && rw->title[0] ? rw->title : rw->name;
    double y = a.y + a.h - text_h + 6;
    ui_set_color(cr, k->text);
    double tw = ui_text_width(UI_FONT_BOLD, title);
    ui_text_draw(cr, UI_FONT_BOLD, a.x + fmax(12, (a.w - tw) / 2), y + ui_font_ascent(UI_FONT_BOLD), title, a.w - 24);
    char line[128], size[48], date[48];
    if (rw->kind == UI_KIND_FOLDER) snprintf(size, sizeof size, "Folder");
    else if (rw->size >= 1024 * 1024) snprintf(size, sizeof size, "%.1f MB", rw->size / 1048576.0);
    else if (rw->size >= 1024) snprintf(size, sizeof size, "%.1f KB", rw->size / 1024.0);
    else snprintf(size, sizeof size, "%lld bytes", (long long)rw->size);
    time_t t = (time_t)rw->mtime;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
    snprintf(line, sizeof line, "%s   %s", size, date);
    ui_set_color(cr, k->shadow);
    tw = ui_text_width(UI_FONT_REGULAR, line);
    ui_text_draw(cr, UI_FONT_REGULAR, a.x + fmax(12, (a.w - tw) / 2), y + lh + ui_text_ascent(), line, a.w - 24);
}
