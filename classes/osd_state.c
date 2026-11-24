#include "osd_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OsdObject.h"

void (*osd_on_dirty)(osdo_object *o);
void (*osd_on_changed)(osdo_object *o);

void osd_state_clear_touched(osd_cstate *c)
{
    for (size_t i = 0; i < c->ntouched; i++) free(c->touched[i]);
    c->ntouched = 0;
}

void osd_state_free(osd_state *st)
{
    if (!st) return;
    for (size_t i = 0; i < st->n; i++) {
        osd_props_free(&st->c[i].props);
        osd_state_clear_touched(&st->c[i]);
        free(st->c[i].touched);
    }
    free(st->c);
    free(st);
}

osd_cstate *osd_state_class(osd_state **stp, const char *cls, bool create)
{
    osd_state *st = *stp;
    for (size_t i = 0; st && i < st->n; i++)
        if (strcmp(st->c[i].cls, cls) == 0) return &st->c[i];
    if (!create) return NULL;
    if (!st && !(st = *stp = calloc(1, sizeof *st))) return NULL;
    if (st->n == st->cap) {
        size_t ncap = st->cap ? st->cap * 2 : 4;
        osd_cstate *n = realloc(st->c, ncap * sizeof *n);
        if (!n) return NULL;
        st->c = n;
        st->cap = ncap;
    }
    osd_cstate *c = &st->c[st->n++];
    memset(c, 0, sizeof *c);
    snprintf(c->cls, sizeof c->cls, "%s", cls);
    osd_props_init(&c->props);
    return c;
}

static void touch(osd_cstate *c, const char *key)
{
    for (size_t i = 0; i < c->ntouched; i++)
        if (strcmp(c->touched[i], key) == 0) return;
    if (c->ntouched == c->captouched) {
        size_t ncap = c->captouched ? c->captouched * 2 : 4;
        char **n = realloc(c->touched, ncap * sizeof *n);
        if (!n) return;
        c->touched = n;
        c->captouched = ncap;
    }
    c->touched[c->ntouched++] = strdup(key);
}

static osd_cstate *bag(osdo_object *o, const char *cls, bool create)
{
    return osd_state_class(&OsdObject_data(o)->state, cls, create);
}

void osd_state_put_str(osdo_object *o, const char *cls, const char *key, const char *val)
{
    osd_cstate *c = bag(o, cls, true);
    if (!c) return;
    char *old = NULL;
    bool same = osd_props_str(&c->props, key, &old) && strcmp(old, val) == 0;
    free(old);
    if (same) return;
    osd_props_set_str(&c->props, key, val);
    touch(c, key);
}

void osd_state_put_int(osdo_object *o, const char *cls, const char *key, int64_t v)
{
    osd_cstate *c = bag(o, cls, true);
    int64_t old;
    if (!c || (osd_props_int(&c->props, key, &old) && old == v)) return;
    osd_props_set_int(&c->props, key, v);
    touch(c, key);
}

void osd_state_remove(osdo_object *o, const char *cls, const char *key)
{
    osd_cstate *c = bag(o, cls, false);
    if (c && osd_props_remove(&c->props, key)) touch(c, key);
}

bool osd_state_get_str(osdo_object *o, const char *cls, const char *key, char **out)
{
    osd_cstate *c = bag(o, cls, false);
    return c && osd_props_str(&c->props, key, out);
}

bool osd_state_get_int(osdo_object *o, const char *cls, const char *key, int64_t *out)
{
    osd_cstate *c = bag(o, cls, false);
    return c && osd_props_int(&c->props, key, out);
}

void osd_state_load(osdo_object *o, const char *cls, const osd_props *loaded)
{
    osd_cstate *c = bag(o, cls, true);
    if (!c) return;
    osd_props_free(&c->props);
    osd_props_init(&c->props);
    c->props.counter = loaded->counter;
    for (size_t i = 0; i < loaded->n; i++)
        osd_props_set(&c->props, loaded->p[i].key, loaded->p[i].type, loaded->p[i].val, loaded->p[i].len);
}
