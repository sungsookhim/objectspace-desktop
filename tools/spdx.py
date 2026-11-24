#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""spdx - the licence and copyright lines at the top of every source file (0.5 step 5).

  python3 tools/spdx.py           check: list the files without them (exit 1 if any)
  python3 tools/spdx.py --add     add them where they are missing

Files are the ones git tracks. Each kind gets its own comment style; the
lines go first, after a #! line or a first line a program needs (PAM's
#%PAM-1.0, <?xml ...?>). Files that take no comments (pictures, JSON, Markdown, the
golden lists) are covered by LICENSE and NOTICE instead."""
import subprocess
import sys

LICENSE = "SPDX-License-Identifier: MIT"
HOLDER = "SPDX-FileCopyrightText: 2026 Sungsoo Khim"

C_STYLE = ("/* ", " */")
HASH = ("# ", "")
ROFF = ('.\\" ', "")
JS = ("// ", "")
XML = ("<!-- ", " -->")

BY_SUFFIX = {
    ".c": C_STYLE, ".h": C_STYLE, ".js": JS,
    ".sh": HASH, ".py": HASH, ".osdi": HASH, ".in": HASH, ".pam": HASH,
    ".service": HASH, ".supp": HASH, ".1": ROFF, ".xml": XML,
}
BY_NAME = {"Makefile": HASH, "osd-session": HASH}
KEEP_FIRST = ("#!", "#%PAM", "<?xml")


def style(path):
    name = path.rsplit("/", 1)[-1]
    if name in BY_NAME:
        return BY_NAME[name]
    dot = name.rfind(".")
    return BY_SUFFIX.get(name[dot:]) if dot > 0 else None


def main():
    add = "--add" in sys.argv[1:]
    files = subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True).stdout.split()
    missing = []
    for f in files:
        st = style(f)
        if not st:
            continue
        with open(f, encoding="utf-8") as fh:
            text = fh.read()
        if LICENSE in text[:600]:
            continue
        missing.append(f)
        if not add:
            continue
        lines = text.splitlines(keepends=True)
        head = [lines.pop(0)] if lines and lines[0].startswith(KEEP_FIRST) else []
        new = [st[0] + LICENSE + st[1] + "\n", st[0] + HOLDER + st[1] + "\n"]
        with open(f, "w", encoding="utf-8") as fh:
            fh.write("".join(head + new + lines))
    for f in missing:
        print(("added: " if add else "no licence line: ") + f)
    if not add and missing:
        return 1
    print(f"spdx: {len(files)} files tracked, {len(missing)} {'given the lines' if add else 'without the lines'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
