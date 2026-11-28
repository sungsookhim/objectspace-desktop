#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""Objectspace's icons as a freedesktop.org icon theme (0.7 step 9), so other
programs' file dialogs and file managers show the same pictures: folders,
places, drives and file types, under the names those programs ask for.
Everything else (programs' own icons, symbolic icons) comes from the themes
it inherits (Adwaita, hicolor).

  python3 tools/icons/make_xdg_icons.py ICONTHEMEDIR OUTDIR
ICONTHEMEDIR: an Objectspace icon theme (build/icon-themes/objectspace-flat:
.osdicon packages, a zip of SIZE.png each); OUTDIR: the theme
(OUTDIR/index.theme, OUTDIR/SIZExSIZE/CONTEXT/NAME.png)."""
import os
import sys
import zipfile

# context -> {freedesktop name: Objectspace icon}
MAP = {
    "places": {
        "folder": "folder", "folder-open": "folder-open", "folder-drag-accept": "folder-open",
        "user-home": "home", "folder-home": "home", "user-trash": "trash-empty", "user-trash-full": "trash-full",
        "folder-templates": "templates", "user-bookmarks": "startup",
        "network-workgroup": "network", "folder-remote": "drive-net", "network-server": "drive-net",
    },
    "devices": {
        "computer": "computer", "drive-harddisk": "drive-disk", "drive-harddisk-system": "drive-root",
        "drive-optical": "drive-optical", "media-optical": "drive-optical", "drive-removable-media": "drive-usb",
        "drive-removable-media-usb": "drive-usb", "media-removable": "drive-usb", "media-flash": "drive-sd",
        "media-flash-sd-mmc": "drive-sd",
    },
    "mimetypes": {
        "inode-directory": "folder", "text-x-generic": "file-text", "text-plain": "file-text",
        "text-x-script": "file-script", "application-x-shellscript": "file-script",
        "text-x-csrc": "file-code", "text-x-chdr": "file-code", "text-x-python": "file-code", "text-html": "file-code",
        "text-x-source": "file-code", "application-json": "file-code", "application-xml": "file-code",
        "image-x-generic": "file-image", "audio-x-generic": "file-audio", "video-x-generic": "file-video",
        "font-x-generic": "file-font", "package-x-generic": "file-package", "application-x-rpm": "file-package",
        "x-office-document": "file-document", "application-vnd.oasis.opendocument.text": "file-document",
        "application-msword": "file-document", "application-pdf": "file-document",
        "x-office-spreadsheet": "file-spreadsheet", "application-vnd.oasis.opendocument.spreadsheet": "file-spreadsheet",
        "x-office-presentation": "file-presentation", "application-vnd.oasis.opendocument.presentation": "file-presentation",
        "application-x-generic": "file-data", "application-octet-stream": "file-data",
        "application-x-executable": "program", "application-x-sharedlib": "file-data",
        "application-x-archive": "file-archive", "application-zip": "file-archive", "application-x-tar": "file-archive",
        "application-x-compressed-tar": "file-archive", "application-x-7z-compressed": "file-archive",
        "application-x-cd-image": "file-disc-image",
    },
}


def main():
    src, out = sys.argv[1], sys.argv[2]
    dirs = set()
    for ctx, names in MAP.items():
        for name, ours in names.items():
            with zipfile.ZipFile(os.path.join(src, ours + ".osdicon")) as z:
                for entry in z.namelist():
                    size = int(entry.split(".")[0])
                    d = f"{size}x{size}/{ctx}"
                    dirs.add((size, ctx))
                    os.makedirs(os.path.join(out, d), exist_ok=True)
                    with open(os.path.join(out, d, name + ".png"), "wb") as f:
                        f.write(z.read(entry))
    order = sorted(dirs)
    with open(os.path.join(out, "index.theme"), "w") as f:
        f.write("[Icon Theme]\nName=Objectspace\nComment=Objectspace's icons (folders, places, drives, file types)"
                " and pointers\nInherits=Adwaita,hicolor\nExample=folder\n")
        f.write("Directories=" + ",".join(f"{s}x{s}/{c}" for s, c in order) + "\n")
        for s, c in order:
            ctxname = {"places": "Places", "devices": "Devices", "mimetypes": "MimeTypes"}[c]
            f.write(f"\n[{s}x{s}/{c}]\nSize={s}\nContext={ctxname}\nType=Fixed\n")


if __name__ == "__main__":
    main()
