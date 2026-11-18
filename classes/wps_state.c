#include "wps_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WPObject.h"

void (*wps_on_dirty)(wpo_object *o);

void wps_state_clear_touched(wps_cstate *c)
{
    for (size_t i = 0; i < c->ntouched; i++) free(c->touched[i]);
    c->ntouched = 0;
}

void wps_state_free(wps_state *st)
{
    if (!st) return;
    for (size_t i = 0; i < st->n; i++) {
        wps_props_free(&st->c[i].props);
        wps_state_clear_touched(&st->c[i]);
        free(st->c[i].touched);
    }
    free(st->c);
    free(st);
}

wps_cstate *wps_state_class(wps_state **stp, const char *cls, bool create)
{
    wps_state *st = *stp;
    for (size_t i = 0; st && i < st->n; i++)
        if (strcmp(st->c[i].cls, cls) == 0) return &st->c[i];
    if (!create) return NULL;
    if (!st && !(st = *stp = calloc(1, sizeof *st))) return NULL;
    if (st->n == st->cap) {
        size_t ncap = st->cap ? st->cap * 2 : 4;
        wps_cstate *n = realloc(st->c, ncap * sizeof *n);
        if (!n) return NULL;
        st->c = n;
        st->cap = ncap;
    }
    wps_cstate *c = &st->c[st->n++];
    memset(c, 0, sizeof *c);
    snprintf(c->cls, sizeof c->cls, "%s", cls);
    wps_props_init(&c->props);
    return c;
}

static void touch(wps_cstate *c, const char *key)
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

static wps_cstate *bag(wpo_object *o, const char *cls, bool create)
{
    return wps_state_class(&WPObject_data(o)->state, cls, create);
}

void wps_state_put_str(wpo_object *o, const char *cls, const char *key, const char *val)
{
    wps_cstate *c = bag(o, cls, true);
    if (!c) return;
    char *old = NULL;
    bool same = wps_props_str(&c->props, key, &old) && strcmp(old, val) == 0;
    free(old);
    if (same) return;
    wps_props_set_str(&c->props, key, val);
    touch(c, key);
}

void wps_state_put_int(wpo_object *o, const char *cls, const char *key, int64_t v)
{
    wps_cstate *c = bag(o, cls, true);
    int64_t old;
    if (!c || (wps_props_int(&c->props, key, &old) && old == v)) return;
    wps_props_set_int(&c->props, key, v);
    touch(c, key);
}

void wps_state_remove(wpo_object *o, const char *cls, const char *key)
{
    wps_cstate *c = bag(o, cls, false);
    if (c && wps_props_remove(&c->props, key)) touch(c, key);
}

bool wps_state_get_str(wpo_object *o, const char *cls, const char *key, char **out)
{
    wps_cstate *c = bag(o, cls, false);
    return c && wps_props_str(&c->props, key, out);
}

bool wps_state_get_int(wpo_object *o, const char *cls, const char *key, int64_t *out)
{
    wps_cstate *c = bag(o, cls, false);
    return c && wps_props_int(&c->props, key, out);
}

void wps_state_load(wpo_object *o, const char *cls, const wps_props *loaded)
{
    wps_cstate *c = bag(o, cls, true);
    if (!c) return;
    wps_props_free(&c->props);
    wps_props_init(&c->props);
    c->props.counter = loaded->counter;
    for (size_t i = 0; i < loaded->n; i++)
        wps_props_set(&c->props, loaded->p[i].key, loaded->p[i].type, loaded->p[i].val, loaded->p[i].len);
}
