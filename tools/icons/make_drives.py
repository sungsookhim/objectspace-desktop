#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""Draws the drive icons (data/icons/drive-KIND.png and drive-KIND-off.png:
mounted, a green lamp; not mounted, grey), the Computer's
(computer.png) and Programs' (programs.png), 128 pixels, in the style of make_trash.py.
Run from the tree's top: python3 tools/icons/make_drives.py (needs Pillow)."""
from PIL import Image, ImageDraw

S = 4
W = 128 * S
INK = (60, 60, 64, 255)
GREY, GREY_D, GREY_L = (176, 180, 188, 255), (140, 144, 152, 255), (214, 217, 222, 255)
BLUE, BLUE_D = (70, 110, 190, 255), (45, 75, 140, 255)
ON, OFF = (60, 200, 80, 255), (120, 124, 130, 255)


def p(*v):
    return [x * S for x in v]


def lamp(d, x, y, on):
    d.ellipse(p(x - 5, y - 5, x + 5, y + 5), fill=ON if on else OFF, outline=INK, width=2 * S)


def disk(d, on):   # a hard disk: a flat box seen from the front
    d.rounded_rectangle(p(14, 46, 114, 92), radius=6 * S, fill=GREY, outline=INK, width=3 * S)
    d.line(p(18, 58, 110, 58), fill=GREY_L, width=3 * S)
    for x in range(24, 70, 8):
        d.line(p(x, 70, x, 84), fill=GREY_D, width=3 * S)
    lamp(d, 98, 78, on)


def root(d, on):   # the system disk: a disk with a slash on it
    disk(d, on)
    d.line(p(80, 66, 72, 86), fill=BLUE_D, width=4 * S)


def usb(d, on):    # a stick: body and plug
    d.rounded_rectangle(p(16, 44, 92, 84), radius=8 * S, fill=BLUE, outline=INK, width=3 * S)
    d.rectangle(p(92, 52, 118, 76), fill=GREY_L, outline=INK, width=3 * S)
    d.rectangle(p(100, 58, 106, 62), fill=INK)
    d.rectangle(p(100, 66, 106, 70), fill=INK)
    d.line(p(26, 52, 70, 52), fill=(120, 150, 215, 255), width=3 * S)
    lamp(d, 30, 72, on)


def optical(d, on):   # a disc
    d.ellipse(p(20, 16, 108, 104), fill=GREY_L, outline=INK, width=3 * S)
    d.arc(p(30, 26, 98, 94), 200, 290, fill=(150, 170, 220, 255), width=5 * S)
    d.arc(p(30, 26, 98, 94), 20, 110, fill=(220, 170, 200, 255), width=5 * S)
    d.ellipse(p(52, 48, 76, 72), fill=GREY, outline=INK, width=3 * S)
    d.ellipse(p(60, 56, 68, 64), fill=(255, 255, 255, 0), outline=INK, width=2 * S)
    lamp(d, 102, 102, on)


def sd(d, on):     # a card with a cut corner
    d.polygon(p(36, 18, 84, 18, 96, 30, 96, 110, 36, 110), fill=BLUE, outline=INK, width=3 * S)
    for x in range(44, 88, 9):
        d.rectangle(p(x, 24, x + 5, 38), fill=(230, 200, 80, 255), outline=INK, width=1 * S)
    d.rectangle(p(44, 60, 88, 92), fill=GREY_L, outline=INK, width=2 * S)
    lamp(d, 80, 102, on)


def computer(d):   # a monitor on a stand
    d.rounded_rectangle(p(14, 18, 114, 86), radius=5 * S, fill=GREY, outline=INK, width=3 * S)
    d.rectangle(p(22, 26, 106, 76), fill=(90, 140, 210, 255), outline=INK, width=2 * S)
    d.polygon(p(54, 86, 74, 86, 80, 104, 48, 104), fill=GREY_D, outline=INK, width=3 * S)
    d.rounded_rectangle(p(34, 102, 94, 112), radius=3 * S, fill=GREY, outline=INK, width=3 * S)


def programs(d):   # a folder holding program tiles
    FOLD, FOLD_D = (240, 200, 90, 255), (200, 160, 60, 255)
    d.polygon(p(12, 30, 48, 30, 56, 40, 116, 40, 116, 106, 12, 106), fill=FOLD_D, outline=INK, width=3 * S)
    d.rectangle(p(12, 48, 116, 106), fill=FOLD, outline=INK, width=3 * S)
    cols = [(220, 80, 70, 255), (70, 150, 220, 255), (90, 180, 90, 255), (150, 100, 200, 255)]
    for k, (x, y) in enumerate(((34, 58), (66, 58), (34, 82), (66, 82))):
        d.rounded_rectangle(p(x, y, x + 26, y + 18), radius=3 * S, fill=cols[k], outline=INK, width=2 * S)


def save(name, draw):
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    draw(ImageDraw.Draw(im))
    im.resize((128, 128), Image.LANCZOS).save(f"data/icons/{name}.png")


for kind, f in (("disk", disk), ("root", root), ("usb", usb), ("optical", optical), ("sd", sd)):
    save(f"drive-{kind}", lambda d, f=f: f(d, True))
    save(f"drive-{kind}-off", lambda d, f=f: f(d, False))
save("computer", computer)
save("programs", programs)
