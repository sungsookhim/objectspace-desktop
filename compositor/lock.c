/*
 * The lock screen (G5 step 4; OS/2's Lockup): the LaunchPad's Lockup
 * button (the desktop's "lock" command) or Super+L.
 *
 * The compositor covers every screen and takes all input until the user's
 * password is typed. The password is checked by PAM (service "wps-lock",
 * installed by tools/install-session.sh, or "system-auth"), in a child
 * process, so the compositor never waits; a wrong password is said so,
 * and the next try waits a second. Built without PAM (pam-devel), the
 * screen cannot be locked: Lockup says so instead.
 *
 * Tests (headless only): OSD_COMP_TEST_PASSWORD is the password, no PAM.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <xkbcommon/xkbcommon.h>
#ifdef HAVE_PAM
#include <security/pam_appl.h>
#endif

#include "comp.h"

static struct {
    bool locked, checking;
    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *shade;
    struct wlr_scene_buffer *panel;
    char pw[256];
    size_t len;
    char msg[160];
    pid_t child;
    int fd;
    struct wl_event_source *src;
} lk = { .fd = -1 };

bool lock_active(void) { return lk.locked; }

/* ------------------------------------------------------------- drawing */

struct pbuf { struct wlr_buffer base; cairo_surface_t *s; };
static void pb_destroy(struct wlr_buffer *b) { struct pbuf *p = wl_container_of(b, p, base); cairo_surface_destroy(p->s); free(p); }
static bool pb_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
    struct pbuf *p = wl_container_of(b, p, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
    *data = cairo_image_surface_get_data(p->s);
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)cairo_image_surface_get_stride(p->s);
    return true;
}
static void pb_end(struct wlr_buffer *b) { (void)b; }
static const struct wlr_buffer_impl pb_impl = { .destroy = pb_destroy, .begin_data_ptr_access = pb_begin, .end_data_ptr_access = pb_end };

#define PW 420
#define PH 190

static void draw(void)
{
    const ui_scheme *k = &ui_scheme_now;
    double sc = comp_scale_at(lk.panel->node.x + PW / 2.0, lk.panel->node.y + PH / 2.0);   /* sharp on a scaled screen */
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(PW * sc), (int)ceil(PH * sc));
    cairo_t *cr = cairo_create(s);
    cairo_scale(cr, sc, sc);
    ui_fill(cr, (ui_rect){ 0, 0, PW, PH }, k->face);
    ui_bevel(cr, (ui_rect){ 0, 0, PW, PH }, true);
    ui_rect t = { 4, 4, PW - 8, ui_scheme_now.title_h };
    ui_fill(cr, t, k->title_active);
    ui_set_color(cr, k->title_text);
    const char *title = "Lockup";
    double tw = ui_text_width(UI_FONT_BOLD, title);
    ui_text_draw(cr, UI_FONT_BOLD, t.x + (t.w - tw) / 2, t.y + (t.h + ui_text_ascent()) / 2 - 1, title, t.w);
    ui_set_color(cr, k->text);
    struct passwd *pw = getpwuid(getuid());
    char line[200];
    snprintf(line, sizeof line, "This computer is locked by %s.", pw ? pw->pw_name : "its user");
    ui_text_draw(cr, UI_FONT_REGULAR, 20, 58, line, PW - 40);
    ui_text_draw(cr, UI_FONT_REGULAR, 20, 82, "Type the password to unlock it, then Enter.", PW - 40);
    ui_rect f = { 20, 100, PW - 40, 26 };
    ui_fill(cr, f, k->client_bg);
    ui_bevel(cr, f, false);
    char dots[3 * 64 + 1] = "";
    size_t n = lk.len > 60 ? 60 : lk.len;
    for (size_t i = 0; i < n; i++) strcat(dots, "\xe2\x97\x8f");   /* ● for each character typed */
    ui_set_color(cr, k->text);
    double w = ui_text_draw(cr, UI_FONT_REGULAR, f.x + 6, f.y + (f.h + ui_text_ascent()) / 2 - 1, dots, f.w - 12);
    if (!lk.checking) ui_fill(cr, (ui_rect){ f.x + 7 + w, f.y + 4, 1, f.h - 8 }, k->cursor);
    ui_set_color(cr, lk.msg[0] == '!' ? k->urgent_text : k->text);
    ui_text_draw(cr, UI_FONT_REGULAR, 20, 152, lk.checking ? "Checking..." : lk.msg[0] == '!' ? lk.msg + 1 : lk.msg, PW - 40);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    struct pbuf *p = calloc(1, sizeof *p);
    wlr_buffer_init(&p->base, &pb_impl, cairo_image_surface_get_width(s), cairo_image_surface_get_height(s));
    p->s = s;
    wlr_scene_buffer_set_buffer(lk.panel, &p->base);
    wlr_scene_buffer_set_dest_size(lk.panel, PW, PH);
    wlr_buffer_drop(&p->base);
}

/* The shade over every screen, the panel in the middle of the first. */
void lock_layout(void)
{
    if (!lk.tree) return;
    struct wlr_box all, first = { 0, 0, 1280, 800 };
    wlr_output_layout_get_box(server.output_layout, NULL, &all);
    struct wlr_output *o = wlr_output_layout_output_at(server.output_layout, all.x + 1, all.y + 1);
    if (o) wlr_output_layout_get_box(server.output_layout, o, &first);
    wlr_scene_node_set_position(&lk.shade->node, all.x, all.y);
    wlr_scene_rect_set_size(lk.shade, all.width, all.height);
    wlr_scene_node_set_position(&lk.panel->node, first.x + (first.width - PW) / 2, first.y + (first.height - PH) / 2);
}

/* --------------------------------------------------------- the password */

#ifdef HAVE_PAM
static const char *pam_pw;
static int conv(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    (void)data;
    struct pam_response *r = calloc((size_t)n, sizeof *r);
    for (int i = 0; i < n; i++)
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) r[i].resp = strdup(pam_pw);
    *resp = r;
    return PAM_SUCCESS;
}

static bool pam_check(const char *password)
{
    struct passwd *pw = getpwuid(getuid());
    if (!pw) return false;
    pam_pw = password;
    struct pam_conv c = { conv, NULL };
    pam_handle_t *h;
    const char *service = access("/etc/pam.d/wps-lock", R_OK) == 0 ? "wps-lock" : "system-auth";
    if (pam_start(service, pw->pw_name, &c, &h) != PAM_SUCCESS) return false;
    int r = pam_authenticate(h, 0);
    if (r == PAM_SUCCESS) pam_setcred(h, PAM_REFRESH_CRED);
    pam_end(h, r);
    return r == PAM_SUCCESS;
}
#endif

static void unlock(void)
{
    lk.locked = false;
    explicit_bzero(lk.pw, sizeof lk.pw);
    lk.len = 0;
    wlr_scene_node_set_enabled(&lk.tree->node, false);
    fprintf(stderr, "wps-compositor: unlocked\n");
    comp_after_unlock();
}

#ifdef HAVE_PAM
static int on_result(int fd, uint32_t mask, void *data)
{
    (void)mask; (void)data;
    char r = 0;
    ssize_t n = read(fd, &r, 1);
    wl_event_source_remove(lk.src);
    lk.src = NULL;
    close(fd);
    lk.fd = -1;
    if (lk.child > 0) waitpid(lk.child, NULL, 0);
    lk.child = 0;
    lk.checking = false;
    if (n == 1 && r == 'y') { unlock(); return 0; }
    explicit_bzero(lk.pw, sizeof lk.pw);
    lk.len = 0;
    snprintf(lk.msg, sizeof lk.msg, "!That is not the password.");
    draw();
    return 0;
}
#endif

static void check(void)
{
    if (lk.checking) return;
    lk.pw[lk.len] = 0;
    const char *test = getenv("OSD_COMP_TEST_PASSWORD");
    if (test && wlr_backend_is_headless(server.backend)) {   /* tests only */
        if (!strcmp(test, lk.pw)) { unlock(); return; }
        lk.len = 0;
        snprintf(lk.msg, sizeof lk.msg, "!That is not the password.");
        draw();
        return;
    }
#ifdef HAVE_PAM
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        char r = pam_check(lk.pw) ? 'y' : 'n';
        if (r == 'n') sleep(1);   /* (a second before trying again) */
        if (write(p[1], &r, 1) < 0) _exit(1);
        _exit(0);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return; }
    lk.child = pid;
    lk.fd = p[0];
    lk.checking = true;
    lk.src = wl_event_loop_add_fd(server.loop, lk.fd, WL_EVENT_READABLE, on_result, NULL);
    draw();
#endif
}

/* All keys while locked. */
void lock_key(xkb_keysym_t sym, const char *utf8)
{
    if (!lk.locked || lk.checking) return;
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { if (lk.len) check(); return; }
    if (sym == XKB_KEY_BackSpace) { if (lk.len) { do lk.len--; while (lk.len && ((unsigned char)lk.pw[lk.len] & 0xc0) == 0x80); } }
    else if (sym == XKB_KEY_Escape) lk.len = 0;
    else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f) {
        size_t n = strlen(utf8);
        if (lk.len + n < sizeof lk.pw - 1) { memcpy(lk.pw + lk.len, utf8, n); lk.len += n; }
    } else return;
    lk.msg[0] = 0;
    draw();
}

bool lock_screen(void)
{
#ifndef HAVE_PAM
    if (!(getenv("OSD_COMP_TEST_PASSWORD") && wlr_backend_is_headless(server.backend))) {
        fprintf(stderr, "wps-compositor: lock: built without PAM (sudo dnf install pam-devel, then make compositor)\n");
        return false;
    }
#endif
    if (lk.locked) return true;
    if (!lk.tree) {
        lk.tree = wlr_scene_tree_create(&server.scene->tree);
        static const float shade[4] = { 0.02f, 0.05f, 0.12f, 1.0f };
        lk.shade = wlr_scene_rect_create(lk.tree, 1, 1, shade);
        lk.panel = wlr_scene_buffer_create(lk.tree, NULL);
    }
    lk.locked = true;
    lk.len = 0;
    lk.msg[0] = 0;
    wlr_scene_node_raise_to_top(&lk.tree->node);
    wlr_scene_node_set_enabled(&lk.tree->node, true);
    lock_layout();
    draw();
    comp_before_lock();
    fprintf(stderr, "wps-compositor: locked\n");
    return true;
}
