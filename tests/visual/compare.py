#!/usr/bin/env python3
"""Compares two sets of shots from tests/visual/run.sh, pixel by pixel.

    tests/visual/compare.py BASE NEW [DIFFS]

The WarpCenter's clock and meters (the right of the bottom bar) change by
themselves and are left out. A shot that differs is listed with its count of
differing pixels and their bounding box; with DIFFS, a picture of each
difference is saved there (the new shot, the differing pixels in magenta).
Exit status 1 if any shot differs or is missing.
"""
import os
import sys

from PIL import Image, ImageChops

# (x0, y0, x1, y1) on the 1280x800 screen: the WarpCenter's tray and clock
MASKS = [(1080, 776, 1280, 800)]


def masked(im):
    im = im.convert("RGB")
    for box in MASKS:
        im.paste((0, 0, 0), box)
    return im


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 2
    base, new = sys.argv[1], sys.argv[2]
    diffs = sys.argv[3] if len(sys.argv) > 3 else None
    if diffs:
        os.makedirs(diffs, exist_ok=True)
    names = sorted(f for f in os.listdir(base) if f.endswith(".png"))
    bad = 0
    for n in names:
        p = os.path.join(new, n)
        if not os.path.exists(p):
            print(f"MISSING {n}")
            bad += 1
            continue
        a, b = masked(Image.open(os.path.join(base, n))), masked(Image.open(p))
        if a.size != b.size:
            print(f"DIFFER  {n}: size {a.size} -> {b.size}")
            bad += 1
            continue
        d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v else 0)
        box = d.getbbox()
        if not box:
            print(f"same    {n}")
            continue
        count = sum(1 for v in d.getdata() if v)
        print(f"DIFFER  {n}: {count} pixels in {box}")
        bad += 1
        if diffs:
            out = b.copy()
            out.paste((255, 0, 255), mask=d)
            out.save(os.path.join(diffs, n))
    print(f"{len(names) - bad} same, {bad} differ or missing")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
