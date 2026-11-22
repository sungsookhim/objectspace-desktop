#!/usr/bin/env python3
"""WPS's isometric icon set (draft, set A: System Setup objects).

Style: isometric pixel art after BeOS / OS/2 Warp 4, 2:1 lines (2 across
for 1 down; verticals straight), light from the upper left (top lightest,
left-facing sides middle, right-facing sides dark). The master is 256 px;
every size is drawn on its own from the same shapes (not scaled down), so
lines stay one pixel apart and the outline keeps a whole-pixel width.

Two variants:
  outline   - a dark outline round each shape, at least 1 px at every
              size; inner edges drawn from 48 px up
  flat      - no outline: faces told apart by their shades alone

Run from the tree's top:
  python3 tools/icons/make_iso.py OUTDIR [--sizes 256,64,48,40,32]
Writes OUTDIR/VARIANT/NAME-SIZE.png and contact sheets (needs Pillow)."""
import argparse
import math
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

INK = (34, 30, 44, 255)

# materials (base colours; faces are shaded from these)
PLASTIC = (204, 202, 194)
PLASTIC_L = (226, 224, 216)
PLASTIC_D = (150, 150, 148)
CABINET = (84, 80, 92)
SCREEN = (52, 98, 188)
SCREEN_L = (120, 160, 230)
TEAL = (0, 128, 128)
PAPER = (246, 242, 226)
PAPER_Y = (236, 222, 176)
WOOD = (200, 150, 88)
METAL = (168, 174, 186)
RED = (210, 58, 48)
YELLOW = (242, 196, 44)
GREEN = (60, 168, 74)
BLUE = (58, 108, 210)
PURPLE = (142, 90, 192)
WHITE = (250, 250, 248)
DARK = (54, 52, 60)
LCD = (28, 44, 34)
LCD_ON = (96, 232, 112)

TOP, LEFT, RIGHT = 1.0, 0.86, 0.66   # face shades


def shade(base, f):
    """A face's colour: lightened toward white above 0.86, darkened below."""
    if f >= LEFT:
        t = (f - LEFT) / (1.0 - LEFT) * 0.30
        c = [b + (255 - b) * t for b in base[:3]]
    else:
        c = [b * (f / LEFT) for b in base[:3]]
    return tuple(max(0, min(255, int(round(v)))) for v in c) + (255,)


def side_factor(nx, ny):
    """Shade of a vertical face by its outward normal: +y (faces left) is
    LEFT, +x (faces right) RIGHT, in steps (pixel art: few shades)."""
    a = math.degrees(math.atan2(ny, nx))       # 0: +x, 90: +y
    f = RIGHT + (LEFT - RIGHT) * (a / 90.0)
    f = max(0.56, min(0.96, f))
    return round(f / 0.05) * 0.05


def area(fp):
    return sum(fp[i][0] * fp[(i + 1) % len(fp)][1] - fp[(i + 1) % len(fp)][0] * fp[i][1] for i in range(len(fp))) / 2


def ccw(fp):
    return fp if area(fp) > 0 else fp[::-1]


def rrect(x0, x1, y0, y1, r, seg=5):
    pts = []
    for cx, cy, a0 in ((x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180), (x1 - r, y0 + r, 270)):
        for i in range(seg + 1):
            a = math.radians(a0 + 90 * i / seg)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return ccw(pts)


def ellipse(cx, cy, rx, ry, seg=32):
    return ccw([(cx + rx * math.cos(2 * math.pi * i / seg), cy + ry * math.sin(2 * math.pi * i / seg)) for i in range(seg)])


def rect(x0, x1, y0, y1):
    return ccw([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


def gear(cx, cy, r_out, r_in, teeth):
    pts = []
    for t in range(teeth):
        a = 2 * math.pi * t / teeth
        w = math.pi / teeth
        for da, r in ((-w * 0.95, r_in), (-w * 0.55, r_out), (w * 0.15, r_out), (w * 0.55, r_in)):
            pts.append((cx + r * math.cos(a + da), cy + r * math.sin(a + da)))
    return ccw(pts)


def ring_arrow(cx, cy, r0, r1, a0, a1, seg=28):
    """A ring from angle a0 to a1 (degrees) with an arrow head at a1."""
    a0, a1 = math.radians(a0), math.radians(a1)
    step = [a0 + (a1 - a0) * i / seg for i in range(seg + 1)]
    rm, s = (r0 + r1) / 2, 1 if a1 > a0 else -1
    out = [(cx + r1 * math.cos(a), cy + r1 * math.sin(a)) for a in step]
    head = [(cx + (r1 + 12) * math.cos(a1), cy + (r1 + 12) * math.sin(a1)),
            (cx + rm * math.cos(a1 + s * 0.62), cy + rm * math.sin(a1 + s * 0.62)),
            (cx + (r0 - 12) * math.cos(a1), cy + (r0 - 12) * math.sin(a1))]
    inn = [(cx + r0 * math.cos(a), cy + r0 * math.sin(a)) for a in reversed(step)]
    return ccw(out + head + inn)


class Canvas:
    """World: x to the lower right, y to the lower left, z up; the viewer
    looks from +x+y. Screen: u = x - y, v = (x + y) / 2 - z (2:1)."""

    def __init__(self, n, variant, fit=None):
        self.n, self.variant = n, variant
        self.rec = fit is None
        self.bbox = [1e9, 1e9, -1e9, -1e9]
        if not self.rec:
            self.k, self.ox, self.oy = fit
        self.im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)
        self.outline = max(1, round(n / 42))                        # 256: 6, 64: 2, 32: 1
        self.inner = round(n / 85) if variant == "outline" else 0   # 256: 3, 64: 1, <48: none

    def P(self, x, y, z):
        u, v = x - y, (x + y) / 2 - z
        if self.rec:
            b = self.bbox
            b[0], b[1], b[2], b[3] = min(b[0], u), min(b[1], v), max(b[2], u), max(b[3], v)
            return (0, 0)
        return (round(self.ox + u * self.k), round(self.oy + v * self.k))

    # --- drawing

    def face(self, pts3, color, edge=True):
        pts = [self.P(*p) for p in pts3]
        if self.rec:
            return
        color = color if len(color) == 4 else color + (255,)
        self.d.polygon(pts, fill=color)
        if edge and self.inner:
            ec = tuple(int(c * 0.55) for c in color[:3]) + (255,)
            self.d.line(pts + [pts[0]], fill=ec, width=self.inner)

    def prism(self, fp, z0, h, base, top=None, smooth=None):
        """A footprint (x, y) extruded from z0 up by h."""
        fp = ccw(fp)
        smooth = len(fp) > 8 if smooth is None else smooth
        sides = []
        for i in range(len(fp)):
            (ax, ay), (bx, by) = fp[i], fp[(i + 1) % len(fp)]
            nx, ny = by - ay, -(bx - ax)
            if nx + ny > 1e-9:
                sides.append(((ax + ay + bx + by) / 2, (ax, ay), (bx, by), nx, ny))
        sides.sort(key=lambda s: s[0])
        for _, (ax, ay), (bx, by), nx, ny in sides:
            self.face([(ax, ay, z0), (bx, by, z0), (bx, by, z0 + h), (ax, ay, z0 + h)],
                      shade(base, side_factor(nx, ny)), edge=not smooth)
        if smooth and self.inner and not self.rec:   # the side band's outer edge
            lo = [self.P(x, y, z0) for x, y in fp]
            self.d.line(lo + [lo[0]], fill=tuple(int(c * 0.45) for c in base) + (255,), width=self.inner)
        self.face([(x, y, z0 + h) for x, y in fp], shade(top or base, TOP))

    def box(self, x0, x1, y0, y1, z0, z1, base, top=None):
        self.prism(rect(x0, x1, y0, y1), z0, z1 - z0, base, top)

    def on_y(self, y, pts2, color, edge=True):
        """A shape on the face y = const (it faces lower left): (x, z)."""
        self.face([(u, y, v) for u, v in pts2], color, edge)

    def on_x(self, x, pts2, color, edge=True):
        """A shape on the face x = const (it faces lower right): (y, z)."""
        self.face([(x, u, v) for u, v in pts2], color, edge)

    def on_z(self, z, pts2, color, edge=True):
        self.face([(u, v, z) for u, v in pts2], color, edge)

    def line(self, pts3, color, w):
        pts = [self.P(*p) for p in pts3]
        if self.rec:
            return
        self.d.line(pts, fill=color + (255,), width=max(1, round(w * self.k)), joint="curve")

    def finish(self):
        if self.variant != "outline":
            return self.im
        t = self.outline
        mask = self.im.split()[3].point(lambda a: 255 if a else 0).filter(ImageFilter.MaxFilter(2 * t + 1))
        out = Image.new("RGBA", self.im.size, (0, 0, 0, 0))
        out.paste(Image.new("RGBA", self.im.size, INK), mask=mask)
        out.alpha_composite(self.im)
        return out


def circ(cx, cz, r, seg=28):
    return [(cx + r * math.cos(2 * math.pi * i / seg), cz + r * math.sin(2 * math.pi * i / seg)) for i in range(seg)]


def sq(x0, x1, z0, z1):
    return [(x0, z0), (x1, z0), (x1, z1), (x0, z1)]


# ------------------------------------------------------------------ icons

def mouse(c):
    c.line([(0, -52, 3), (2, -72, 2), (16, -86, 2), (40, -92, 2), (60, -86, 2)], DARK, 5)   # cable
    c.prism(rrect(-34, 34, -54, 54, 28), 0, 16, PLASTIC)
    c.prism(rrect(-27, 27, -47, 46, 22), 16, 8, PLASTIC)
    c.line([(-26, -10, 24), (26, -10, 24)], PLASTIC_D, 3)                     # buttons
    c.line([(0, -46, 24), (0, -10, 24)], PLASTIC_D, 3)


def sound(c):
    c.box(0, 60, -50, -4, 0, 112, CABINET)
    for r, col in ((24, DARK), (19, (120, 118, 130)), (7, DARK)):             # woofer
        c.on_y(-4 + 0.1, circ(30, 38, r), col)
    for r, col in ((12, DARK), (5, (150, 148, 160))):                         # tweeter
        c.on_y(-4 + 0.1, circ(30, 88, r), col)
    for r in (36, 52, 68):                                                    # sound
        c.line([(30, -4 + r * math.cos(math.radians(a)), 38 + r * math.sin(math.radians(a)))
                for a in range(-40, 41, 8)], BLUE, 6)


def scheme_palette(c):
    c.prism(ellipse(0, 0, 74, 54, 40), 0, 7, WOOD)
    c.on_z(7.1, ellipse(-46, 26, 10, 8, 16), shade(WOOD, 0.45), edge=False)   # thumb hole
    blobs = [((-52, -10), RED), ((-36, -32), YELLOW), ((-8, -42), GREEN),
             ((20, -40), BLUE), ((44, -26), PURPLE), ((56, -2), WHITE)]
    for (x, y), col in sorted(blobs, key=lambda b: b[0][0] + b[0][1]):
        c.prism(ellipse(x, y, 11, 11, 16), 7, 6, col)
    c.box(-36, -22, 19, 25, 8, 13, (90, 70, 50))                              # brush: bristles
    c.box(-22, -10, 18, 26, 7, 14, METAL)                                     # ferrule
    c.box(-10, 66, 19, 25, 8, 13, RED)                                        # handle
    c.on_x(-36 - 0.1, sq(19, 25, 8, 13), BLUE)


def system(c):
    c.box(-30, 30, -26, 10, 0, 6, PLASTIC_D)                                  # base
    c.box(-36, 36, -44, -8, 52, 108, PLASTIC_D)                               # back
    c.box(-8, 8, -16, 0, 6, 46, PLASTIC_D)                                    # neck
    c.box(-54, 54, -8, 4, 40, 122, PLASTIC)                                   # bezel
    c.on_y(4.1, sq(-46, 46, 48, 114), SCREEN)
    c.on_y(4.2, [(-40, 108), (-22, 108), (-40, 78)], SCREEN_L, edge=False)
    c.prism(gear(44, 50, 32, 24, 10), 0, 12, METAL)
    c.on_z(12.1, ellipse(44, 50, 9, 9, 16), shade(METAL, 0.4), edge=False)


def launchpad(c):
    for x in (-70, 64):
        c.box(x, x + 8, -6, 4, 0, 30, PLASTIC_D)                               # feet
    c.box(-86, 86, -10, 8, 30, 80, PLASTIC)
    for i, col in enumerate((BLUE, RED, GREEN, YELLOW)):
        cx = -60 + i * 40
        c.box(cx - 16, cx + 16, 8, 13, 38, 72, PLASTIC_L)
        c.on_y(13.1, sq(cx - 9, cx + 9, 46, 64), col)
        c.on_z(80.1, [(cx - 7, 2), (cx + 7, 2), (cx, -6)], DARK, edge=False)   # drawer arrow


def warpcenter(c):
    c.box(-82, 82, -8, 4, 0, 104, PLASTIC)
    c.on_y(4.1, sq(-76, 76, 6, 98), TEAL)
    for x, z, col in ((-66, 70, YELLOW), (-66, 40, WHITE), (-66, 12, YELLOW)):   # desktop icons
        c.on_y(4.2, sq(x, x + 14, z, z + 12), col)
    c.box(-86, 86, -10, 16, 104, 124, PLASTIC_L)                              # the bar
    c.on_y(16.1, sq(-80, -62, 108, 120), RED)
    for x in (-52, -36, -20, -4):
        c.on_y(16.1, sq(x, x + 10, 109, 119), PLASTIC_D)
    c.on_y(16.1, sq(34, 80, 108, 120), LCD)
    for x in (40, 52, 68):
        c.on_y(16.2, sq(x, x + 6, 111, 117), LCD_ON, edge=False)


def envelope(c, x0, x1, y, z0, z1, col):
    c.box(x0, x1, y - 3, y + 3, z0, z1, col)
    mx, mz = (x0 + x1) / 2, z0 + (z1 - z0) * 0.45
    c.line([(x0 + 2, y + 3.2, z1 - 2), (mx, y + 3.2, mz), (x1 - 2, y + 3.2, z1 - 2)], shade(col, 0.5)[:3], 3)
    return mx, mz


def messages(c):
    envelope(c, -44, 66, -32, 16, 88, PAPER_Y)
    mx, mz = envelope(c, -64, 48, 0, 0, 74, PAPER)
    c.on_y(3.3, circ(mx, mz, 9, 16), RED)


def restore_desktop(c):
    c.box(-76, 76, -58, 58, 0, 10, TEAL)
    for y, col in ((-44, YELLOW), (-18, WHITE), (8, YELLOW)):
        c.box(-62, -48, y, y + 14, 10, 20, col)
    arrow = ring_arrow(26, 10, 24, 40, 330, 50)
    c.on_z(10.1, arrow, shade(TEAL, 0.5), edge=False)                         # its shadow
    c.prism(arrow, 30, 12, GREEN)


def minwin_viewer(c):
    c.box(-70, 70, -50, 50, 0, 8, PLASTIC)
    for x0, y0 in ((-62, -34), (-46, -6), (-30, 22)):
        c.box(x0, x0 + 96, y0 - 3, y0 + 3, 8, 84, PLASTIC_L)
        c.on_y(y0 + 3.1, sq(x0 + 5, x0 + 91, 13, 66), WHITE)
        c.on_y(y0 + 3.1, sq(x0 + 5, x0 + 91, 68, 79), BLUE)
        c.on_y(y0 + 3.2, sq(x0 + 77, x0 + 87, 70, 77), PLASTIC_L, edge=False)


QMARK = [".###.",
         "#...#",
         "....#",
         "...#.",
         "..#..",
         ".....",
         "..#.."]


def drive_missing(c):
    c.box(-62, 62, -38, 38, 0, 32, (186, 188, 194))
    for x in range(-50, -10, 9):
        c.on_y(38.1, sq(x, x + 4, 8, 24), PLASTIC_D, edge=False)
    c.on_y(38.1, circ(46, 16, 5, 12), (120, 124, 130))
    vox = [(col, row) for row, line in enumerate(QMARK) for col, ch in enumerate(line) if ch == "#"]
    for col, row in sorted(vox, key=lambda v: (v[0], -v[1])):
        x, z = -25 + col * 10, 48 + (len(QMARK) - 1 - row) * 10
        c.box(x, x + 10, -5, 5, z, z + 10, BLUE)


ICONS = [("mouse", "Mouse", mouse), ("sound", "Sound", sound), ("scheme-palette", "Scheme Palette", scheme_palette),
         ("system", "System", system), ("launchpad", "LaunchPad", launchpad), ("warpcenter", "WarpCenter", warpcenter),
         ("messages", "Messages", messages), ("restore-desktop", "Restore Desktop", restore_desktop),
         ("minwin-viewer", "Minimized Window Viewer", minwin_viewer), ("drive-missing", "Drive (not present)", drive_missing)]


def render(fn, n, variant):
    rec = Canvas(256, variant)
    fn(rec)
    u0, v0, u1, v1 = rec.bbox
    room = 256 - 2 * 14
    s = min(room / (u1 - u0), room / (v1 - v0), 1.7)
    k = s * n / 256
    c = Canvas(n, variant, (k, n / 2 - (u0 + u1) / 2 * k, n / 2 - (v0 + v1) / 2 * k))
    fn(c)
    return c.finish()


def font(sz):
    try:
        return ImageFont.truetype("DejaVuSans.ttf", sz)
    except OSError:
        return ImageFont.load_default(sz)


def sheets(out, imgs, sizes):
    f, fb = font(14), font(18)
    big = [s for s in sizes if s >= 128]
    for variant in ("outline", "flat"):
        for s in big:   # the masters, 5 to a row
            cols, cell = 5, s + 24
            sh = Image.new("RGBA", (cols * cell, 40 + 2 * (cell + 24)), (196, 198, 202, 255))
            d = ImageDraw.Draw(sh)
            d.text((12, 10), f"WPS icons, set A - {variant}, {s} px", fill=(20, 20, 20), font=fb)
            for i, (name, title, _) in enumerate(ICONS):
                x, y = (i % cols) * cell + 12, 40 + (i // cols) * (cell + 24)
                sh.alpha_composite(imgs[variant, name, s], (x, y))
                d.text((x, y + s + 2), title, fill=(20, 20, 20), font=f)
            sh.save(f"{out}/sheet-{variant}-{s}.png")
    small = [s for s in sizes if s < 128]
    if not small:
        return
    colw = [s + 10 for s in small]
    block = sum(colw) + 16
    bgs = [("light", (204, 204, 204, 255)), ("desktop teal", (0, 128, 128, 255)), ("dark", (48, 48, 52, 255))]
    rowh = max(small) + 10
    W, H = 190 + len(bgs) * 2 * block, 64 + len(ICONS) * rowh
    sh = Image.new("RGBA", (W, H), (236, 236, 236, 255))
    d = ImageDraw.Draw(sh)
    d.text((12, 8), "Desktop sizes " + "/".join(map(str, small)) + " - outline | flat, on three backgrounds", fill=(20, 20, 20), font=fb)
    for b, (bname, bg) in enumerate(bgs):
        for vi, variant in enumerate(("outline", "flat")):
            x0 = 190 + (b * 2 + vi) * block
            d.rectangle((x0, 40, x0 + block - 8, H - 4), fill=bg)
            d.text((x0 + 4, 42), f"{variant} / {bname}", fill=(255, 255, 255) if bname != "light" else (20, 20, 20), font=f)
            for i, (name, title, _) in enumerate(ICONS):
                x, y = x0 + 6, 62 + i * rowh
                for s, w in zip(small, colw):
                    sh.alpha_composite(imgs[variant, name, s], (x, y + (max(small) - s) // 2))
                    x += w
    for i, (name, title, _) in enumerate(ICONS):
        d.text((10, 62 + i * rowh + max(small) // 2 - 8), title, fill=(20, 20, 20), font=f)
    sh.save(f"{out}/sheet-small.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--sizes", default="256,64,48,40,32")
    a = ap.parse_args()
    sizes = [int(s) for s in a.sizes.split(",")]
    imgs = {}
    for variant in ("outline", "flat"):
        os.makedirs(f"{a.out}/{variant}", exist_ok=True)
        for name, _, fn in ICONS:
            for s in sizes:
                im = render(fn, s, variant)
                im.save(f"{a.out}/{variant}/{name}-{s}.png")
                imgs[variant, name, s] = im
    sheets(a.out, imgs, sizes)


if __name__ == "__main__":
    main()
