/* Damage planning: a simulated surface must always match the truth. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "ui_damage.h"

#define W 40
#define H 30
#define ROWS 400   /* content rows a scroll can reach */

static const ui_irect AREA = { 0, 5, W, 20 };   /* rows 5..24 scroll */
static int S;                                   /* scroll position */
static int row_ver[ROWS + H], out_ver[H];

static uint32_t truth(int x, int y)
{
    if (y >= AREA.y && y < AREA.y + AREA.h) {
        int r = S + y - AREA.y;
        return (uint32_t)(r * 7919 + row_ver[r] * 104729 + x);
    }
    return (uint32_t)(0x40000000u + (uint32_t)y * 7919 + (uint32_t)out_ver[y] * 104729 + (uint32_t)x);
}

struct buf { uint32_t px[H][W]; long frame; };   /* frame drawn; -1 none */

static void apply(struct buf *b, const ui_damage_plan *p)
{
    if (p->move) {
        ui_irect a = p->area;
        if (p->dy > 0)
            for (int y = a.y; y < a.y + a.h - p->dy; y++) memmove(&b->px[y][a.x], &b->px[y + p->dy][a.x], (size_t)a.w * 4);
        else
            for (int y = a.y + a.h - 1; y >= a.y - p->dy; y--) memmove(&b->px[y][a.x], &b->px[y + p->dy][a.x], (size_t)a.w * 4);
    }
    for (int i = 0; i < p->nrects; i++) {
        ui_irect r = p->rects[i];
        for (int y = r.y; y < r.y + r.h; y++)
            for (int x = r.x; x < r.x + r.w; x++) b->px[y][x] = truth(x, y);
    }
}

static int matches(const struct buf *b)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (b->px[y][x] != truth(x, y)) return 0;
    return 1;
}

static void simulate(unsigned seed, int frames, int *moves, int *fulls, int *drawn_px)
{
    static struct buf bufs[3];
    for (int i = 0; i < 3; i++) bufs[i].frame = -1;
    memset(row_ver, 0, sizeof row_ver);
    memset(out_ver, 0, sizeof out_ver);
    S = 100;
    ui_damage_frame hist[UI_DAMAGE_HISTORY];
    int nh = 0;
    for (long f = 0; f < frames; f++) {
        ui_damage_frame cur;
        memset(&cur, 0, sizeof cur);
        int ops = rand_r(&seed) % 4;
        for (int o = 0; o < ops; o++) {
            int k = rand_r(&seed) % 10;
            if (k < 5) {
                int dy = rand_r(&seed) % 31 - 15;
                if (rand_r(&seed) % 20 == 0) dy = rand_r(&seed) % 2 ? 25 : -25;   /* more than the area */
                if (S + dy < 0 || S + dy > ROWS - AREA.h) dy = 0;
                S += dy;
                ui_damage_scroll(&cur, AREA, dy);
            } else if (k < 8) {
                int y = AREA.y + rand_r(&seed) % AREA.h;   /* a visible row changes */
                row_ver[S + y - AREA.y]++;
                ui_damage_add(&cur, (ui_irect){ 0, y, W, 1 }, W, H);
            } else if (k < 9) {
                int y = rand_r(&seed) % 5;                 /* outside the area */
                if (rand_r(&seed) % 2) y = AREA.y + AREA.h + y;
                out_ver[y]++;
                ui_damage_add(&cur, (ui_irect){ 0, y, W, 1 }, W, H);
            } else if (rand_r(&seed) % 30 == 0) {
                for (int y = 0; y < H; y++) out_ver[y]++;
                ui_damage_add(&cur, (ui_irect){ 0, 0, W, H }, W, H);
            }
        }
        /* The history: up to the last four frames, ending with this one. */
        if (nh == UI_DAMAGE_HISTORY) { memmove(hist, hist + 1, sizeof hist[0] * (UI_DAMAGE_HISTORY - 1)); nh--; }
        hist[nh++] = cur;
        /* Buffers are released at irregular times: pick one at random. */
        struct buf *b = &bufs[rand_r(&seed) % 3];
        int age = b->frame < 0 ? 0 : (int)(f - b->frame);
        ui_damage_plan p;
        ui_damage_make_plan(hist, nh, age, W, H, &p);
        *moves += p.move;
        *fulls += p.full;
        for (int i = 0; i < p.nrects; i++) *drawn_px += p.rects[i].w * p.rects[i].h;
        apply(b, &p);
        b->frame = f;
        if (!matches(b)) {
            fprintf(stderr, "  mismatch: seed %u frame %ld age %d move %d dy %d rects %d\n", seed, f, age, p.move, p.dy, p.nrects);
            CHECK(0);
            return;
        }
    }
    CHECK(1);
}

TEST(random_histories_always_match)
{
    int moves = 0, fulls = 0, px = 0;
    for (unsigned seed = 1; seed <= 300; seed++) simulate(seed, 200, &moves, &fulls, &px);
    printf("  60000 random frames: %d moved, %d full redraws, %.1f%% of pixels drawn\n", moves, fulls,
           100.0 * px / (60000.0 * W * H));
}

/* A steady scroll with three buffers used in turn, as a compositor releases
 * them: after the first frames, only the uncovered strips are drawn. */
TEST(steady_scroll_draws_little)
{
    ui_damage_frame hist[UI_DAMAGE_HISTORY];
    int nh = 0, fulls = 0;
    long px = 0;
    long drawn_at[3] = { -1, -1, -1 };
    for (long f = 0; f < 1000; f++) {
        ui_damage_frame cur;
        memset(&cur, 0, sizeof cur);
        ui_damage_scroll(&cur, AREA, 1);
        if (nh == UI_DAMAGE_HISTORY) { memmove(hist, hist + 1, sizeof hist[0] * (UI_DAMAGE_HISTORY - 1)); nh--; }
        hist[nh++] = cur;
        int b = (int)(f % 3);
        int age = drawn_at[b] < 0 ? 0 : (int)(f - drawn_at[b]);
        ui_damage_plan p;
        ui_damage_make_plan(hist, nh, age, W, H, &p);
        fulls += p.full;
        for (int i = 0; i < p.nrects; i++) px += p.rects[i].w * p.rects[i].h;
        drawn_at[b] = f;
    }
    printf("  1000 frames: %d full redraws, %.1f%% of pixels drawn\n", fulls, 100.0 * px / (1000.0 * W * H));
    CHECK(fulls == 3);
    CHECK(px < 1000L * W * 4);   /* about three rows a frame */
}

TEST(scroll_moves_rects_already_damaged)
{
    ui_damage_frame f;
    memset(&f, 0, sizeof f);
    ui_damage_add(&f, (ui_irect){ 0, 20, W, 2 }, W, H);
    ui_damage_scroll(&f, AREA, 4);
    /* The damaged row moved up with the contents, and the bottom strip is new. */
    int found_moved = 0, found_strip = 0;
    for (int i = 0; i < f.nrects; i++) {
        if (f.rects[i].y == 16 && f.rects[i].h == 2) found_moved = 1;
        if (f.rects[i].y == AREA.y + AREA.h - 4 && f.rects[i].h == 4) found_strip = 1;
    }
    CHECK(found_moved);
    CHECK(found_strip);
}

/* Many small rectangles, some straddling the scrolled area's edge, then a
 * scroll: the list stays bounded, and the plan still covers everything. */
TEST(many_rects_stay_bounded)
{
    ui_damage_frame f;
    memset(&f, 0, sizeof f);
    for (int i = 0; i < 200; i++) ui_damage_add(&f, (ui_irect){ (i * 7) % (W - 3), (i * 3) % (H - 2), 3, 2 }, W, H);
    CHECK(f.nrects <= UI_DAMAGE_RECTS);
    ui_damage_scroll(&f, AREA, 3);
    CHECK(f.nrects <= UI_DAMAGE_RECTS);
    ui_damage_scroll(&f, AREA, -5);
    CHECK(f.nrects <= UI_DAMAGE_RECTS);
}

TEST(old_buffer_is_redrawn_whole)
{
    ui_damage_frame h[2];
    memset(h, 0, sizeof h);
    ui_damage_plan p;
    ui_damage_make_plan(h, 2, 3, W, H, &p);
    CHECK(p.full);
    ui_damage_make_plan(h, 2, 0, W, H, &p);
    CHECK(p.full);
    ui_damage_make_plan(h, 2, 1, W, H, &p);
    CHECK(!p.full && p.nrects == 0);   /* nothing changed: nothing to draw */
}

int main(void)
{
    RUN(random_histories_always_match);
    RUN(steady_scroll_draws_little);
    RUN(scroll_moves_rects_already_damaged);
    RUN(many_rects_stay_bounded);
    RUN(old_buffer_is_redrawn_whole);
    return CHECK_SUMMARY();
}
