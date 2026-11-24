/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "ui_damage.h"

#include <string.h>

static ui_irect clip(ui_irect r, ui_irect to)
{
    int x0 = r.x > to.x ? r.x : to.x, y0 = r.y > to.y ? r.y : to.y;
    int x1 = r.x + r.w < to.x + to.w ? r.x + r.w : to.x + to.w;
    int y1 = r.y + r.h < to.y + to.h ? r.y + r.h : to.y + to.h;
    ui_irect o = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
    return o;
}

static bool empty(ui_irect r) { return r.w <= 0 || r.h <= 0; }

static bool inside(ui_irect r, ui_irect a)
{
    return r.x >= a.x && r.y >= a.y && r.x + r.w <= a.x + a.w && r.y + r.h <= a.y + a.h;
}

static bool same(ui_irect a, ui_irect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

static ui_irect bound(ui_irect a, ui_irect b)
{
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    ui_irect o = { x0, y0, x1 - x0, y1 - y0 };
    return o;
}

/* Add a rectangle to a list, merging with an overlapping one, or into the
 * bounding box when the list is full. */
static void push(ui_irect *list, int *n, int max, ui_irect r)
{
    if (empty(r)) return;
    for (int i = 0; i < *n; i++)
        if (inside(r, list[i])) return;
    /* Drop rectangles the new one covers. */
    for (int i = 0; i < *n;)
        if (inside(list[i], r)) list[i] = list[--*n];
        else i++;
    if (*n < max) { list[(*n)++] = r; return; }
    list[max - 1] = bound(list[max - 1], r);
}

void ui_damage_add(ui_damage_frame *f, ui_irect r, int sw, int sh)
{
    ui_irect s = { 0, 0, sw, sh };
    r = clip(r, s);
    if (empty(r) || f->full) return;
    if (same(r, s)) { f->full = true; return; }
    push(f->rects, &f->nrects, UI_DAMAGE_RECTS, r);
}

/* Contents of `area` move up by dy: rectangles already damaged in this
 * frame move with them; the strip the move uncovers is damaged. */
void ui_damage_scroll(ui_damage_frame *f, ui_irect area, int dy)
{
    if (f->full || dy == 0 || empty(area)) return;
    if (f->scrolled && !same(f->area, area)) { f->full = true; return; }
    if (dy >= area.h || -dy >= area.h) {   /* nothing survives the move */
        push(f->rects, &f->nrects, UI_DAMAGE_RECTS, area);
        return;
    }
    int n = f->nrects;
    ui_irect moved[UI_DAMAGE_RECTS];
    int nm = 0;
    for (int i = 0; i < n; i++) {
        ui_irect r = f->rects[i];
        ui_irect in = clip(r, area);
        if (empty(in)) { push(moved, &nm, UI_DAMAGE_RECTS, r); continue; }
        if (!inside(r, area)) push(moved, &nm, UI_DAMAGE_RECTS, r);   /* the part outside stays */
        in.y -= dy;
        push(moved, &nm, UI_DAMAGE_RECTS, clip(in, area));
    }
    memcpy(f->rects, moved, (size_t)nm * sizeof *moved);
    f->nrects = nm;
    f->scrolled = true;
    f->area = area;
    f->dy += dy;
    ui_irect strip = area;
    if (dy > 0) { strip.y = area.y + area.h - dy; strip.h = dy; }
    else { strip.h = -dy; }
    push(f->rects, &f->nrects, UI_DAMAGE_RECTS, strip);
}

void ui_damage_make_plan(const ui_damage_frame *fr, int n, int age, int sw, int sh, ui_damage_plan *p)
{
    memset(p, 0, sizeof *p);
    ui_irect s = { 0, 0, sw, sh };
    int max = (int)(sizeof p->rects / sizeof p->rects[0]);
    if (age <= 0 || age > n) goto full;
    /* The frames the buffer has not seen: fr[n-age .. n-1]. */
    const ui_damage_frame *seen = fr + (n - age);
    bool any_scroll = false;
    ui_irect area = { 0, 0, 0, 0 };
    for (int i = 0; i < age; i++) {
        if (seen[i].full) goto full;
        if (seen[i].scrolled) {
            if (any_scroll && !same(area, seen[i].area)) goto full;
            any_scroll = true;
            area = seen[i].area;
        }
    }
    /* Each frame's rectangles move by the scrolls of the frames after it. */
    int net = 0;
    for (int i = age - 1; i >= 0; i--) {
        for (int k = 0; k < seen[i].nrects; k++) {
            ui_irect r = seen[i].rects[k];
            if (net && any_scroll) {
                ui_irect in = clip(r, area);
                if (!empty(in)) {
                    if (!inside(r, area)) push(p->rects, &p->nrects, max, r);
                    in.y -= net;
                    push(p->rects, &p->nrects, max, clip(in, area));
                    continue;
                }
            }
            push(p->rects, &p->nrects, max, r);
        }
        if (seen[i].scrolled) net += seen[i].dy;
    }
    if (any_scroll && net) {
        if (net >= area.h || -net >= area.h) {
            push(p->rects, &p->nrects, max, area);
        } else {
            p->move = true;
            p->area = area;
            p->dy = net;
            ui_irect strip = area;
            if (net > 0) { strip.y = area.y + area.h - net; strip.h = net; }
            else strip.h = -net;
            push(p->rects, &p->nrects, max, strip);
        }
    }
    for (int i = 0; i < p->nrects; i++) p->rects[i] = clip(p->rects[i], s);
    return;
full:
    memset(p, 0, sizeof *p);
    p->full = true;
    p->rects[0] = s;
    p->nrects = 1;
}
