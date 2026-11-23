# Objects all the way down: properties, styles, events, plug-ins

2026-11-22, on branch `refactor/object-model`. Approved by the owner; stages R0-R6 in progress.

## The owner's directives

1. **Every property, current and future, of every object is reachable through the object design**: folders, programs, data files and abstract objects alike.
2. **Global and individual settings for everything**, from the file structure to the looks: set once for all, changed for one.
3. **OOP for looks and behaviour:** colours, styles and fonts, and the actions taken on events, are inherited and overridden like methods.
4. **Users can add their own plug-ins.**
5. **The WPS concept laid over Linux's own structures**, as WPS was laid over OS/2's, at least from the GUI.
6. **Existing features keep working.** Reimplementing them is acceptable.

## 1. Before: how it works today

The core is sound: `runtime/` is a SOM-like object runtime (single inheritance, metaclasses, calls by name with signatures, class replacement, plug-ins). State is per class in `user.wps.<Class>` xattrs with a per-user SQLite overlay, and the tracker keeps identity across moves. The problem is that **properties are not part of the object model**. They are a convention spread over four layers.

### 1.1 A property is five to seven edits in four places

Adding a folder's "icon font" took:

1. The class's C getter and setter, with a default (`classes/WPFolder.c`).
2. A `field` line in the text returned by `wpAddSettingsPages`. The `.wpi` declares no properties.
3. Sometimes wpsd: setters `@`, `@col:`, `@lock` and `@assoc`, and the pages wpsd injects (`service/wpsd.c:1308-1410`).
4. The GUI reads it **by parsing the settings notebook's text** (`got_bg`, `gui/shell/wps-shell.c:302-345`). There is no property-read API.
5. A global for the system-wide version (`folders_font`, `titles_font`, `show_ext`...) set from an if-chain over setting keys (`apply_setup_object`, wps-shell.c:6483).
6. Its own fallback logic: folder, then System, then the interface font (`folder_font`, wps-shell.c:365).
7. For frames and menus, a text sync to the compositor (`winlist_ext "frame" ...`).

### 1.2 "Global" and "individual" are different mechanisms

| Property | Individual | Global | Resolved |
|---|---|---|---|
| Folder background colour | `WPFolder/bg_color` | the scheme's `client_bg` (no setting) | in the GUI |
| Icon font | `WPFolder/icon_font` | `WPSystem font_folders` | in the GUI, its own chain |
| Title font | `WPFolder/title_font` | `WPSystem font_titles` | toolkit static, per-surface flag |
| Menu font | none | `WPSystem font_menus` | toolkit static; the compositor's menus never see it |
| LaunchPad font | none | none | fixed |
| Show extensions | none | `WPSystem show_ext` | GUI global; wpsd never sees it |
| Sounds | none | the `WPSound` table | by event name only |

No level exists between "this object" and "the whole system". "All folders", "all image files" or "all programs" can't be expressed. Defaults are written twice: in the classes and again in the GUI (`bg_mode`, `bg_gnome`, the sound table).

### 1.3 Looks are not objects

- **Schemes** are three static C structs (`gui/ui_draw.c:23-70`). The Scheme Palette stores only a name.
- **Colours are hardcoded** outside the scheme in about a dozen places: icon art, desktop titles, scrollbar trough, WarpCenter, LaunchPad, tooltip, urgent colours, the desktop's teal.
- **Sizes are hardcoded:** `ICON 32`, `UI_CELL_W 100`, menu rows 22, LaunchPad and WarpCenter sizes.
- **The shell and the compositor each keep their own scheme, fonts and frame options**, synced by text commands, with the menu font missing. The compositor has a second menu implementation.

### 1.4 Events can only play a sound

- `events.c` has a bus (`events_raise`) with at most 8 subscribers. The only real subscriber is the sound player.
- The binding is a flat name-to-sound list, filled from defaults, then `sounds.conf`, then gsettings, then WPSound (which replaces all of them).
- No per-object or per-class actions. Nothing can run a program, call a method or reach a plug-in on an event.

### 1.5 Plug-ins are half connected

- **What works:**
  - sandboxed host processes (namespaces, Landlock, deadlines, restarts);
  - classes, methods and overrides, class replacement;
  - version checks.
- **What's missing:**
  - **Only `call` and `menu` reach the plug-in.** Settings pages, setters and menu-item handlers run in wpsd against a proxy, so a plug-in's pages and handlers are never called.
  - No SDK is installed: no headers, `.wpi` files or `wpoc.py`.
  - No events, no properties, no GUI part.
  - MIME types don't choose classes (planned in `object-core-design.md:173`, not built).

### 1.6 Linux structures are reached ad hoc

Each mapping is its own code:

- **Desktop entries:** a program's title and command are mirrored into its `.desktop` file (`objects.c`), and `@assoc` writes the user's copy (`launch.c`).
- **mimeapps.list:** written by `wps_app_set_default`.
- **gsettings:** two-way sync in `gnome.c`, plus separate readers in `wallpaper.c`, `events.c` and `keymap.c`, by spawning `gsettings`. There are three copies of the reader.
- **autostart:** `startup.c`.
- **Config files next to the objects:** `sounds.conf`, `mouse.conf` and `system.conf` are read directly by the GUI. Config-path logic is copied three times.

### 1.7 Size and shape of the code

| File | Lines | Holds |
|---|---|---|
| `gui/shell/wps-shell.c` | 7,226 | desktop, folder windows, tree, layout, notebooks, menus, drag and drop, jobs, settings objects, login restore, and about 41 self-test scenarios (1,900 lines) |
| `service/wpsd.c` | 4,665 | every protocol op in one `handle()` if-chain (about 60 ops), settings text and setters, setup objects, events |
| `compositor/wps-compositor.c` | 2,725 | includes a second menu implementation |
| `classes/` (all) | 1,568 C + `.wpi` | small, and the right shape |

**Tests today:**
- `make test` passes: unit tests plus the drives, programs, messages, first-start and Open With scenarios.
- `make gui-test` passes: 310 + 11 + 18 + 25 + 12 checks.
- `make check` has failed since M4 stage 1 (`6847e54`): the checked-call `abort()` is outside the core's allowed C library subset.
- Stray empty `Sys2b/` and `Progs2/` folders appear at the top of the repo, probably a test writing into the working directory.

## 2. After: the design

One idea: **a property is a declared member of a class, like a method, and its value is found by walking a chain of objects.** Pages, setup strings, the GUI's looks, events and Linux mappings are all built from those declarations.

### 2.1 Properties declared in the class

`.wpi` gains `property`:

```
class WPFolder : WPFileSystem
  property icon_font  : font   = inherit   page "Icon view" label "Icon font"
  property bg_color   : colour = inherit   page "Background" label "Colour"
  property bg_image   : file   = ""        page "Background" label "Picture" hint image
  property sort_key   : choice(name,type,size,date) = name  page "Sort"
  property show_ext   : bool   = inherit   page "View" label "Show file extensions"
  event    open, close, drop
```

wpoc generates a table of **property descriptors**:
- name and type: text, int, bool, choice, colour, font, file, icon, size, action, list;
- default, or `inherit`;
- page, label and hint;
- flags (hidden, read-only, per-user, not copied);
- an optional **backing** (2.6).

The runtime gives every class `wpGetProperty(name)`, `wpSetProperty(name, value)` and `wpListProperties()`, which a class may override to validate a value or react to it. These replace the hand-written getters, setters and `field` lines.

- **Settings pages come from the descriptors.** `WPObject`'s `wpAddSettingsPages` builds them, so a class with properties gets its notebook for free. A class can still add or change pages, as today. The setters `@`, `@col:`, `@lock` and `@assoc` go away and become properties.
- **Storage doesn't change:** the same `user.wps.<Class>` xattr and DB overlay, with the same keys. Existing objects keep their settings.

### 2.2 Global and individual: the resolution chain

A property set to `inherit`, or never set, is looked up along a chain, nearest first:

| # | Level | Example | Where it's kept |
|---|---|---|---|
| 1 | **The object** | this folder's icon font | its xattr or the overlay, as today |
| 2 | **Its file type** (data files) | all `image/png`, then all `image/*` | a type object, `type:image/png` |
| 3 | **Its class, then each parent class** | all WPFolder, then all WPFileSystem, then all WPObject | the **class object's** settings (the metaclass data that exists but is unused) |
| 4 | **The style (scheme)** | the scheme's fonts, colours and sizes | the WPSchemePalette entry in use, now an object (2.3) |
| 5 | **The declared default** | from the `.wpi` | built in |

**This is the OOP model the directives ask for.** A class's settings are inherited by its subclasses, as methods are, and overridden per object. "Global" is simply a property set on a class object or the style, and "individual" is the same property set on one object.

- **The System notebook doesn't go away.** Its Fonts, View and Sounds pages now edit WPObject's or WPFolder's class settings, so they mean what they meant before.
- **New notebook page: Class defaults** (on any object's notebook, behind a button). It edits the object's class settings or its file type's, such as "all image files" or "all folders".
- **The Font Palette and Colour Palette follow OS/2:**
  - a drop sets the property on that object;
  - **Shift**+drop sets it on its class.
  - That's the same gesture as today's Shift drop, which sets it for everything.
- **wpsd resolves the chain, once and in one place.** A new `props` op returns each value with its **origin** (which level it came from), so a notebook can show "from: all folders" with a **Reset** button. A change at any level notifies the objects below it that are open. The GUI never resolves anything again.

**Proposed:** no "folder to its contents" level. OS/2 didn't have one, and it makes moving a file change its looks. It can be added later as level 1½.

### 2.3 Looks as objects

- **Every look is a property:**
  - colours: face, text, title bars, selection, background, the desktop's colour;
  - fonts: interface, icons, titles, menus, dialogs, WarpCenter, LaunchPad;
  - sizes: the icon set Small, Default or Large, borders, title height;
  - backgrounds;
  - the icon theme.
- **The hardcoded colours and sizes become declared properties** with today's values as their defaults.
- **A scheme is an object holding a set of property values**: level 4 of the chain. Classic, Dark and High contrast ship as objects, and users can copy and edit them. The Scheme Palette lists them.
- **One style object per window in the GUI:** the window's resolved properties, filled from `props` and kept current by its subscription. It replaces `ui_scheme_now`'s globals, `folders_font`, `titles_font`, `menu_font` and the other statics.
- **The compositor gets the same:** one `style` channel instead of the `scheme`, `font` and `frame` text commands, and it drops its second menu implementation in favour of the toolkit's.
- This also covers 0.4's icon work: the **icon set** and the **icon theme** are properties, so a folder can use its own.

### 2.4 Events and actions as properties

An **event** is declared on a class (`event open, close, drop, delete, ...`; the existing names are kept: `window.*`, `menu.*`, `object.*`, `job.*`, `alert.*`, `shell.*`). Each event has a property `on:EVENT` of type `action`, resolved through the same chain. So an action can be set for:
- one object ("this folder plays a chime when opened");
- a type ("all PDFs run my script when dropped on the printer");
- a class;
- the style (today's sound scheme).

**Action kinds:**

| Kind | Example | Runs |
|---|---|---|
| `sound:FILE` | today's sounds | in the GUI |
| `run:COMMAND` | a program or script, given `WPS_OBJECT`, `WPS_PATH`, `WPS_EVENT` and `WPS_CLASS` in its environment | by wpsd, as the user's program, in a systemd scope like any launch |
| `call:METHOD ARGS` | a method on the object, or on another object by ID | in wpsd, or in the plug-in's host |
| `set:SETUP` | a setup string (2.5) applied to the object | in wpsd |
| `none` | silence an inherited action | |

- Several actions can be given per event.
- Plug-ins can declare their own events (a printer's `print.done`).
- The bus loses its 8-subscriber limit.
- `sounds.conf` is read once into the WPSound object, then retired.

### 2.5 Setup strings: one text form for any property

This is OS/2's `WinSetObjectData` style:

```
wpsctl set ~/Work 'icon_font=12.Sans Bold;bg_color=#336699;on:open=sound:/usr/share/sounds/x.oga'
wpsctl get ~/Work icon_font            # 12.Sans Bold   (object)
wpsctl props ~/Work                    # every property, value and origin
wpsctl set class:WPFolder 'show_ext=yes'
wpsctl set type:image/png 'icon=...'
```

New ops `get`, `set` and `props`, for any object, class or type. They're used by:
- the GUI;
- `wpsctl` and scripts;
- `.wpobj` files and templates (a `Setup=` line);
- the first start;
- plug-ins.

The same declarations validate them, with the same `expect` counter as `settings_set` today.

### 2.6 Linux's structures as backings

A property can declare where it really lives on Linux. wpsd reads it from there, writes it there, and watches for changes:

| Backing | Properties | Replaces |
|---|---|---|
| `xattr` (default) | most | today's store, unchanged |
| `desktop:KEY` | a program's title, command, icon, types, terminal | the mirroring in `objects.c` and `@assoc` |
| `mimeapps:` | a type object's default program | `wps_app_set_default` |
| `gsettings:SCHEMA/KEY` | the scheme (color-scheme, high-contrast), interface font, text scaling, mouse, event sounds, background | `gnome.c`'s sync and the three readers |
| `autostart:` | Start at login | `startup.c`'s own path |
| `user.mime_type` (after 0.4, as decided) | a file's type | |

Backings are small modules in wpsd with a fixed interface: read, write, watch. A plug-in can add one.

So the Linux structures *are* the objects' properties, not copies synced beside them. Changing GNOME's dark mode, a program's `.desktop` file or `mimeapps.list` changes the WPS object, and the other way round. The GUI and the compositor stop reading config files and gsettings themselves.

**Proposed for later:** GNOME's own per-file metadata (gvfs `metadata::custom-icon`) as a backing for custom icons, so Files would show them too.

### 2.7 Plug-ins

- **Plug-ins declare properties and events** in their `.wpi`, like built-in classes, and get pages, setup strings and actions for free.
- **Everything routed to the host:**
  - settings pages and setters;
  - `menu_select`;
  - `wpGetProperty` and `wpSetProperty` overrides;
  - `call:` actions.
  
  This closes 1.5's gap. Sandbox, deadlines and fallback stay as they are.
- **Types choose classes:** a plug-in can register a class for a MIME type (`WPImageFile` for `image/*`), as the core design planned. Data files of that type become instances of it.
- **An SDK:** a `wps-devel` RPM with the headers, the built-in `.wpi` files, `wpoc.py` and a sample plug-in. `wpsctl plugin new NAME` makes a skeleton that builds with one `make`.
- **Script actions (`run:`)** are the simplest kind of user plug-in: any language, no C. Plug-ins in `~/.config/wps/plugins` load per user, as now.
- **Not in this refactor:** GUI plug-ins (custom drawn controls, animations). They need the separate GUI plug-in host from `gui-design.md`. Plug-in pages use the standard field types until then.

### 2.8 The code reorganized

These are done first, with no behaviour change, so the rest lands in smaller files:

- **`wps-shell.c` split by area:**
  - `desktop.c`, `folderwin.c`, `tree.c`, `notebook.c`, `objmenu.c` (extended), `dnd.c`, `jobs.c`, `session.c` (login restore, first start);
  - `setup.c`: settings objects, replaced by styles in 2.3;
  - `selftest.c`: about 1,900 lines of scenarios.
  - Target: no file over about 1,500 lines.
- **`wpsd.c`:** a table of op handlers grouped into `ops_objects.c`, `ops_jobs.c`, `ops_setup.c`, `ops_props.c` and so on, in place of the `handle()` if-chain.
- **One config-path helper, one gsettings client and one colour parser**, in place of three copies, two copies and two copies.
- The compositor uses the toolkit's menus.

## 3. Before and after

| | Before | After |
|---|---|---|
| Add a property | 5-7 edits in 3-4 files (class C, page text, wpsd, GUI parse, GUI global, fallback, compositor sync) | **1 line in the `.wpi`**, plus the code that uses its value |
| GUI reads a property | parses notebook text | `props`, with values and origins |
| Global vs individual | separate mechanisms, per property | one chain: object, type, class and parents, style, default |
| "All folders", "all images" | not possible | class and type objects |
| Where a value came from | unknown | shown in the notebook, with Reset |
| Looks | 3 C structs, about a dozen hardcoded colours, fixed sizes | scheme objects; every colour, font and size a property |
| Shell and compositor | two copies, text sync, menu font missing, two menu implementations | one style channel, one menu implementation |
| Events | sound only, by name, 8 subscribers | actions per object, type, class or style: sound, run, call, set |
| Scripting | `call` a method; `settings_set` declared fields | setup strings for any property, `get`, `set`, `props` |
| Linux | ad-hoc mirrors, three gsettings readers, `.conf` files | declared backings: gsettings, desktop entries, mimeapps, autostart |
| Plug-ins | classes and methods; pages, setters and menu handlers never reach them; no SDK | properties, events, pages, handlers and type classes, routed; `wps-devel` SDK; script actions |
| Biggest file | 7,226 lines | about 1,500 lines |
| Stored data | | **unchanged**: same xattrs, keys and overlay |

## 4. Keeping existing features working

- **Data:** storage format and keys don't change. Retired files are read once and migrated: `sounds.conf`, `mouse.conf`, `system.conf`, and the System object's font and view keys, which move to class settings. A unit test covers the migration from a 0.3 state.
- **Protocol:** `settings`, `settings_set` and every existing op stay. `props`, `get` and `set` are added. `WPS_PROTOCOL` becomes 3. A wpsd from 0.3 refuses a newer GUI clearly, as it does today.
- **Tests:**
  - every existing suite passes at every stage: `make test`, `gui-test`, the M2-M5 scenarios, `tests/comp`, and the headless self-tests;
  - `make check` is fixed first;
  - added: unit tests for the chain (each level, `inherit`, Reset, notification), for setup strings, for each backing (against a fake gsettings, as udisks has one), and for plug-in routing.
- **Looks:**
  - before any change, a **baseline set of headless screenshots** is taken: the desktop, a folder in each view, the notebooks, menus, the WarpCenter and LaunchPad, and each scheme;
  - after each stage, they're compared pixel by pixel;
  - a difference must be explained or fixed.
- **Each stage** is committed and pushed on this branch. You check it on the VM at stages 3, 4 and 6. The branch merges to `main` when you're satisfied.

## 5. Stages

| Stage | What | Size |
|---|---|---|
| R0 | **Baseline:** fix `make check`; the stray `Sys2b`/`Progs2` folders; baseline screenshots; a script comparing them | S |
| R1 | **Reorganize with no behaviour change:** split `wps-shell.c` and `wpsd.c`, add the op table, merge the helper copies | M |
| R2 | **Property declarations:** wpoc, descriptors, `wpGet/SetProperty`, generated pages; every class converted; `props`/`get`/`set`, setup strings, `wpsctl` | L |
| R3 | **The chain and looks (VM):** class and type objects, scheme objects, origins and Reset, the Class defaults page; the GUI and compositor style; hardcoded colours and sizes become properties; Shift drops on classes | L |
| R4 | **Events and actions (VM):** declared events, `on:` properties, the four action kinds; sounds migrated | M |
| R5 | **Backings:** gsettings, desktop entries, mimeapps, autostart; `gnome.c`, the readers and mirrors retired | M |
| R6 | **Plug-ins (VM):** routing, plug-in properties and events, type classes, the `wps-devel` SDK, `wpsctl plugin new`, a sample plug-in | M-L |

The total is about the size of a version, comparable to 0.3. **Proposed:** this becomes the first part of 0.4.
- R2-R3 already do step 3's metrics and step 7-8's colour and font plumbing.
- Steps 2-10 then build on properties: icon theme and size, folder windows' remembered places, Prevent delete and Work area are all properties.
- The 0.4 plan gets shorter and is renumbered after R6.

## 6. The owner's decisions (2026-11-22)

1. **Folder-to-contents inheritance:** left out, as in OS/2 (2.2).
2. **Script actions (`run:`)** run as the user's own programs, not sandboxed.
3. **Placement:** the first part of 0.4; 0.4's steps are rebuilt on this base after R6.
4. **GNOME's per-file metadata:** not now.

Approved: go (R0 first).

## 7. As built (stages R0-R4, 2026-11-22)

| Stage | Commit | What |
|---|---|---|
| R0 | 8e72dfc | The first start's and the LaunchPad's objects made in a fixed order; `make check` again; `tests/visual` (`make visual-baseline`, `make visual-check`), `make comp-test` |
| R1 | 66a9969 | `wps-shell.c` and `wpsd.c` split by area; wpsd's ops in a table; `tests/gui/golden.sh` for every self-test |
| R2 | ad74f7f | `property` lines in the `.wpi`; every class's pages from them; `get`, `set` (setup strings), `props`; `tests/props` |
| R3a | ea4ee8a | The chain: object, type, class and the classes above, default; `class:`/`type:` targets, `reset`; folders' Show hidden files and extensions; the System's Folders and Fonts fields are all folders' |
| R3d | ad8a06e | Notebooks: where an inherited value comes from, Default, Class... (the notebook of all objects of a class or a type) |
| R3c | b579fdc | Schemes are objects (WPScheme, the Schemes folder); the colours fixed in the code are the scheme's; the compositor gets the schemes' definitions |
| R4 | 2ede8ae, 5b9ad85 | Events' actions: on:object.open, .create, .trash, .delete (an Actions page); none, sound:, run:, call:, set:; through the chain; sounds.conf moved into the Sound object |

**Not done yet in R3, carried on:**

- A folder's sort and columns are computed by WPFolder from older storage, so they are not shared through `class:WPFolder` yet.
- Data files have no shareable properties yet, so `type:` scopes hold nothing until R4's actions and R5's default program.
- The WarpCenter's and the LaunchPad's own colours, and every size (icon sets: 0.4 step 3), are still fixed in the code.
- The compositor still has its own menus.
- A `.wpobj`'s `Setup=` line, and setup strings given to `create`.
- Events raised by the GUI alone (window.*, menu.*, drags and drops) play the Sound object's sounds only; objects' own actions are for the service's events (open, create, trash, delete). Plug-ins' own events wait for R6.
