#!/usr/bin/env python3
"""WPS's icon themes as packages (roadmap 0.4): every icon of the isometric
set (tools/icons/make_iso.py, sets A to E) drawn at each size and packed as
NAME.wpsicon, one theme per variant:

  OUTDIR/wps-flat/      the default (the owner's, 2026-11-22)
  OUTDIR/wps-outline/
each with theme.ini (its name and what it is) and NAME.wpsicon packages: a
zip of SIZE.png, stored, no manifest (gui/shell/wpsicon.h). The sizes: the
standard five (256, 128, 96, 64, 32) and the small designs 48, 40 and 16,
each drawn on its own (--sizes to change them).

The classic theme (0.2's pictures) is packed from data/icons by
`wpsctl icon pack` (the Makefile's `icons`).

Run from the tree's top (needs Pillow):
  python3 tools/icons/make_theme.py OUTDIR [--variant flat|outline|all] [--sizes ...]"""
import argparse
import importlib.util
import io
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = "256,128,96,64,48,40,32,16"
THEMES = {
    "flat": ("wps-flat", "WPS (flat)", "The isometric set without outlines (the default)"),
    "outline": ("wps-outline", "WPS (outline)", "The isometric set with a dark outline round each shape"),
}


def load_iso():
    spec = importlib.util.spec_from_file_location("make_iso", os.path.join(HERE, "make_iso.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def pack(path, pngs):
    """A .wpsicon: SIZE.png entries, largest first, stored, a fixed time
    (the same pictures make the same file)."""
    tmp = path + ".new"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_STORED) as z:
        for size in sorted(pngs, reverse=True):
            info = zipfile.ZipInfo(f"{size}.png", date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            z.writestr(info, pngs[size])
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--variant", default="all", choices=["all"] + sorted(THEMES))
    ap.add_argument("--sizes", default=SIZES)
    a = ap.parse_args()
    sizes = [int(s) for s in a.sizes.split(",")]
    iso = load_iso()
    variants = sorted(THEMES) if a.variant == "all" else [a.variant]
    for variant in variants:
        dirname, title, comment = THEMES[variant]
        out = os.path.join(a.out, dirname)
        os.makedirs(out, exist_ok=True)
        names = []
        for key in sorted(iso.SETS):
            for name, _, fn in iso.SETS[key]:
                pngs = {}
                for s in sizes:
                    buf = io.BytesIO()
                    iso.render(fn, s, variant).save(buf, "PNG", optimize=True)
                    pngs[s] = buf.getvalue()
                pack(os.path.join(out, name + ".wpsicon"), pngs)
                names.append(name)
        with open(os.path.join(out, "theme.ini"), "w") as f:
            f.write(f"[Icon Theme]\nName={title}\nComment={comment}\nVariant={variant}\n"
                    f"Sizes={','.join(str(s) for s in sizes)}\nIcons={len(names)}\n")
        print(f"{out}: {len(names)} icons, sizes {a.sizes}", file=sys.stderr)


if __name__ == "__main__":
    main()
