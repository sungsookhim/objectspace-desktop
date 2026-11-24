#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""Draws the Trash Can's two default icons (data/icons/trash-empty.png,
trash-full.png), 128 pixels, in the flat, outlined style of Objectspace's folders.
Run from the tree's top: python3 tools/icons/make_trash.py (needs Pillow)."""
from PIL import Image, ImageDraw

S = 4                     # drawn 4x, then scaled down (smooth edges)
W = 128 * S
INK = (60, 60, 64, 255)
BODY, BODY_D, BODY_L = (176, 180, 188, 255), (140, 144, 152, 255), (214, 217, 222, 255)
PAPER, PAPER_D = (250, 250, 244, 255), (205, 205, 196, 255)

def p(*v): return [x * S for x in v]

def can(d, full):
    # body: a tapered can with ribs
    top, bot = 46, 118
    d.polygon(p(30, top, 98, top, 92, bot, 36, bot), fill=BODY, outline=INK, width=3 * S)
    for i, x in enumerate((44, 54, 64, 74, 84)):
        xb = 64 + (x - 64) * 0.82
        d.line(p(x, top + 8, xb, bot - 6), fill=BODY_D if i % 2 else BODY_L, width=4 * S)
    d.line(p(31, top + 5, 97, top + 5), fill=INK, width=2 * S)
    if full:
        # paper heaped above the rim, the lid tipped back on it
        d.polygon(p(36, 46, 42, 26, 56, 32, 62, 18, 76, 28, 88, 22, 94, 46), fill=PAPER, outline=INK, width=3 * S)
        d.line(p(46, 40, 54, 34), fill=PAPER_D, width=2 * S)
        d.line(p(66, 30, 74, 38), fill=PAPER_D, width=2 * S)
        d.line(p(82, 32, 86, 40), fill=PAPER_D, width=2 * S)
        lid = [(70, 4), (112, 22), (108, 30), (66, 12)]
        d.polygon([(x * S, y * S) for x, y in lid], fill=BODY_L, outline=INK, width=3 * S)
        d.line(p(86, 8, 92, 4, 98, 7), fill=INK, width=3 * S)
    else:
        d.rounded_rectangle(p(24, 36, 104, 46), radius=3 * S, fill=BODY_L, outline=INK, width=3 * S)
        d.rounded_rectangle(p(54, 26, 74, 36), radius=3 * S, fill=BODY, outline=INK, width=3 * S)

for name, full in (("trash-empty", False), ("trash-full", True)):
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    can(ImageDraw.Draw(im), full)
    im.resize((128, 128), Image.LANCZOS).save(f"data/icons/{name}.png")
