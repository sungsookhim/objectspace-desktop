/* osd-shell: The tree view. */
#define _GNU_SOURCE
#include "shell.h"

static void tree_load(struct folder *f, struct tnode *n, size_t offset);

static void tnode_free(struct tnode *n)
{
    if (!n) return;
    if (n->sub) svc_unsubscribe(n->sub);
    for (size_t i = 0; i < n->nkids; i++) tnode_free(n->kids[i]);
    for (size_t i = 0; i < n->npending; i++) free(n->pending[i]);
    free(n->pending);
    free(n->kids);
    free(n->name);
    free(n->path);
    free(n);
}

static struct tnode *tnode_new(struct tnode *parent, const char *name, const char *path)
{
    struct tnode *n = calloc(1, sizeof *n);
    n->parent = parent;
    n->name = strdup(name);
    n->path = strdup(path);
    n->depth = parent ? parent->depth + 1 : 0;
    return n;
}

static void tree_flatten(struct folder *f, struct tnode *n)
{
    if (f->ntvis == f->captvis) {
        f->captvis = f->captvis ? f->captvis * 2 : 64;
        f->tvis = realloc(f->tvis, f->captvis * sizeof *f->tvis);
    }
    f->tvis[f->ntvis++] = n;
    if (n->expanded)
        for (size_t i = 0; i < n->nkids; i++) tree_flatten(f, n->kids[i]);
}

/* Rebuild the visible rows, keeping the cursor on the same node. */
void tree_rebuild(struct folder *f, struct tnode *keep)
{
    f->ntvis = 0;
    if (f->troot) tree_flatten(f, f->troot);
    if (ui_container_view(f->cont) != UI_VIEW_TREE) return;
    ui_container_select_none(f->cont);
    ui_container_reset(f->cont);
    for (size_t i = 0; keep && i < f->ntvis; i++)
        if (f->tvis[i] == keep) { ui_container_set_cursor(f->cont, i); ui_container_select(f->cont, i); }
    damage_status(f);
}

static int cmp_names(const void *a, const void *b) { return strcoll(*(char *const *)a, *(char *const *)b); }

static void got_kids(void *arg, const cv *reply)
{
    struct tctx *t = arg;
    struct folder *f = folder_by_id(t->folder);
    struct tnode *n = t->n;
    size_t offset = t->offset;
    bool valid = f && t->tgen == f->tgen;
    free(t);
    if (!valid) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const cv *items = res ? cv_get(res, "items") : NULL, *cmp = res ? cv_get(res, "complete") : NULL;
    size_t cnt = items && items->k == CV_ARRAY ? items->n : 0;
    for (size_t i = 0; i < cnt; i++) {
        const char *ty = cv_text(&items->items[i], "type"), *nm = cv_text(&items->items[i], "name");
        if (!ty || strcmp(ty, "folder") || !nm) continue;
        if (n->npending == n->cappending) {
            n->cappending = n->cappending ? n->cappending * 2 : 16;
            n->pending = realloc(n->pending, n->cappending * sizeof *n->pending);
        }
        n->pending[n->npending++] = strdup(nm);
    }
    if (res && !(cmp && cmp->k == CV_BOOL && cmp->b)) { tree_load(f, n, offset + cnt); return; }
    /* All read: the new subfolders, keeping nodes (and their expansion)
     * that are still there. */
    if (n->npending) qsort(n->pending, n->npending, sizeof *n->pending, cmp_names);
    struct tnode **kids = calloc(n->npending ? n->npending : 1, sizeof *kids);
    for (size_t i = 0; i < n->npending; i++) {
        struct tnode *old = NULL;
        for (size_t k = 0; k < n->nkids && !old; k++)
            if (n->kids[k] && !strcmp(n->kids[k]->name, n->pending[i])) { old = n->kids[k]; n->kids[k] = NULL; }
        if (!old) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", strcmp(n->path, "/") ? n->path : "", n->pending[i]);
            old = tnode_new(n, n->pending[i], p);
        }
        kids[i] = old;
        free(n->pending[i]);
    }
    for (size_t k = 0; k < n->nkids; k++) tnode_free(n->kids[k]);   /* gone */
    free(n->kids);
    n->kids = kids;
    n->nkids = n->npending;
    n->npending = 0;
    n->loaded = true;
    n->loading = false;
    if (!reply) snprintf(f->error, sizeof f->error, "lost the connection to osdd");
    struct tnode *cur = NULL;
    size_t ci = ui_container_cursor(f->cont);
    if (ci != SIZE_MAX && ci < f->ntvis) cur = f->tvis[ci];
    tree_rebuild(f, cur);
}

static void tree_load(struct folder *f, struct tnode *n, size_t offset)
{
    struct tctx *t = malloc(sizeof *t);
    t->folder = f->id;
    t->tgen = f->tgen;
    t->n = n;
    t->offset = offset;
    n->loading = true;
    svc_list_v(n->path, "", f->show_hidden, offset, 4096, false, got_kids, t);
}

static void got_tree_sub(void *arg, const cv *reply)
{
    struct tsub *t = arg;
    struct folder *f = folder_by_id(t->folder);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (f && t->tgen == f->tgen && res && res->k == CV_INT) {
        if (t->n->expanded && !t->n->sub) t->n->sub = res->i;
        else svc_unsubscribe(res->i);   /* collapsed meanwhile */
    }
    free(t);
}

void tree_expand(struct folder *f, struct tnode *n, bool expand)
{
    if (expand == n->expanded) return;
    n->expanded = expand;
    if (expand) {
        if (!n->loaded && !n->loading) tree_load(f, n, 0);
        if (!n->sub && n != f->troot) {   /* the root is the window's own subscription */
            struct tsub *t = malloc(sizeof *t);
            t->folder = f->id;
            t->tgen = f->tgen;
            t->n = n;
            svc_subscribe_folder(n->path, got_tree_sub, t);
        }
    } else if (n->sub) {
        svc_unsubscribe(n->sub);
        n->sub = 0;
    }
    tree_rebuild(f, n);
}

/* A watched folder changed: read its subfolders again. */
bool tree_event(struct folder *f, int64_t sub)
{
    struct tnode *stack[256];
    int sp = 0;
    if (f->troot) stack[sp++] = f->troot;
    while (sp) {
        struct tnode *n = stack[--sp];
        if (n->sub == sub || (n == f->troot && sub == f->sub)) {
            if (n->loaded && !n->loading) { n->npending = 0; tree_load(f, n, 0); }
            if (n != f->troot) return true;
        }
        for (size_t i = 0; i < n->nkids && sp < 256; i++)
            if (n->kids[i]->expanded) stack[sp++] = n->kids[i];
    }
    return false;
}

void tree_start(struct folder *f)
{
    if (f->troot) return;
    f->tgen++;
    const char *base = strrchr(f->path, '/');
    f->troot = tnode_new(NULL, base && base[1] ? base + 1 : f->path, f->path);
    tree_expand(f, f->troot, true);
}

void tree_free(struct folder *f)
{
    tnode_free(f->troot);
    f->troot = NULL;
    free(f->tvis);
    f->tvis = NULL;
    f->ntvis = f->captvis = 0;
    f->tgen++;
}

bool is_tree(struct folder *f) { return ui_container_view(f->cont) == UI_VIEW_TREE; }
