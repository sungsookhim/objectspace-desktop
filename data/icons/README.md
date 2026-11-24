# Where Objectspace's pictures come from

Every picture Objectspace ships was made for this project, by its scripts in `tools/icons` (Python with Pillow) or by hand. None is copied from OS/2 or from another project. All are under the MIT License, as the code is (`LICENSE`, `NOTICE`).

| Pictures | Made by | Shipped as |
|---|---|---|
| `trash-empty.png`, `trash-full.png` | `tools/icons/make_trash.py` (0.2) | these files, and the Classic theme |
| `drive-*.png` (mounted and `-off`), `computer.png`, `programs.png` | `tools/icons/make_drives.py` (0.2) | these files, and the Classic theme |
| The isometric set: System Setup objects, special folders, data files, the basic kinds and Objectspace's own icons redrawn (sets A to E) | `tools/icons/make_iso.py` (0.4) | not in the repository: drawn at build time by `tools/icons/make_theme.py` (`make icons`) into the Objectspace (flat) and Objectspace Outline themes |
| The Classic theme | the pictures above, packed by `osdctl icon pack` (`make icons`) | `build/icon-themes/classic` |
| `docs/mockups/0.3-search-field.png` | drawn for the 0.3 plan | documentation only |

To draw them again, from the tree's top:

```sh
python3 tools/icons/make_trash.py
python3 tools/icons/make_drives.py
make icons
```

The pictures in this folder are checked in so that a build needs no Pillow for them. The themes do need Pillow (`python3-pillow`).

**From the system, not shipped:** icons for programs and for file types Objectspace has no picture for come from the icon themes installed on the system (Adwaita by default, the RPM's dependency) or from the programs' own `.desktop` files. They are read at run time and never copied.

**Sounds:** Objectspace ships no sounds. The action events play files the user chooses.
