#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""Objectspace's cursors (0.7 step 8): OS/2's arrow, busy clock, hand, text,
move and resize cursors, drawn anew in the icon set's manner: flat, a white
body with a thin dark outline (1 px at 24 and 32, 2 px at 48 and 64) and a
soft shadow down and to the right, light from the upper left as the icons.

Each is drawn on a 32-unit grid and rendered at every size on its own
(supersampled), not scaled from one picture. Run from the tree's top:
  python3 tools/icons/make_cursors.py OUTDIR [--sizes 24,32,48,64] [--sheet]
  python3 tools/icons/make_cursors.py --theme THEMEDIR
The first writes OUTDIR/NAME-SIZE.png (and NAME-SIZE-fFRAME.png for the
busy clock's turning hand), OUTDIR/hotspots.txt, and with --sheet
OUTDIR/sheet.png. --theme writes an Xcursor theme (the owner approved the
drawings, 2026-11-27): THEMEDIR/index.theme and THEMEDIR/cursors/NAME, each
with every size (and the busy clock's frames), and links for the other
names programs ask for (X11's and CSS's); a name not here comes from the
theme it inherits (Adwaita)."""
import argparse
import math
import os
import struct

from PIL import Image, ImageDraw, ImageFilter, ImageFont

INK = (24, 22, 30, 255)
BODY = (255, 255, 255, 255)
FACE = (250, 248, 238, 255)
CLOCK_HAND = (24, 22, 30, 255)
SHADOW = (0, 0, 0, 96)
SS = 8          # supersampling
FRAMES = 8      # the busy clock's hand, once round


def arrow_pts(x=1.0, y=1.0, k=1.0):
    p = [(0, 0), (0, 21.5), (5.2, 16.6), (8.6, 24.6), (12.0, 23.2), (8.6, 15.4), (15.6, 15.4)]
    return [(x + a * k, y + b * k) for a, b in p]


def double_arrow(cx, cy, angle, length=27, head=7.4, shaft=2.8):
    """A double-headed arrow through (cx, cy) along angle (degrees)."""
    a = math.radians(angle)
    ux, uy = math.cos(a), math.sin(a)
    vx, vy = -uy, ux
    h, s2, hw = length / 2, shaft / 2, head * 0.9
    P = lambda t, w: (cx + ux * t + vx * w, cy + uy * t + vy * w)
    return [P(h, 0), P(h - head, hw), P(h - head, s2), P(-(h - head), s2), P(-(h - head), hw),
            P(-h, 0), P(-(h - head), -hw), P(-(h - head), -s2), P(h - head, -s2), P(h - head, -hw)]


def four_way(cx=16, cy=16, r=14, head=5.2, wide=4.0, shaft=2.6):
    """The move cursor: four arrows from the middle, round clockwise."""
    pts = []
    s2 = shaft / 2
    for ux, uy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        vx, vy = -uy, ux
        P = lambda t, w: (cx + ux * t + vx * w, cy + uy * t + vy * w)
        pts += [P(s2, -s2), P(r - head, -s2), P(r - head, -wide), P(r, 0), P(r - head, wide), P(r - head, s2)]
    return pts


# ---------------------------------------------------------------- shapes
# Each cursor: a list of (kind, data, fill) layers on the 32-unit grid,
# and its hotspot. kind: "poly" (points), "ellipse" (box), "line" (points,
# width), "rrect" (box, radius). Outlined layers get the outline.

def c_default(_f):
    return [("poly", arrow_pts(), BODY, True)], (1, 1)


def clock_layers(cx, cy, r, f, hand_w=1.7):
    a = 2 * math.pi * f / FRAMES - math.pi / 2
    m = [("ellipse", (cx - r, cy - r, cx + r, cy + r), FACE, True)]
    for k in range(4):   # the quarter marks
        t = k * math.pi / 2
        m.append(("line", [(cx + math.cos(t) * r * 0.66, cy + math.sin(t) * r * 0.66),
                           (cx + math.cos(t) * r * 0.86, cy + math.sin(t) * r * 0.86)], INK, max(1.2, r / 9)))
    m.append(("line", [(cx, cy), (cx + math.cos(-math.pi / 3) * r * 0.48, cy + math.sin(-math.pi / 3) * r * 0.48)], CLOCK_HAND, hand_w * 1.2))
    m.append(("line", [(cx, cy), (cx + math.cos(a) * r * 0.74, cy + math.sin(a) * r * 0.74)], CLOCK_HAND, hand_w))
    m.append(("ellipse", (cx - r * 0.1, cy - r * 0.1, cx + r * 0.1, cy + r * 0.1), INK, False))
    return m


def c_wait(f):
    # OS/2's busy pointer: a clock (its knob on top), the minute hand turning
    layers = [("rrect", (13.2, 1.6, 18.8, 5.6), BODY, True)]
    layers += clock_layers(16, 17, 12.4, f)
    return layers, (16, 17)


def c_progress(f):
    # working in the background: the arrow, a small clock by it
    layers = [("poly", arrow_pts(), BODY, True)]
    layers += clock_layers(22.6, 22.6, 7.6, f, hand_w=1.3)
    return layers, (1, 1)


def c_pointer(_f):
    # a hand, its first finger up
    hand = [(9.6, 3.6), (10.6, 2.0), (12.4, 1.4), (14.0, 2.0), (14.8, 3.6), (14.8, 11.6),
            (15.4, 10.6), (17.0, 10.2), (18.6, 11.0), (19.0, 12.2), (19.6, 11.4), (21.2, 11.0), (22.6, 11.8),
            (23.0, 13.0), (23.6, 12.6), (25.2, 12.8), (26.2, 14.0), (26.2, 21.0), (24.6, 26.4), (22.6, 29.6),
            (12.6, 29.6), (11.0, 27.0), (5.0, 19.8), (4.6, 18.0), (5.6, 16.6), (7.4, 16.6), (9.6, 18.6)]
    layers = [("poly", hand, BODY, True)]
    for x0, y0, y1 in ((14.8, 15.4, 20.0), (19.0, 15.6, 20.0), (23.0, 16.2, 20.0)):
        layers.append(("line", [(x0, y0), (x0, y1)], INK, 1.0))
    return layers, (12, 2)


def c_text(_f):
    # the I-beam: dark, a light edge round it to be seen on dark text
    ink = [(16, 5.5), (16, 26.5)]
    return [("line", [(12, 4.2), (14.6, 4.2), (16, 5.6), (17.4, 4.2), (20, 4.2)], INK, 1.8, "halo"),
            ("line", ink, INK, 1.8, "halo"),
            ("line", [(12, 27.8), (14.6, 27.8), (16, 26.4), (17.4, 27.8), (20, 27.8)], INK, 1.8, "halo"),
            ("line", [(13.4, 16), (18.6, 16)], INK, 1.4, "halo")], (16, 16)


def c_move(_f):
    return [("poly", four_way(), BODY, True)], (16, 16)


def resize(angle):
    return lambda _f: ([("poly", double_arrow(16, 16, angle), BODY, True)], (16, 16))


def c_not_allowed(_f):
    return [("ellipse", (4, 4, 28, 28), (214, 40, 40, 255), True),
            ("ellipse", (8.2, 8.2, 23.8, 23.8), (0, 0, 0, 0), "hole"),
            ("line", [(8.6, 23.4), (23.4, 8.6)], (214, 40, 40, 255), 3.6)], (16, 16)


def c_crosshair(_f):
    return [("line", [(16, 2), (16, 13)], INK, 1.6, "halo"), ("line", [(16, 19), (16, 30)], INK, 1.6, "halo"),
            ("line", [(2, 16), (13, 16)], INK, 1.6, "halo"), ("line", [(19, 16), (30, 16)], INK, 1.6, "halo")], (16, 16)


CURSORS = [
    ("default", "Arrow", c_default),
    ("wait", "Busy (clock)", c_wait),
    ("progress", "Busy in the background", c_progress),
    ("pointer", "Hand (a link)", c_pointer),
    ("text", "Text", c_text),
    ("move", "Move", c_move),
    ("ns-resize", "Size up and down", resize(90)),
    ("ew-resize", "Size left and right", resize(0)),
    ("nwse-resize", "Size diagonally", resize(45)),
    ("nesw-resize", "Size diagonally", resize(-45)),
    ("not-allowed", "Not allowed", c_not_allowed),
    ("crosshair", "Crosshair", c_crosshair),
]
ANIMATED = {"wait", "progress"}


def render(fn, size, frame=0):
    """One cursor at one size: an RGBA image and its hotspot in pixels."""
    layers, hot = fn(frame)
    k = size / 32 * SS
    W = size * SS
    ow = max(1, round(size / 32)) * SS   # the outline: 1 px at 24 and 32, 2 at 48 and 64
    body = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(body)
    sc = lambda pts: [(x * k, y * k) for x, y in pts]

    def draw(layer, d, outline_only=False, color=None, grow=0):
        kind, data, fill = layer[0], layer[1], layer[2]
        extra = layer[3] if len(layer) > 3 else None
        if kind == "poly":
            if outline_only:
                d.polygon(sc(data), fill=color)
                d.line(sc(data) + sc(data[:1]), fill=color, width=int(grow * 2), joint="curve")
            else:
                d.polygon(sc(data), fill=fill)
        elif kind in ("ellipse", "rrect"):
            x0, y0, x1, y1 = [v * k for v in data]
            g = grow if outline_only else 0
            box = (x0 - g, y0 - g, x1 + g, y1 + g)
            if extra == "hole":
                d.ellipse(box, fill=(0, 0, 0, 0)) if not outline_only else None
            elif kind == "ellipse":
                d.ellipse(box, fill=color if outline_only else fill)
            else:
                d.rounded_rectangle(box, radius=1.2 * k + g, fill=color if outline_only else fill)
        elif kind == "line":
            w = extra * k if isinstance(extra, (int, float)) else layer[3] * k
            width = layer[3] * k + (2 * grow if outline_only else 0)
            d.line(sc(data), fill=color if outline_only else fill, width=int(width), joint="curve")
            if not outline_only or True:
                r = width / 2
                for x, y in sc(data)[::max(1, len(data) - 1)]:
                    d.ellipse((x - r, y - r, x + r, y + r), fill=color if outline_only else fill)

    # the outline: each outlined layer drawn larger in ink, then the body over it
    outl = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    od = ImageDraw.Draw(outl)
    for layer in layers:
        mode = layer[3] if len(layer) > 3 else None
        if layer[0] == "line" and len(layer) > 4 and layer[4] == "halo":
            draw(layer, od, True, (255, 255, 255, 255), ow)   # a light edge round dark strokes
        elif mode is True:
            draw(layer, od, True, INK, ow)
    hole = None
    for layer in layers:
        if layer[0] == "line" and len(layer) > 4:
            draw((layer[0], layer[1], layer[2], layer[3]), d)
        elif len(layer) > 3 and layer[3] == "hole":
            hole = layer
        else:
            draw(layer, d)
    img = Image.alpha_composite(outl, body)
    if hole:   # (the not-allowed sign's ring: its middle clear, the bar over)
        m = Image.new("L", (W, W), 255)
        x0, y0, x1, y1 = [v * k for v in hole[1]]
        ImageDraw.Draw(m).ellipse((x0, y0, x1, y1), fill=0)
        img.putalpha(Image.composite(img.getchannel("A"), Image.new("L", (W, W), 0), m))
        ring_in = Image.new("RGBA", (W, W), (0, 0, 0, 0))
        ImageDraw.Draw(ring_in).ellipse((x0, y0, x1, y1), outline=INK, width=ow)
        img = Image.alpha_composite(img, ring_in)
        bar = Image.new("RGBA", (W, W), (0, 0, 0, 0))
        bd = ImageDraw.Draw(bar)
        for layer in layers:
            if layer[0] == "line":
                draw(layer, bd, True, INK, ow)
                draw((layer[0], layer[1], layer[2], layer[3]), bd)
        bar.putalpha(Image.composite(bar.getchannel("A"), Image.new("L", (W, W), 0), Image.eval(m, lambda v: 255 - v)))
        img = Image.alpha_composite(img, bar)
    # the shadow: the shape's alpha, down and right, soft
    sh = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    off = round(max(1, size / 32 * 1.5) * SS)
    a = img.getchannel("A").point(lambda v: v * SHADOW[3] // 255)
    sh.paste(Image.new("RGBA", (W, W), SHADOW[:3] + (255,)), (off, off), a)
    sh = sh.filter(ImageFilter.GaussianBlur(SS * max(1, size / 32)))
    out = Image.alpha_composite(sh, img).resize((size, size), Image.LANCZOS)
    return out, (round(hot[0] * size / 32), round(hot[1] * size / 32))


def sheet(out, sizes):
    names = CURSORS
    cell = 150
    cols = 6
    rows = (len(names) + cols - 1) // cols
    W, H = cols * cell, rows * (cell + 150) + 40
    im = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype("/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf", 13)
    except OSError:
        font = ImageFont.load_default()
    d.text((8, 8), "Objectspace cursors (draft): 32 px at 3x, then 24 / 32 / 48 actual size on light, teal and dark (64 made too)", fill=(0, 0, 0), font=font)
    for i, (name, label, fn) in enumerate(names):
        x0, y0 = (i % cols) * cell, 40 + (i // cols) * (cell + 150)
        c32, hot = render(fn, 32, 2)
        big = c32.resize((96, 96), Image.NEAREST)
        d.rectangle((x0 + 26, y0, x0 + 26 + 96, y0 + 96), fill=(200, 200, 200))
        im.paste(big, (x0 + 26, y0), big)
        d.text((x0 + 6, y0 + 100), f"{name}", fill=(0, 0, 0), font=font)
        d.text((x0 + 6, y0 + 116), label, fill=(90, 90, 90), font=font)
        for j, bg in enumerate(((236, 236, 236), (0, 128, 128), (40, 40, 44))):
            yy = y0 + 138 + j * 50
            d.rectangle((x0, yy, x0 + cell - 6, yy + 46), fill=bg)
            xx = x0 + 2
            for s in (24, 32, 48):
                c, _ = render(fn, s, 2)
                im.paste(c, (xx, yy + (46 - s) // 2), c)
                xx += s + 4
    im.save(os.path.join(out, "sheet.png"))


# The names programs ask for, each to one of ours (CSS's names, X11's
# cursor-font names, GNOME's and Qt's old hashes).
ALIASES = {
    "default": ["left_ptr", "arrow", "top_left_arrow", "context-menu", "08e8e1c95fe2fc01f976f1e063a24ccd"],
    "wait": ["watch", "clock"],
    "progress": ["left_ptr_watch", "half-busy", "3ecb610c1bf2410f44200f48c40d3599", "00000000000000020006000e7e9ffc3f"],
    "pointer": ["hand", "hand1", "hand2", "pointing_hand", "9d800788f1b08800ae810202380a0822", "e29285e634086352946a0e7090d73106"],
    "text": ["xterm", "ibeam", "vertical-text"],
    "move": ["fleur", "all-scroll", "size_all", "grabbing", "closedhand", "4498f0e0c1937ffe01fd06f973665830",
             "9081237383d90e509aa00f00170e968f"],
    "ns-resize": ["n-resize", "s-resize", "row-resize", "sb_v_double_arrow", "v_double_arrow", "size_ver", "top_side",
                  "bottom_side", "split_v", "00008160000006810000408080010102"],
    "ew-resize": ["e-resize", "w-resize", "col-resize", "sb_h_double_arrow", "h_double_arrow", "size_hor", "left_side",
                  "right_side", "split_h", "028006030e0e7ebffc7f7070c0600140"],
    "nwse-resize": ["nw-resize", "se-resize", "top_left_corner", "bottom_right_corner", "size_fdiag", "bd_double_arrow",
                    "c7088f0f3e6c8088236ef8e1e3e70000"],
    "nesw-resize": ["ne-resize", "sw-resize", "top_right_corner", "bottom_left_corner", "size_bdiag", "fd_double_arrow",
                    "fcf1c3c7cd4491d801f1e1c78f100000"],
    "not-allowed": ["no-drop", "crossed_circle", "forbidden", "circle", "03b6e0fcb3499374a867c041f52298f0"],
    "crosshair": ["cross", "tcross", "cross_reverse", "diamond_cross"],
}


def xcursor(images):
    """An Xcursor file: images [(nominal size, RGBA image, hotspot, delay ms)]."""
    XCURSOR_IMAGE = 0xFFFD0002
    head = struct.pack("<4sIII", b"Xcur", 16, 0x10000, len(images))
    toc_len = 12 * len(images)
    pos = 16 + toc_len
    toc, chunks = b"", b""
    for nominal, img, hot, delay in images:
        w, h = img.size
        px = bytearray()
        raw = img.convert("RGBA").tobytes()
        for i in range(0, len(raw), 4):   # premultiplied ARGB, little-endian
            r, g, b, a = raw[i], raw[i + 1], raw[i + 2], raw[i + 3]
            px += struct.pack("<I", (a << 24) | ((r * a // 255) << 16) | ((g * a // 255) << 8) | (b * a // 255))
        chunk = struct.pack("<IIIIIIIII", 36, XCURSOR_IMAGE, nominal, 1, w, h, hot[0], hot[1], delay) + bytes(px)
        toc += struct.pack("<III", XCURSOR_IMAGE, nominal, pos + len(chunks))
        chunks += chunk
    return head + toc + chunks


def write_theme(out, sizes):
    cur = os.path.join(out, "cursors")
    os.makedirs(cur, exist_ok=True)
    for name, _label, fn in CURSORS:
        images = []
        for s in sizes:
            frames = FRAMES if name in ANIMATED else 1
            for f in range(frames):
                img, hot = render(fn, s, f)
                images.append((s, img, hot, 120 if frames > 1 else 0))
        with open(os.path.join(cur, name), "wb") as fh:
            fh.write(xcursor(images))
        for alias in ALIASES.get(name, []):
            link = os.path.join(cur, alias)
            if os.path.lexists(link):
                os.remove(link)
            os.symlink(name, link)
    if not os.path.exists(os.path.join(out, "index.theme")):   # (the icons' has one already: the theme is both)
        with open(os.path.join(out, "index.theme"), "w") as fh:
            fh.write("[Icon Theme]\nName=Objectspace\nComment=Objectspace's pointers (after OS/2's)\nInherits=Adwaita\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?")
    ap.add_argument("--sizes", default="24,32,48,64")
    ap.add_argument("--sheet", action="store_true")
    ap.add_argument("--theme", metavar="THEMEDIR")
    a = ap.parse_args()
    sizes = [int(s) for s in a.sizes.split(",")]
    if a.theme:
        write_theme(a.theme, sizes)
        return
    os.makedirs(a.out, exist_ok=True)
    hot = []
    for name, _label, fn in CURSORS:
        for s in sizes:
            for f in range(FRAMES if name in ANIMATED else 1):
                img, h = render(fn, s, f)
                img.save(os.path.join(a.out, f"{name}-{s}" + (f"-f{f}" if name in ANIMATED else "") + ".png"))
            hot.append(f"{name} {s} {h[0]} {h[1]}")
    with open(os.path.join(a.out, "hotspots.txt"), "w") as f:
        f.write("\n".join(hot) + "\n")
    if a.sheet:
        sheet(a.out, sizes)


if __name__ == "__main__":
    main()
