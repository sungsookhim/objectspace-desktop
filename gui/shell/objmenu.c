/* Object menus (objmenu.h), from the WarpCenter's Desktop menu (0.2). */
#define _GNU_SOURCE
#include "objmenu.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cbor.h"
#include "icons.h"
#include "svc.h"

#define MAX_NODES 800

struct mnode {
    char title[256], addr[PATH_MAX];
    bool folder, shadow, link;
    int kind;                  /* a ui_kind, for its icon */
    char icon[PATH_MAX];       /* its own icon (a PNG), or "" */
    struct mnode *kids;
    size_t nkids;
};

struct objmenu {
    struct mnode root;
    int pending, depth;
    bool freed;                /* objmenu_free while being read */
    void (*ready)(objmenu *m, void *ctx);
    void *ctx;
    struct mnode **flat;       /* by menu ID - OBJMENU_FIRST */
    size_t nflat;
};

struct fetch { objmenu *m; struct mnode *node; int depth; char folder[PATH_MAX]; };

static void free_tree(struct mnode *n)
{
    for (size_t i = 0; i < n->nkids; i++) free_tree(&n->kids[i]);
    free(n->kids);
    n->kids = NULL;
    n->nkids = 0;
}

static size_t count_nodes(const struct mnode *n)
{
    size_t c = 1;
    for (size_t i = 0; i < n->nkids; i++) c += count_nodes(&n->kids[i]);
    return c;
}

static int by_title(const void *a, const void *b)
{
    const struct mnode *x = a, *y = b;
    if (x->folder != y->folder) return x->folder ? -1 : 1;   /* folders first, as OS/2's menus */
    return strcasecmp(x->title, y->title);
}

static ui_kind kind_for(const char *type, const char *name)
{
    if (type && !strcmp(type, "folder")) return UI_KIND_FOLDER;
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (type && !strcmp(type, "program")) return UI_KIND_PROGRAM;
    if (dot && (!strcmp(dot, ".desktop") || !strcmp(dot, ".sh"))) return UI_KIND_PROGRAM;
    return UI_KIND_FILE;
}

static void really_free(objmenu *m)
{
    free_tree(&m->root);
    free(m->flat);
    free(m);
}

static void read_folder(objmenu *m, struct mnode *node, const char *folder, int depth);

static void got_folder(void *arg, const cv *reply)
{
    struct fetch *f = arg;
    objmenu *m = f->m;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    struct mnode *node = f->node;
    size_t total = m->freed ? MAX_NODES : count_nodes(&m->root);
    for (size_t i = 0; items && items->k == CV_ARRAY && i < items->n && total < MAX_NODES; i++, total++) {
        const cv *it = &items->items[i];
        const char *nm = cv_text(it, "name"), *ti = cv_text(it, "title"), *ty = cv_text(it, "type");
        const char *tt = cv_text(it, "target"), *tp = cv_text(it, "target_path");
        if (!nm || nm[0] == '.') continue;
        const cv *sh = cv_get(it, "shadow");
        bool shadow = sh && sh->k == CV_BOOL && sh->b, link = ty && !strcmp(ty, "link");
        node->kids = realloc(node->kids, (node->nkids + 1) * sizeof *node->kids);
        struct mnode *k = &node->kids[node->nkids++];
        memset(k, 0, sizeof *k);
        if (nm[0] == '\x01') snprintf(k->addr, sizeof k->addr, "%s", nm + 1);   /* a central object */
        else snprintf(k->addr, sizeof k->addr, "%s/%s", strcmp(f->folder, "/") ? f->folder : "", nm);
        k->folder = ty && !strcmp(ty, "folder");
        k->shadow = shadow;
        k->link = link;
        k->kind = kind_for(ty, nm);
        const char *mk = cv_text(it, "makes"), *ic = cv_text(it, "icon");
        if (mk) k->kind = !strcmp(mk, "WPFolder") ? UI_KIND_FOLDER : !strcmp(mk, "WPProgram") ? UI_KIND_PROGRAM : UI_KIND_FILE;
        if (ic) snprintf(k->icon, sizeof k->icon, "%s", ic);
        if (shadow || link) k->kind = tt && !strcmp(tt, "folder") ? UI_KIND_FOLDER : kind_for("file", tp && *tp ? tp : nm);
        if ((shadow || link) && tt && !strcmp(tt, "folder") && tp && *tp) {   /* opens as the folder it shows */
            k->folder = true;
            snprintf(k->addr, sizeof k->addr, "%s", tp);
        }
        const char *t = ti && *ti ? ti : nm[0] == '\x01' ? "" : nm;
        if (!*t && tp) t = strrchr(tp, '/') ? strrchr(tp, '/') + 1 : tp;
        snprintf(k->title, sizeof k->title, "%s", *t ? t : "?");
    }
    if (node->nkids) qsort(node->kids, node->nkids, sizeof *node->kids, by_title);
    /* (kids are final now: their addresses stay put) */
    for (size_t i = 0; !m->freed && i < node->nkids && f->depth + 1 < m->depth; i++)
        if (node->kids[i].folder) read_folder(m, &node->kids[i], node->kids[i].addr, f->depth + 1);
    free(f);
    if (--m->pending == 0) {
        if (m->freed) really_free(m);
        else if (m->ready) m->ready(m, m->ctx);
    }
}

static void read_folder(objmenu *m, struct mnode *node, const char *folder, int depth)
{
    struct fetch *f = calloc(1, sizeof *f);
    f->m = m;
    f->node = node;
    f->depth = depth;
    snprintf(f->folder, sizeof f->folder, "%s", folder);
    m->pending++;
    svc_list(folder, 0, 300, true, got_folder, f);
}

objmenu *objmenu_read(const char *folder, int depth, void (*ready)(objmenu *m, void *ctx), void *ctx)
{
    objmenu *m = calloc(1, sizeof *m);
    m->depth = depth < 1 ? 1 : depth;
    m->ready = ready;
    m->ctx = ctx;
    snprintf(m->root.addr, sizeof m->root.addr, "%s", folder);
    m->root.folder = true;
    read_folder(m, &m->root, folder, 0);
    return m;
}

void objmenu_free(objmenu *m)
{
    if (!m) return;
    if (m->pending) { m->freed = true; return; }
    really_free(m);
}

static ui_menu_item node_item(objmenu *m, struct mnode *c, const char *label)
{
    m->flat = realloc(m->flat, (m->nflat + 1) * sizeof *m->flat);
    m->flat[m->nflat] = c;
    return (ui_menu_item){ .id = OBJMENU_FIRST + (int64_t)m->nflat++, .label = strdup(label), .enabled = true };
}

static ui_menu_item *to_items(objmenu *m, struct mnode *n, size_t *count, bool open_first, size_t extra)
{
    size_t cap = n->nkids + 2 + extra, k = 0;
    ui_menu_item *it = calloc(cap, sizeof *it);
    if (open_first) {   /* a folder's cascade: the folder itself first */
        it[k++] = node_item(m, n, "Open");
        if (n->nkids) it[k++] = (ui_menu_item){ -1, strdup("-"), false, false, NULL, 0 };
    }
    for (size_t i = 0; i < n->nkids; i++) {
        struct mnode *c = &n->kids[i];
        if (c->folder && c->nkids) {
            size_t sub = 0;
            ui_menu_item *si = to_items(m, c, &sub, true, 0);
            it[k] = node_item(m, c, c->title);
            it[k].items = si;
            it[k++].nitems = sub;
        } else {
            it[k++] = node_item(m, c, c->title);
        }
    }
    *count = k;
    return it;
}

ui_menu_item *objmenu_items(objmenu *m, size_t *n, bool open_first, size_t extra)
{
    free(m->flat);
    m->flat = NULL;
    m->nflat = 0;
    return to_items(m, &m->root, n, open_first, extra);
}

bool objmenu_icon(void *arg, int64_t id, void *item)
{
    objmenu *m = arg;
    if (!m || id < OBJMENU_FIRST || (size_t)(id - OBJMENU_FIRST) >= m->nflat) return false;
    const struct mnode *c = m->flat[id - OBJMENU_FIRST];
    ui_item *it = item;
    it->kind = c->folder ? UI_KIND_FOLDER : (ui_kind)c->kind;
    it->shadow = c->shadow;
    it->link = c->link;
    it->image = icons_item(NULL, c->icon, it->kind, c->addr, ui_icons()->small * 2);   /* (sharp at a scale of 2) */   /* (its own, or the desktop's theme's) */
    return true;
}

bool objmenu_choice(objmenu *m, int64_t id, char *addr, size_t n, bool *folder, char *title, size_t tn)
{
    if (!m || id < OBJMENU_FIRST || (size_t)(id - OBJMENU_FIRST) >= m->nflat) return false;
    const struct mnode *c = m->flat[id - OBJMENU_FIRST];
    snprintf(addr, n, "%s", c->addr);
    if (folder) *folder = c->folder && c->addr[0] == '/';
    if (title) snprintf(title, tn, "%s", c->title);
    return true;
}

size_t objmenu_count(objmenu *m) { return m ? m->root.nkids : 0; }

size_t objmenu_labels(objmenu *m, char *out, size_t n)
{
    size_t o = 0, c = 0;
    out[0] = 0;
    for (size_t i = 0; m && i < m->root.nkids; i++, c++)
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s%s", i ? ", " : "", m->root.kids[i].title,
                              m->root.kids[i].folder && m->root.kids[i].nkids ? " >" : "");
    return c;
}
