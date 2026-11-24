#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""rename-0.5: WPS's names to Objectspace's (docs/v0.5-names.md, docs/v0.5-plan.md).

    tools/rename-0.5.py STAGE [--dry]    apply a stage's rules to the tree
    tools/rename-0.5.py --check          every old name left, by file

Stages (the plan's steps):
    code      step 2: classes WP* -> Osd*, methods wp*/wpcls* -> osd*/osdcls*,
              the runtime wpo -> osdo, wps_/WPS_ -> osd_/OSD_, .wpi -> .osdi
    external  step 3: commands, packages, units, app IDs, folders, attributes,
              file types (what other programs and the disk see)
    ui        step 4: TheBar, Launcher, Objectspace Terminal, the product name

Each rule is a regular expression on whole identifiers (not inside other
words); the same rules rename the files and folders they match (git mv).
History is never touched: docs/ (the plans, notes, reviews and release
notes of WPS, 0.1 to 0.4.1, and 0.5's own lists of the old names), the
binary pictures, and this tool.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Not renamed: the record of what was (docs/), pictures, this tool.
SKIP_DIRS = ("docs/", "build/")
SKIP_FILES = {"tools/rename-0.5.py", "tests/test_rename.py"}
BINARY = re.compile(r"\.(png|jpg|jxl|ico|icns|wpsicon|osdicon|gz|zip|ogg|oga|wav|ttf|otf)$")

B = r"(?<![A-Za-z0-9])"   # start of an identifier ('_' may precede: Class_method)
E = r"(?![A-Za-z0-9])"    # its end

STAGES = {
    "code": [
        # the runtime: wpo_*, WPO_*, WPORoot, WPOClass, wpo.h; the class compiler wpoc
        (B + r"wpoc" + E, "osdc"),
        (B + r"wpo" + E, "osdo"),
        (B + r"wpo_", "osdo_"),
        (B + r"WPO_", "OSDO_"),
        (B + r"WPO(?=[A-Z][a-z])", "Osdo"),
        # class interface files: .wpi, WpiError, _WPI_H
        (B + r"wpi" + E, "osdi"),
        (r"(?<=[A-Za-z0-9_])\.wpi" + E, ".osdi"),
        (B + r"Wpi(?=[A-Z])", "Osdi"),
        (r"(?<=_)WPI_H", "OSDI_H"),
        (B + r"WPI" + E, "OSDI"),
        # classes WPObject ... (and M_WPFolder, WPFolderClass, WPFolder_data)
        (B + r"WP(?=[A-Z][a-z])", "Osd"),
        # methods wpQueryTitle, wpclsNew (and Class_wpQueryTitle)
        (B + r"wp(cls)?(?=[A-Z])", r"osd\1"),
        (B + r"wpcls" + E, "osdcls"),
        (B + r"osdimagefile" + E, "osdimagefile"),   # (the SDK sample's plug-in, its class in lower case)
        # C names: wps_*, WPS_* (macros, header guards, menu IDs, the environment)
        (B + r"wps_", "osd_"),
        (B + r"WPS_", "OSD_"),
        (r"(?<=_)WPS_", "OSD_"),
        (r"(?<=-D)WPS_", "OSD_"),   # (compiler flags: -DOSD_VERSION)
        (r"(?<=_)wps" + E, "osd"),
        (r"(?<=_)wpsd" + E, "osdd"),
    ],
    "external": [
        # the repository, the GNOME extension, app IDs, MIME types, attributes
        (B + r"wps@wps-linux" + E, "objectspace@sungsookhim.github.io"),
        (r"sungsookhim/wps-linux", "sungsookhim/objectspace-desktop"),
        (B + r"wps-linux" + E, "objectspace-desktop"),
        (r"org\.gnome\.shell\.extensions\.wps" + E, "org.gnome.shell.extensions.objectspace"),
        (B + r"org\.wps\.", "io.github.sungsookhim.Objectspace."),
        (B + r"user\.wps\.", "user.osd."),
        (B + r"x-wps-", "x-objectspace-"),
        (B + r"WpsExtension" + E, "ObjectspaceExtension"),
        # file types: icon packages, abstract objects as files
        (B + r"wpsicon", "osdicon"),
        (B + r"WPSICON", "OSDICON"),
        (B + r"wpobj" + E, "osdobj"),
        # commands (the long names are links: the Makefile makes them)
        (B + r"wpsctl" + E, "osdctl"),
        (B + r"WPSCTL" + E, "OSDCTL"),
        (B + r"wps-(shell|terminal|compositor|session|watchd|plughost|lock)" + E, r"osd-\1"),
        (B + r"wpsd" + E, "osdd"),
        (B + r"wpsd(?=_)", "osdd"),
        (B + r"WPSD" + E, "OSDD"),
        # packages
        (B + r"wps-(devel|gnome-shell-extension)" + E, r"objectspace-\1"),
        # files and folders: wps.sock, wps.desktop, wps.spec, 80-wps.preset, .../wps/...
        (B + r"wps\.(sock|desktop|spec|preset)" + E, r"objectspace.\1"),
        (r"(?<=-)wps\.preset", "objectspace.preset"),
        (r"(?<=/)wps(?=/|\"|'|\s|$|\)|\}|;|%|\\)", "objectspace"),
        (r"(?<=/\.config/)wps|(?<=/\.local/share/)wps|(?<=/\.local/state/)wps", "objectspace"),
        # the scripts' command itself (wps open, $(B)/wps): left for review by --check
    ],
    "ui": [
        (B + r"WPS Terminal" + E, "Objectspace Terminal"),
        (B + r"WPS \((flat|outline)\)", r"Objectspace (\1)"),
        (B + r"wps-(flat|outline)" + E, r"objectspace-\1"),   # (the icon themes' folders)
        (r"\b[Tt]he WarpCenter" + E, "TheBar"),   # (a name: no article)
        (B + r"WarpCenter" + E, "TheBar"),
        (B + r"WarpCenters" + E, "TheBars"),
        (B + r"warpcenter" + E, "thebar"),
        (B + r"warpcenter(?=_)", "thebar"),
        (B + r"WARPCENTER" + E, "THEBAR"),
        (r"(?<=_)WARPCENTER", "THEBAR"),
        (r"(?<=_)warpcenter", "thebar"),
        (B + r"LaunchPad" + E, "Launcher"),
        (B + r"LaunchPads" + E, "Launchers"),
        (B + r"launchpad" + E, "launcher"),
        (B + r"launchpad(?=_)", "launcher"),
        (B + r"LAUNCHPAD" + E, "LAUNCHER"),
        (r"(?<=_)LAUNCHPAD", "LAUNCHER"),
        (r"(?<=_)launchpad", "launcher"),
        (r"(?<=Osd)(WarpCenter)", "TheBar"),
        (r"(?<=Osd)(LaunchPad)", "Launcher"),
        (B + r"Workplace Shell" + E, "Objectspace"),
        # (inside longer names: LaunchPadWindow, launchpad2, ...)
        (r"LaunchPad", "Launcher"), (r"launchpad", "launcher"), (r"WarpCenter", "TheBar"), (r"warpcenter", "thebar"),
    ],
}

# step 4's last part: the words left (the product's name in text, the
# command in prose and logs, internal names with "wps-"); the README is
# written anew in step 6 and is left out here.
STAGES["words"] = [
    (B + r"MIME_WPS" + E, "MIME_OSD"),
    (B + r"(wpsclient|wpsession|wpsext4|wpsbtrfs|wpsbig|wpsd1)" + E, None),   # (below, one by one)
    (B + r"WPSFAT" + E, "OSDFAT"),
    (B + r"DIST\s*:= wps-", "DIST    := objectspace-"),
    (B + r"wps-(?=[a-z$(])", "osd-"),
    (B + r"WPS for Linux" + E, "Objectspace"),
    (B + r"WPS" + E, "Objectspace"),
    (B + r"wps" + E, "osd"),
]
STAGES["words"][1] = (B + r"wps(?=client|ext4|btrfs|big|d1)", "osd")
STAGES["words"].insert(1, (B + r"wpsession" + E, "osd_session"))
WORDS_SKIP = {"README.md"}

# Old names --check looks for (whole identifiers, or parts of compound ones).
OLD = re.compile(r"(?<![A-Za-z0-9])(?:WP[A-Z][a-z]\w*|wp(?:cls)?[A-Z]\w*|wpoc?|wpo_\w+|WPO\w*|wpi|wps\w*|WPS\w*|Wps\w*|"
                 r"WarpCenter|LaunchPad|warpcenter|launchpad|WARPCENTER|LAUNCHPAD|Workplace Shell)(?![A-Za-z0-9])"
                 r"|(?<=_)(?:wps|wpsd|WPS|warpcenter|launchpad)(?![a-z])")


def tracked():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split("\n")
    return [f for f in out if f and not f.startswith(SKIP_DIRS) and f not in SKIP_FILES]


def apply(text, rules):
    n = 0
    for pat, repl in rules:
        text, k = re.subn(pat, repl, text)
        n += k
    return text, n


def run_stage(stage, dry):
    rules = [(re.compile(p), r) for p, r in STAGES[stage]]
    changed, total = 0, 0
    for f in tracked():
        if stage == "words" and f in WORDS_SKIP:
            continue
        p = ROOT / f
        if not p.is_file() or BINARY.search(f):
            continue
        try:
            text = p.read_text()
        except UnicodeDecodeError:
            continue
        new, n = apply(text, rules)
        if n:
            changed += 1
            total += n
            if not dry:
                p.write_text(new)
    # then the files and folders the rules rename (deepest first)
    moves = []
    for f in tracked():
        nf, n = apply(f, rules)
        if n and nf != f:
            moves.append((f, nf))
    for f, nf in sorted(moves, key=lambda m: -m[0].count("/")):
        print(f"move {f} -> {nf}")
        if not dry:
            (ROOT / nf).parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(["git", "mv", f, nf], cwd=ROOT, check=True)
    print(f"{stage}: {total} changes in {changed} files, {len(moves)} files moved{' (dry run)' if dry else ''}")


EMBEDDED = re.compile(r"LaunchPad|launchpad|LAUNCHPAD|WarpCenter|warpcenter|WARPCENTER|WPS|(?<![a-z])wps")


def check():
    left = {}
    for f in tracked():
        p = ROOT / f
        names = OLD.findall(f)
        if p.is_file() and not BINARY.search(f):
            try:
                names += OLD.findall(p.read_text())
            except UnicodeDecodeError:
                pass
            try:
                names += ["(inside) " + m for m in EMBEDDED.findall(p.read_text())]
            except UnicodeDecodeError:
                pass
        for nm in names:
            left.setdefault(nm, set()).add(f)
    for nm, files in sorted(left.items(), key=lambda x: -len(x[1])):
        print(f"{nm:32s} {len(files):4d} files  e.g. {sorted(files)[0]}")
    print(f"{len(left)} old names left")
    return 1 if left else 0


def main():
    args = sys.argv[1:]
    if args == ["--check"]:
        sys.exit(check())
    if not args or args[0] not in STAGES:
        sys.exit(__doc__)
    run_stage(args[0], "--dry" in args)


if __name__ == "__main__":
    main()
