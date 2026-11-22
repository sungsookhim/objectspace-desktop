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
        self.outline = max(1, round(n / 85))                        # 256: 3, 64 and down: 1
        self.inner = round(n / 128) if variant == "outline" else 0  # 256: 2, 128: 1, 64 and down: none

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

    def prism(self, fp, z0, h, base, top=None, smooth=None, lift=0.0, edges=True):
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
                      shade(base, side_factor(nx, ny) * (1 - lift) + TOP * lift), edge=edges and not smooth)
        if smooth and edges and self.inner and not self.rec:   # the side band's outer edge
            lo = [self.P(x, y, z0) for x, y in fp]
            self.d.line(lo + [lo[0]], fill=tuple(int(c * 0.45) for c in base) + (255,), width=self.inner)
        self.face([(x, y, z0 + h) for x, y in fp], shade(top or base, TOP), edge=edges)

    def dome(self, fp_at, z0, h, base, inset, layers=12):
        """A rounded top: fp_at(d) is the footprint inset by d; each layer
        steps in as a quarter circle does and turns lighter toward the top."""
        for i in range(layers):
            t = i / layers
            d = inset * (1 - math.sqrt(1 - t * t))
            self.prism(fp_at(d), z0 + h * t, h / layers + 0.01, base, lift=min(0.9, t ** 1.4), edges=False)

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
    c.line([(0, -50, 3), (2, -72, 2), (16, -86, 2), (40, -92, 2), (60, -86, 2)], DARK, 5)   # cable
    c.prism(rrect(-34, 34, -56, 56, 30), 0, 8, PLASTIC_D, top=PLASTIC)
    c.dome(lambda d: rrect(-34 + d, 34 - d, -56 + d, 56 - d, max(4, 30 - d)), 8, 26, PLASTIC, 24)
    top = 8 + 26
    c.line([(-17, -14, top - 3), (0, -12, top), (17, -14, top - 3)], PLASTIC_D, 3)   # buttons
    c.line([(0, -40, top - 4), (0, -12, top)], PLASTIC_D, 3)


def cone(c, y, cx, cz, r, depth, steps=6):
    """A speaker cone sunk into the face y: rings stepping in and back,
    then the dust cap standing out."""
    c.on_y(y + 0.1, circ(cx, cz, r + 4), DARK)                                # surround
    for k in range(steps):
        t = k / steps
        c.on_y(y - depth * t, circ(cx, cz, r * (1 - 0.7 * t)), shade((120, 118, 132), 0.62 + 0.3 * t), edge=False)
    for k in range(3):
        c.on_y(y - depth + 2 * k, circ(cx, cz, r * 0.32 * (1 - 0.18 * k)), shade((150, 148, 162), 0.7 + 0.12 * k), edge=False)


def note_shapes(x, z, beamed):
    """A note (or two joined by a beam) on a face: (x, z) polygons."""
    def head(hx, hz):
        a = math.radians(25)
        return [(hx + 9 * math.cos(t) * math.cos(a) - 6 * math.sin(t) * math.sin(a),
                 hz + 9 * math.cos(t) * math.sin(a) + 6 * math.sin(t) * math.cos(a))
                for t in [2 * math.pi * i / 16 for i in range(16)]]
    if not beamed:
        return [head(x, z), sq(x + 5.5, x + 9, z, z + 38),
                [(x + 9, z + 38), (x + 9, z + 28), (x + 20, z + 18), (x + 19, z + 26)]]
    x2, z2 = x + 26, z + 8
    return [head(x, z), head(x2, z2), sq(x + 5.5, x + 9, z, z + 38), sq(x2 + 5.5, x2 + 9, z2, z2 + 38),
            [(x + 5.5, z + 32), (x2 + 9, z2 + 32), (x2 + 9, z2 + 39), (x + 5.5, z + 39)]]


def note(c, y, x, z, beamed, col=DARK):
    for dy, cc in ((-3, shade(col, 0.5)), (0, col)):
        for shp in note_shapes(x, z, beamed):
            c.on_y(y + dy, shp, cc, edge=False)


def sound(c):
    c.box(0, 60, -52, -8, 0, 112, CABINET)
    c.box(2, 58, -8, -3, 2, 110, (100, 96, 110))                              # front baffle
    cone(c, -3, 30, 40, 19, 10)
    cone(c, -3, 30, 89, 9, 5, 4)
    note(c, 26, -14, 86, True, BLUE)                                          # music, in front
    note(c, 46, -30, 40, False, RED)


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


def win(c, x0, x1, y, z0, z1, d=4, body=WHITE):
    """A window standing on the face y: frame, title bar with its buttons."""
    c.box(x0, x1, y - d, y, z0, z1, PLASTIC_L)
    c.on_y(y + 0.1, sq(x0 + 4, x1 - 4, z0 + 4, z1 - 16), body)
    c.on_y(y + 0.1, sq(x0 + 4, x1 - 4, z1 - 14, z1 - 4), (30, 40, 140))
    for k in range(3):
        bx = x1 - 14 - k * 11
        c.on_y(y + 0.2, sq(bx, bx + 8, z1 - 12, z1 - 6), PLASTIC_L, edge=False)


def system(c):
    """System: the computer as a whole - the case, its monitor showing the
    desktop, and the keyboard."""
    c.box(-58, 28, -26, -8, 0, 6, PLASTIC_D)                                  # monitor: base, neck
    c.box(-22, -8, -22, -12, 6, 30, PLASTIC_D)
    c.box(-46, 16, -40, -16, 42, 92, PLASTIC_D)                               # back
    c.box(-62, 32, -16, -4, 30, 104, PLASTIC)                                 # bezel
    c.on_y(-3.9, sq(-55, 25, 37, 97), TEAL)
    for z in (80, 62):
        c.on_y(-3.8, sq(-49, -39, z, z + 9), YELLOW, edge=False)
    c.on_y(-3.8, sq(-49, 25, 37, 43), PLASTIC_L, edge=False)                  # the bar along the screen's foot
    c.box(42, 82, -62, 6, 0, 112, PLASTIC)                                    # the case, facing as the monitor
    for z in (94, 80):                                                        # drive bays
        c.on_y(6.1, sq(48, 76, z, z + 10), PLASTIC_D)
        c.on_y(6.2, sq(52, 72, z + 4, z + 6), DARK, edge=False)
    c.on_y(6.1, circ(62, 26, 5, 12), PLASTIC_L)                               # power, lamp
    c.on_y(6.2, sq(53, 58, 12, 15), LCD_ON, edge=False)
    c.box(-62, 30, 12, 40, 0, 7, PLASTIC_L)                                   # keyboard
    for r in range(3):
        for k in range(10):
            x = -57 + k * 8.6
            y = 15 + r * 8
            c.on_z(7.1, rect(x, x + 6.5, y, y + 6), PLASTIC_D, edge=False)


# glyphs on a face y = const, (x, z) centred on cx, cz, half-size h

def g_padlock(c, y, cx, cz, h):
    r = h * 0.55
    c.line([(cx + r * math.cos(math.radians(a)), y + 0.1, cz + h * 0.15 + r * math.sin(math.radians(a))) for a in range(0, 181, 15)],
           (110, 110, 118), h * 0.3)
    c.on_y(y + 0.2, sq(cx - h * 0.85, cx + h * 0.85, cz - h, cz + h * 0.2), (214, 168, 40))
    c.on_y(y + 0.3, circ(cx, cz - h * 0.4, h * 0.18, 10), DARK, edge=False)


def g_find(c, y, cx, cz, h):
    c.line([(cx + h * 0.2, y + 0.1, cz - h * 0.2), (cx + h * 0.9, y + 0.1, cz - h * 0.9)], DARK, h * 0.35)
    c.on_y(y + 0.2, circ(cx - h * 0.2, cz + h * 0.2, h * 0.68, 16), DARK, edge=False)
    c.on_y(y + 0.3, circ(cx - h * 0.2, cz + h * 0.2, h * 0.48, 16), (176, 214, 250), edge=False)


def g_power(c, y, cx, cz, h):
    r = h * 0.7
    c.line([(cx + r * math.cos(math.radians(a)), y + 0.1, cz - h * 0.1 + r * math.sin(math.radians(a))) for a in range(125, 416, 15)],
           RED, h * 0.28)
    c.line([(cx, y + 0.2, cz + h * 0.85), (cx, y + 0.2, cz + h * 0.05)], RED, h * 0.28)


def g_windows(c, y, cx, cz, h):
    for k in range(2):
        x0, z1 = cx - h * 0.9 + k * h * 0.6, cz + h * 0.8 - k * h * 0.6
        c.on_y(y + 0.1 + k * 0.1, sq(x0, x0 + h * 1.2, z1 - h, z1), WHITE, edge=False)
        c.on_y(y + 0.15 + k * 0.1, sq(x0, x0 + h * 1.2, z1 - h * 0.28, z1), (30, 40, 140), edge=False)


def g_folder(c, y, cx, cz, h):
    c.on_y(y + 0.1, [(cx - h, cz - h * 0.8), (cx + h, cz - h * 0.8), (cx + h, cz + h * 0.5), (cx - h * 0.1, cz + h * 0.5),
                     (cx - h * 0.3, cz + h * 0.75), (cx - h, cz + h * 0.75)], (214, 160, 40))
    c.on_y(y + 0.2, sq(cx - h, cx + h, cz - h * 0.8, cz + h * 0.3), YELLOW)


def g_terminal(c, y, cx, cz, h):
    c.on_y(y + 0.1, sq(cx - h, cx + h, cz - h * 0.8, cz + h * 0.8), (30, 32, 40))
    c.line([(cx - h * 0.7, y + 0.2, cz + h * 0.4), (cx - h * 0.3, y + 0.2, cz + h * 0.1), (cx - h * 0.7, y + 0.2, cz - h * 0.2)],
           LCD_ON, h * 0.16)
    c.on_y(y + 0.2, sq(cx - h * 0.15, cx + h * 0.5, cz - h * 0.45, cz - h * 0.3), LCD_ON, edge=False)


def g_document(c, y, cx, cz, h):
    c.on_y(y + 0.1, [(cx - h * 0.75, cz - h), (cx + h * 0.75, cz - h), (cx + h * 0.75, cz + h * 0.55),
                     (cx + h * 0.3, cz + h), (cx - h * 0.75, cz + h)], WHITE)
    for k in range(4):
        z = cz + h * 0.45 - k * h * 0.38
        c.on_y(y + 0.2, sq(cx - h * 0.5, cx + h * 0.45, z - h * 0.07, z + h * 0.07), (150, 150, 160), edge=False)


def g_envelope(c, y, cx, cz, h):
    c.on_y(y + 0.1, sq(cx - h, cx + h, cz - h * 0.7, cz + h * 0.7), WHITE)
    c.line([(cx - h * 0.9, y + 0.2, cz + h * 0.6), (cx, y + 0.2, cz - h * 0.05), (cx + h * 0.9, y + 0.2, cz + h * 0.6)],
           (120, 120, 130), h * 0.15)


def button(c, x0, x1, y, z0, z1, glyph, d=4):
    c.box(x0, x1, y, y + d, z0, z1, PLASTIC_L)
    glyph(c, y + d, (x0 + x1) / 2, (z0 + z1) / 2, (min(x1 - x0, z1 - z0)) * 0.34)


def launchpad(c):
    """OS/2's LaunchPad: a floating bar; the system buttons (Lockup, Find,
    Shut down, Window list) two by two on the left, big object buttons
    with their drawers' arrows above."""
    c.box(-96, 100, -10, 6, 20, 84, PLASTIC)
    for (x0, z0), g in zip(((-90, 54), (-64, 54), (-90, 26), (-64, 26)), (g_padlock, g_find, g_power, g_windows)):
        button(c, x0, x0 + 24, 6, z0, z0 + 24, g, 3)
    for x0, g in ((-32, g_folder), (12, g_terminal), (56, g_document)):
        button(c, x0, x0 + 38, 6, 28, 76, g)
        cx = x0 + 19
        c.box(cx - 10, cx + 10, -6, 2, 84, 90, PLASTIC_L)                    # drawer tab
        c.on_y(2.1, [(cx - 6, 85), (cx + 6, 85), (cx, 89.5)], DARK, edge=False)


def warpcenter(c):
    """The WarpCenter: the bar along the screen's top - the desktop menu
    and window list buttons, the meters, messages, the clock, Lockup and
    Shut down - over the desktop it serves."""
    c.box(-94, 94, -8, 4, 16, 86, PLASTIC)
    c.on_y(4.1, sq(-88, 88, 22, 86), TEAL)
    for x, z, col in ((-80, 62, YELLOW), (-80, 36, WHITE)):
        c.on_y(4.2, sq(x, x + 14, z, z + 12), col)
    c.box(-100, 100, -10, 14, 86, 118, PLASTIC_L)
    y = 14
    button(c, -96, -74, y, 90, 114, lambda c, y, cx, cz, h: (c.on_y(y + 0.1, sq(cx - h, cx + h, cz - h * 0.8, cz + h * 0.8), TEAL),
                                                              c.on_y(y + 0.2, sq(cx - h * 0.6, cx - h * 0.1, cz + h * 0.1, cz + h * 0.5), YELLOW, edge=False)), 2)
    button(c, -70, -48, y, 90, 114, g_windows, 2)
    c.on_y(y + 0.1, sq(-42, -10, 91, 113), LCD)                               # meters
    for k, hgt in enumerate((16, 9, 19, 6)):
        x = -39 + k * 7.5
        c.on_y(y + 0.2, sq(x, x + 5, 93, 93 + hgt), LCD_ON if hgt < 15 else YELLOW, edge=False)
    g_envelope(c, y, 6, 102, 11)
    c.on_y(y + 0.1, sq(22, 62, 92, 112), LCD)                                 # clock
    for x in (25, 33, 45, 53):
        c.on_y(y + 0.2, sq(x, x + 6, 95, 109), LCD_ON, edge=False)
    for z in (98, 105):
        c.on_y(y + 0.2, sq(40, 42, z, z + 2), LCD_ON, edge=False)
    button(c, 66, 82, y, 92, 112, g_padlock, 2)
    button(c, 84, 98, y, 92, 112, g_power, 2)


def envelope(c, x0, x1, y, z0, z1, col):
    c.box(x0, x1, y - 3, y + 3, z0, z1, col)
    mx, mz = (x0 + x1) / 2, z0 + (z1 - z0) * 0.45
    c.line([(x0 + 2, y + 3.2, z1 - 2), (mx, y + 3.2, mz), (x1 - 2, y + 3.2, z1 - 2)], shade(col, 0.5)[:3], 3)
    return mx, mz


def messages(c):
    envelope(c, -44, 66, -32, 16, 88, PAPER_Y)
    mx, mz = envelope(c, -64, 48, 0, 0, 74, PAPER)
    c.on_y(3.3, circ(mx, mz, 9, 16), RED)


def folder3d(c, x, y, z, w=34, h=26, d=6):
    """A folder standing up, facing lower left."""
    c.box(x, x + w * 0.45, y, y + d, z + h, z + h + 5, (214, 160, 40))       # tab
    c.box(x, x + w, y, y + d, z, z + h, YELLOW)


def dashed_rect(c, z, x0, x1, y0, y1, col, dash=7, w=2.5):
    for x in range(int(x0), int(x1), dash * 2):
        for yy in (y0, y1 - w):
            c.on_z(z, rect(x, min(x + dash, x1), yy, yy + w), col, edge=False)
    for y in range(int(y0), int(y1), dash * 2):
        for xx in (x0, x1 - w):
            c.on_z(z, rect(xx, xx + w, y, min(y + dash, y1)), col, edge=False)


def restore_desktop(c):
    """Restore Desktop: default objects put back where they belong - the
    Trash Can in its place, a folder coming down into its marked place."""
    c.box(-64, 64, -54, 54, 0, 8, TEAL)
    c.prism(ellipse(-42, 22, 17, 17, 24), 8, 42, (176, 180, 188))           # the Trash Can, in place
    c.on_z(50.1, ellipse(-42, 22, 12, 12, 20), shade((176, 180, 188), 0.45), edge=False)
    x0, x1, y0, y1 = -4, 44, 14, 30                                          # the folder's place
    c.on_z(8.1, rect(x0 + 3, x1 - 3, y0 + 3, y1 - 3), shade(TEAL, 0.55), edge=False)
    dashed_rect(c, 8.2, x0 - 4, x1 + 4, y0 - 4, y1 + 4, WHITE)
    lift = 34
    for x in (x0, x1):                                                       # dropping straight down
        for z in range(14, lift, 8):
            c.line([(x, y1, z), (x, y1, z + 4)], WHITE, 2)
    c.box(x0, x0 + 20, y0, y1, lift + 36, lift + 42, (214, 160, 40))         # the folder: tab, body
    c.box(x0, x1, y0, y1, lift, lift + 36, YELLOW)
    ax = (x0 + x1) / 2                                                       # the arrow, pointing down
    shape = [(ax - 7, 140), (ax + 7, 140), (ax + 7, 112), (ax + 17, 112), (ax, 92), (ax - 17, 112), (ax - 7, 112)]
    c.on_y(y1 - 4, shape, shade(GREEN, 0.5), edge=False)
    c.on_y(y1, shape, GREEN)


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
