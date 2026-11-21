# WPS for Linux: GUI Design

Version 0.6 · 2026-11-18 · Direction, session model and toolkit decided; architecture accepted; G0 and G1 done.

The GUI sits on top of the object core ([object-core-design.md](object-core-design.md)). It talks to `wpsd` only through the service API: events, menus and settings as data, streaming listings, jobs. It never makes a synchronous call to `wpsd` from its input or drawing threads (object core: Requirements carried to the GUI design).

## Direction (decided 2026-11-18)

### Base: the OS/2 Warp 4 – 4.52 Workplace Shell

The base design follows the Workplace Shell of OS/2 Warp 4 through 4.52:

- The desktop is itself a folder. Folders open as icon, tree and details views.
- Objects have pop-up menus on mouse button 2 and settings notebooks with tabbed pages.
- Templates folder; drag a template to make an object. Shadows; drag with Ctrl+Shift.
- Mouse button 2 drags; mouse button 1 selects. Drop targets show whether a drop is allowed before release.
- The window list, and minimized windows going to the Minimized Window Viewer (or hidden, per the setting).
- A launch bar like WarpCenter or the LaunchPad, made of ordinary objects.
- Sharp, square window frames with clear borders, title bars and visible buttons.

The look is our own work in that style. Icons, artwork, fonts and sounds are drawn or recorded for this project; none are copied from IBM's releases.

### Modern features on top

Features that users now expect, added without changing the object model:

- High-DPI and fractional scaling; vector icons.
- A search field over the object core, by title, class, type and settings.
- Thumbnails and previews for data files, made off the GUI thread.
- A dark scheme alongside the classic one.
- Accessibility: keyboard access to every action, screen-reader names from object titles and classes, high-contrast schemes.
- Notifications, and progress for jobs, using the job events that already exist.
- Touchpad gestures where they map to existing actions (scroll, zoom of icon views).

### Appearance is configurable; the classic look is the default

Rounded corners waste pixels, and window controls that appear only on hover are hard to find. The default therefore stays square and explicit. People who expect a modern look can switch it on, a setting at a time:

| Setting | Default | Choices |
| --- | --- | --- |
| Window corner radius | 0 (square) | 0 – 16 px |
| Title-bar buttons | Always visible | Always, on hover |
| Frame border | Visible, sizeable | Visible, thin, none (resize from the edge area) |
| Window shadows | Off | Off, subtle, strong |
| Translucency | Off | Off, title bars, menus |
| Title-bar layout | Icon left, buttons right | Mirror, compact |
| Scheme | Classic | Classic, dark, high contrast, user-made |

These settings are objects themselves: a Scheme Palette and a System settings object, with settings notebooks, like the originals.

### Every mouse action is configurable, as on OS/2's Mouse object (decided 2026-11-18)

No mouse action is fixed in the code: the GUI asks the mapping for each one. The defaults are OS/2's.

| Action | Default | |
| --- | --- | --- |
| Select | button 1 | Ctrl and Shift still extend or toggle a selection |
| Open | button 1, double click | |
| Pop-up menu | button 2 click | the system menu icon opens with select or pop-up |
| Edit title | Alt + button 1 | in-place rename |
| Drag | button 2 | used from the drag-and-drop step |
| Window list | button 3 | used from G4 |

Also left-handed (buttons 1 and 2 swapped), double-click time and wheel speed. Each action is a chord: modifiers, a button, and click or double click. Until the Mouse settings object exists (G3, with its notebook pages Mappings and Setup), they are read from `<config>/mouse.conf`:

```
popup_menu  = button3
edit_title  = shift+button1
left_handed = yes
double_click_ms = 500
```

### Every action is an event: sounds now, animation hooks for later

The Workplace Shell played system sounds on actions. Here every user action raises a named action event, which a sound scheme and (later) animations can respond to.

- **Action events** have stable names, for example `window.open`, `window.close`, `window.minimize`, `window.maximize`, `window.restore`, `object.drop`, `object.drop.refused`, `object.delete`, `object.trash`, `object.shred`, `menu.open`, `menu.select`, `job.done`, `job.failed`, `alert.warning`, `alert.error`, `shell.startup`, `shell.shutdown`. Each carries the object, its class, and the place on screen where it happened.
- **Sound schemes** map action events to sounds. There is a scheme object with a settings page listing every event, the way the Sound object did. Silent is a choice; the default plays few sounds.
- **Animation hooks.** Each action event has a *before* and an *after* phase, together with the start and end rectangles (for example an icon's position and the opened window's frame). An animation can run between them. The first version ships no animations, or only the simplest ones (zoom rectangles on open and minimize, off by default). Later versions or plug-ins can add more.
  - An animation never delays the action itself: the window is open, and takes input, while it is still being animated. Input cancels a running animation.
  - Animation plug-ins run in the GUI, not in `wpsd`, and have a frame-time budget; one that misses it is dropped for that event.
  - Reduce motion (an accessibility setting) turns all animations off.
- Classes and plug-ins can define their own action events (a printer object's `print.done`), which appear in the sound scheme like the built-in ones.

### Coexisting with GNOME: both modes, in stages (decided 2026-11-18)

One GUI codebase runs in two modes. The object service (`wpsd`) is the same underneath both, so objects, titles, settings, shadows and templates are identical whichever mode a user logs into.

- **Stage 1: inside a GNOME session.** The desktop folder, folder windows, settings notebooks, the window list and the launch bar run as ordinary Wayland clients. GNOME's window manager keeps its frames. An optional GNOME Shell extension (`wps@…`) forwards GNOME's window events (open, close, minimize, maximize, focus) to the GUI as action events, so sound schemes and event hooks cover all windows. Square corners and always-visible buttons apply to WPS's own windows only.
- **Stage 2: a WPS session.** "WPS" appears beside "GNOME" on the login screen. Our compositor draws window frames for every app that accepts server-side decorations, and every window action raises our action events directly. GTK 4 and libadwaita apps draw their own title bars; by default they are left as they are, and a per-application setting can put our frame around them instead.
- **Settings shared with GNOME.** Where WPS and GNOME's Settings overlap (fonts, scaling, colour scheme, keyboard, mouse, touchpad), WPS reads and writes the same GNOME settings (gsettings), so a change in either appears in both. In the WPS session we also answer the desktop portal's settings interface, so GNOME and other apps follow the WPS scheme's light or dark choice. Settings only WPS has stay in WPS's settings objects.

### Toolkit: our own drawing (decided 2026-11-18)

A small widget set of our own, written in C like the object core, gives full control of the Warp 4 look (frames, menus, notebooks, containers). The same code works in both session modes, and it can later serve the X64-DOS target.

Libraries (proposed):

- **Cairo** for 2D drawing (C, already on every desktop). Skia would be faster on the GPU, but it is C++ and large; Cairo is fast enough for folder views. Revisit only if measurements show otherwise.
- **HarfBuzz and FreeType** for text shaping and fonts, with **fontconfig** for font lookup. We do not use Pango, so the GUI does not depend on GLib.
- **libxkbcommon** for keyboard layouts. Input methods (CJK and others) go through the Wayland `text-input-v3` protocol.
- **AT-SPI** over D-Bus for screen readers. This is the one place we need GLib-free D-Bus code (sd-bus).
- **Stage 2 compositor: wlroots** (C). Smithay is in Rust and a separate language; Mutter and KWin are not meant to be embedded.

## Architecture (accepted 2026-11-18)

### Processes

| Process | Role | Stage |
| --- | --- | --- |
| `wpsd` | Object core (done, M0–M5) | — |
| `wps-shell` | The GUI: desktop, folder windows, notebooks, window list, launch bar, sounds, action events | 1 |
| GNOME Shell extension | Forwards GNOME window events to `wps-shell` | 1 |
| `wps-compositor` | Wayland compositor and window manager, server-side frames | 2 |

One `wps-shell` process draws all WPS windows, as the original Workplace Shell did in PMSHELL. A crash restarts it; windows reopen where they were, because their positions are object settings in `wpsd`.

### Threads in `wps-shell`

- **Input and drawing thread:** handles input and draws frames. It never waits on `wpsd` or the disk; it only reads the window's own model.
- **Service thread:** one connection to `wpsd`, with subscriptions. Replies and events update models, then wake the input and drawing thread.
- **Worker pool:** thumbnails, icon loading and rasterising, and text layout of large lists.
- **Audio thread:** plays action-event sounds through PipeWire, preloaded so a sound starts within 10 ms of the action.

A folder window's model is filled from the streaming listing a page at a time. Scrolling to rows not yet loaded shows placeholders and asks for that page, so a folder of a million entries opens like a small one.

### Widget set

Built in the order the shell needs it:

1. Frames: title bar, system menu, min/max/close/hide buttons, sizing border; the configurable appearance settings apply here.
2. Containers: icon view (free placement, as in the original), tree view, details view; selection, rubber-band, in-place rename.
3. Pop-up menus and cascades from the service's menu data.
4. Notebooks with tabbed pages from the service's settings data: check box, entry, spin button, drop-down, radio group, slider, list.
5. Dialogs and job progress windows.
6. Scroll bars, buttons, labels, tool tips.

Everything is drawn by the scheme. A scheme is a set of colours, fonts, metrics (border width, corner radius, button size) and bitmaps or vector images, stored as an object.

### Drag and drop

Within `wps-shell`, drag and drop passes object IDs or a selection ID (object core: Selections and drag and drop). To other apps, and from them, it uses Wayland data-device with `text/uri-list` and a WPS type carrying IDs. Mouse button 2 drags, with Ctrl (copy) and Ctrl+Shift (shadow), as in OS/2. Button 1 drag can be enabled as a setting for users used to other desktops.

### Action events

Raised in `wps-shell` (and, in stage 1, received from the GNOME Shell extension). A small in-process bus delivers each event to the sound scheme and to animation hooks, then to any plug-ins that subscribed. Plug-in hooks for the GUI run in a separate GUI plug-in host with a frame-time budget, like the object plug-in hosts (sandboxed, never in-process).

## Milestones

| Milestone | Contents | Mode |
| --- | --- | --- |
| G0 | Prototypes: Cairo + HarfBuzz drawing on a Wayland surface; a folder window over a 1,000,000-entry folder at 60 fps; sound latency through PipeWire. **Done** ([results](../proto/g0/RESULTS.md)) | Stage 1 |
| G1 | Widget set 1–3: frames, containers, menus; one folder window from `wpsd`. **Done** ([results](../gui/RESULTS-G1.md)) | Stage 1 |
| G2 | Desktop, all three folder views, notebooks, drag and drop, templates, shadows, trash | Stage 1 |
| G3 | Action events, sound schemes, scheme objects, appearance settings, GNOME settings sync, the GNOME Shell extension. **Done** (2026-11-19) | Stage 1 |
| G4 | Window list, launch bar, minimized window viewer, search, accessibility, input methods. **Done** (2026-11-19; also per-object icons, the LaunchPad's order and Log off) | Stage 1 |
| G5 | `wps-compositor` on wlroots: frames for all apps, the WPS login session, portal settings | Stage 2 |

### G0 findings

- A folder window over a million entries, drawn by one process with Cairo on the CPU, holds 60 fps at 1× and at 2× for ordinary window sizes, cold or warm. The first rows appear in under 7 ms; names stream in faster than anyone can scroll (0.5 s warm, 2 s cold); rows awaiting details show grey bars for a frame or two after a jump. HarfBuzz shaping of every visible string each frame costs nothing measurable.
- A near-full-screen window at 2× (7.6 million pixels) drops to 47 fps when the whole window is redrawn each frame (14.5 ms). **G1's widget set therefore draws by damage**: scrolling copies the rows already drawn and draws only the newly exposed ones; the frame, headings and status line are redrawn only when they change; three buffers so a frame never waits on the compositor. Target: under 4 ms a frame at 2× full screen.
- Sound: with the stream kept open, a sound is written within 5 ms of its action. The device's own buffer was not measurable on the test machine (dummy output only); the sound service asks PipeWire for a 256-frame quantum. Measure on real speakers in G3.
- GNOME does not draw frames for Wayland clients, so in stage 1 every WPS window draws its own frame, which is what the configurable appearance needs anyway.

### G1 findings

- **Scrolling uses a scroll layer, not pixel moves.** Moving the buffer's rows still cost 7–8 ms a frame at 2× full screen (the move alone 6.8 ms). A container's list is now a ring buffer of the visible rows, twice the area's height with every row stored twice so any window is one contiguous slice, shown by one subsurface through `wp_viewporter`. Scrolling draws only the rows that come into view. At 2× full screen: 1.7–2.3 ms a frame (median), 57–60 fps. Compositors without subsurfaces or `wp_viewporter` get the pixel-moving path.
- **Damage sent to the compositor is what differs from the frame on screen** (after a scroll, the whole scrolled area), kept separate from what we redraw.
- **Text shaping has its own FreeType face**, separate from Cairo's, or positions depend on which drew last at 2×.
- **A pixel check** (`WPS_SHELL_VERIFY`) rebuilds what the compositor shows and compares it with a fresh full redraw; it found the last two problems and now passes in every mode.
- Jumps (a scroll bar drag landing somewhere new) redraw the whole list: at 2× full screen in the icon view that costs about 20 ms, one dropped frame per jump. Acceptable; GPU drawing later would remove it.

### G2 progress (2026-11-18)

- Done: in-place rename (F2, Alt+click; renames the file), settings notebooks, tree view, drag and drop (move, copy, shadow; jobs with progress and clash dialogs; to and from other programs), mouse mappings, icon view with icons placed anywhere.
- **Icon view places icons anywhere, as in OS/2.** Moved icons keep their stored positions (by object ID, on the folder, personal in folders the user does not own). Other icons get a grid spot the first time their name is seen and keep it for the session, so resizing a window moves nothing, a new file takes the next free spot and a deleted one leaves a gap. A small folder without stored positions opens arranged by name. Arrange (folder menu) makes a grid that fits the window, in name order, stored for folders up to 2,000 objects. Icon view scrolls sideways when icons reach past the window.
- Drags within an icon view move the icons, each keeping its offset from where it was grabbed. Over a million entries icon view still runs at 59–60 fps at 1× and 2× full screen, and the pixel check matches.
- Found: GNOME chooses a drag's action from the modifiers (Shift move, Ctrl copy, Alt ask); whether the program sees Ctrl+Shift during a drag is being checked by hand (`WPS_DEBUG_DND=1`). Alt (ask) opens a Move / Copy / Create shadow menu at the drop point.

- **Central objects in the GUI** (2026-11-18): shadows made by dragging (and other WPS-only objects) are kept centrally unless the folder is set to keep files (its View page: "Keep objects as files here"). Windows list them after the files, address them as `vobj:<id>`, and move or copy them through the service. Anything dropped on a window's background in icon view lands where it was dropped, each item keeping its offset in the drag: shadows and central objects as soon as they exist, copied and moved files once they have their final names (after any renaming).

- **Delete** (2026-11-18): Delete... in every object's menu and the Delete key move the selection (or the item) to the trash; Shift+Delete deletes for good. Both ask first, as OS/2 does; the trash confirmation can be turned off in `<config>/system.conf` (`confirm_delete`, `confirm_folder_delete`, as on OS/2's System object Confirmations page, until that object exists); deleting for good always asks. Items no trash can take are asked about again. Windows of deleted folders close.

- **The desktop in GNOME** (decided 2026-11-19: the Shell extension now). The desktop is `wps-shell --desktop`: the XDG desktop folder (`~/Desktop`) in icon view, frameless, on GNOME's background picture (`org.gnome.desktop.background`, the dark picture with a dark colour scheme, followed live; decoded by `glycin-thumbnailer`, GNOME's sandboxed image loaders, once per picture and size into `$XDG_CACHE_HOME/wps`), with no status line, no scroll bars, and no closing. The `wps@wps-linux` GNOME Shell extension starts it as a `Meta.WaylandClient` (so only that process can claim the role) and gives its window (app ID `org.wps.Desktop`) Mutter's desktop window type: below all windows, on every workspace, out of Alt+Tab, the window list and the overview, covering the primary monitor's work area; it restarts the desktop if it exits, and stays loaded on the lock screen. Window events for sounds come to the same extension in G3. `tests/gnome/run.sh` runs all this in a private headless GNOME Shell with a probe extension.
- Found doing so: Mutter holds the frame callbacks of a surface that a subsurface covers entirely, so a window whose scroll layer covered it all stopped drawing (the desktop froze after the overview). The desktop draws without a scroll layer.

- **Templates** (2026-11-19, as in OS/2 Warp 4). The templates folder is the XDG templates folder (`~/Templates`, the one GNOME's "New Document" uses): anything in it is a template, besides objects marked as one. The service adds three built-in templates there once, Folder, Data File and Program, kept centrally so other programs do not see them (deleting one is final). Templates are drawn on a pad of paper. Dragging a template (moving or copying it) makes a new object where it is dropped, with its name ready to edit; a folder template brings a copy of what it holds. Create another, in every object's menu, makes a new, empty object of the same kind beside it (a data file of the same type), its name ready to edit. Double-clicking a built-in template opens its settings; a file template opens like the file. The desktop gets a shadow of the templates folder on its first start.

- **Big folders follow changes** (2026-11-19). A window applies each change event to its rows as the service did to its listing (by position, so a million-entry folder whose names are mostly unread follows too), keeping selection, cursor and a name being edited. Events that come together are applied as one batch; a batch of more than 64 changes, or one that does not follow on from the rows (version check), is cheaper as one read of the folder, as before. Only the rows on screen have their details read again.

### G3 progress (2026-11-19)

- **Action events.** One bus in `wps-shell` (`gui/shell/events.c`). The toolkit reports what it handles itself through one hook (`window.minimize`, `window.maximize`, `window.restore`, `menu.open`, `menu.select`); the shell raises the rest: `window.open`, `window.close`, `object.open`, `object.drag`, `object.drop`, `object.create`, `object.trash`, `object.delete`, `job.done`, `job.failed`, `alert.warning` (every question to the user), `alert.error` (every failure reported), `shell.startup`, `shell.shutdown`. `WPS_DEBUG_EVENTS=1` prints them.
- **Sounds.** A PipeWire stream (role Event, 256-frame buffer) paused while quiet; sounds decoded once into memory, with libsndfile when installed (the freedesktop theme is Ogg), else WAV only; up to 8 at once. The default scheme plays few sounds: alerts, finished and failed jobs, emptying into the trash, start-up. Sounds are named as in GNOME's sound theme (`org.gnome.desktop.sound theme-name`, then freedesktop), and GNOME's "event sounds" switch turns them off. Until the Sound object, `<config>/sounds.conf` changes the scheme (`volume`, `enabled`, `<event> = <theme sound | file | none>`).
- **Settings objects.** System, Sound, Mouse and Scheme Palette are objects (classes `WPSystem`, `WPSound`, `WPMouse`, `WPSchemePalette`, on a common `WPSetup` that describes each one's settings as a table, so a setting is one line), with settings notebooks. The service makes one of each per user, once, as central objects in the System Setup folder (`$XDG_DATA_HOME/wps/System Setup`); the desktop gets a shadow of it once. `{op:"setup"}` returns every value; the GUI reads them at start and again whenever one of the objects changes, so a change in a notebook takes effect at once. They replace `mouse.conf`, `sounds.conf` and `system.conf` (still read first, then overridden).
  - System: window corner radius, title-bar buttons (always or on hover), frame border (visible, thin, none), window shadows, translucency, title-bar layout, delete confirmations. Sound: on/off, volume, a sound for each action event. Mouse: each action's chord, left-handed, double-click time, wheel speed. Scheme Palette: classic, dark, high contrast.
  - All applied, live. Shadows: under GNOME our windows draw their own frames, so the shadow is ours too: the buffer has a margin around the window (10 px subtle, 18 px strong; none when maximized, for the desktop and for menus), the window geometry tells GNOME where the window is (moving, tiling, maximizing use its real size) and the input region keeps clicks in the shadow for what is behind. Translucency: the title bar (or menus) painted with alpha, so what is behind shows through. Title-bar layout: mirrored (buttons left, system menu right) or compact (a shorter bar).
- **Settings shared with GNOME** (`gui/shell/gnome.c`). Scheme Palette ↔ GNOME's colour scheme (dark: prefer-dark, classic: default) and high-contrast switch; Mouse left-handed and double-click time ↔ GNOME's mouse settings; Sound on/off ↔ GNOME's event sounds; GNOME's interface font and text scaling are followed (WPS has no font setting of its own yet). Every wps-shell follows the font; the desktop process keeps the rest in step both ways through `gsettings monitor`, writing a value only where it differs, so nothing echoes. When the desktop starts, GNOME's values win, since other programs already follow them. The status line now grows with the font.
- **Window events from GNOME.** The `wps@wps-linux` extension writes every other program's window events (opening, closing, minimizing, restoring, maximizing) to the desktop process's standard input, one line each (`--events-stdin`); the desktop raises them as action events, so the sound scheme covers all windows. A pipe from the process the extension itself started: nothing else can send these.

### G4 progress (2026-11-19)

- **Window List and Minimized Window Viewer** (`gui/shell/winlist.c`). Inside GNOME a program cannot see other programs' windows, so the `wps@wps-linux` extension tells the desktop about every window (WPS's own included, the desktop and the lists themselves excepted) on its standard input: `W` lines (add, change, remove; ID, app, title, minimized, focused, app icon, maximized). Commands go back on the desktop's standard output (activate, restore, minimize, maximize, close, terminate an ID): both pipes are the ones the extension made, so nothing else can use them, and even WPS's own windows are restored through GNOME (no Wayland program can unminimize itself). The Window List (all windows, as a list) opens with the window-list mouse button on the desktop (button 3, as on OS/2) or from the desktop's menu; double-click shows a window; its menu has Show, Restore, Minimize, Maximize, Close and Terminate (ends a program that does not answer Close). The Minimized Window Viewer shows the minimized windows as icons; double-click restores one. Ctrl+Esc (OS/2's) opens the Window List from anywhere: the extension registers the shortcut (a setting in its schema, `org.gnome.shell.extensions.wps window-list`), asks the desktop to open the list, and brings it forward (only GNOME can give it the focus).
- **LaunchPad** (`gui/shell/launchpad.c`; Warp 3/4's floating palette, chosen over WarpCenter for the GNOME session). A frameless bar the extension keeps above windows, on every workspace, starting at the bottom centre. Left: the grip (drag to move), then OS/2's buttons Lockup, Find (WPS's Find window), Shut down (a dialog: Shut down, Restart, Log off, Cancel; the extension then ends the session through `gnome-session-quit`), Window list. Then a button per object in its folder (`$XDG_DATA_HOME/wps/LaunchPad`, `WPS_LAUNCHPAD_DIR`): drop any object on the bar to add a shadow of it (a second of the same is refused); button 2 gives Open, Remove from LaunchPad, Open LaunchPad folder. Above each button an arrow opens its drawer (a folder in "LaunchPad Drawers"), a popup list; drop on the arrow to add to it. The first time it gets the home folder, Templates and System Setup. The toolkit gained `ui_window_set_size` (a window choosing its own, fixed size) and `ui_draw_item_icon`.
- **Keyboard access** (accessibility): Shift+Esc opens any framed window's system menu, as on OS/2 (Alt+Space too, where GNOME does not take it for its own window menu); the LaunchPad, once it has the keyboard, goes from button to button with Left and Right (Home, End, Tab), presses one with Enter or Space, opens a drawer with Up and a menu with Shift+F10. The high contrast scheme (G3) follows GNOME's high-contrast setting.
- **Objects' own icons** (`gui/shell/icons.c`, `gui/shell/iconpick.c`; WPObject's `icon`). Every object's Properties has an Icon page: the icon (its own, or its kind's), Choose..., Default, and the picture takes a dropped image file. Any image glycin reads (PNG, SVG, JPEG, ICO, WebP, JPEG XL, ...) is made into a 128-pixel PNG in `$XDG_DATA_HOME/wps/icons` (so the source may go), whose path is the object's `icon` setting; folder listings carry it, and folders, the desktop, the LaunchPad and its drawers draw it (a shadow shows its target's). Choose... opens the icon picker: the system's icon themes (each icon name once, the best-sized file; symbolic icons left out) or WPS's icon library folder (`$XDG_DATA_HOME/wps/Icons`: any images put there), with a search field; previews of non-PNG images are made once into the cache. The service now announces an object's change again once it is written, so listings (which read storage) show new titles and icons at once.
- **Screen readers** (`gui/ui_a11y.c`): AT-SPI over D-Bus (sd-bus; no new packages), registered with the AT-SPI registry. The application's children are its windows (frame, dialog with its message, popup menu, the desktop, the LaunchPad as a tool bar); each window's child is the element with the keyboard, as `ui_a11y_focus` describes it: role, name, description, text (entries), states (focused, selected, checked, expanded), position in its set. Each focus change is a new object announced with object:state-changed:focused; window activation with window:activate; a check box or a value changing in place with its state or description event. The toolkit describes containers (the item under the cursor: title, kind, n of m), menus and dialogs; the Find window, settings notebooks and the LaunchPad describe their controls. Test: `tests/gnome/with-gnome.sh -- tests/gui/a11y-run.sh` (an AT-SPI client prints what it hears). WPS_NO_A11Y turns it off, WPS_DEBUG_A11Y traces it.
- **Input methods** (`gui/ui_textinput.c`): Wayland's text-input-v3, which GNOME connects to IBus. An entry's owner says when the entry has the keyboard (`ui_text_input_focus`: Find's fields, renaming in folders, text fields in settings notebooks); the input method is then enabled for it, with the entry's text and caret position. Committed text arrives as key events, one character each, so every owner's key handling sees it as typed; text being composed is drawn at the caret, underlined. WPS_UI_NO_IM turns it off, WPS_DEBUG_IM traces it.
- **Font fallback** (`gui/ui_draw.c`): characters the interface font lacks are drawn with the font fontconfig picks for each one (Noto CJK for Japanese, Chinese and Korean, and so on), shaped in runs per font; before this they were boxes.
- **Type-ahead** in every container (folders, the desktop, the lists): a printable key moves the cursor to the next item whose title starts with it, and keys typed within a second spell the start of a title; the same letter again goes on to the next one.
- **Find** (`gui/shell/findwin.c`, `service/find.c`; OS/2's Find, searching as you type). wpsd's `find` walks a folder (and its subfolders) on a thread of its own and sends matches as `found` events in batches as it goes; `find_cancel` stops one. Names match without regard to case, as a part of the name or a wildcard pattern; hidden entries only when the pattern starts with a dot; the type narrows to folders, files or programs. The window has OS/2's fields (name, folder, include subfolders, type) over a details view of the results (title, the folder it is in, size, date) and a status line; every change of the name starts the search again, Enter or Find starts it with the other fields, Escape stops a search or closes the window. Double-click opens a result; its menu also opens the folder it is in. Opened by the LaunchPad's Find (the home folder) and Ctrl+F in any folder window (that folder). Gaps: docs/open-issues.md.

## Open

- Later: animation hook API in detail; the GUI plug-in host; how plug-in settings pages draw custom controls.


### G5 progress

- **Step 1, the compositor core** (`compositor/wps-compositor.c`, wlroots 0.20). Windows of programs that agree to it (xdg-decoration, server side) get WPS's frame, drawn by the same code as the toolkit's own windows: the frame's geometry and painting moved to `gui/ui_framepaint.c` (`ui_frame_geom`), which the toolkit and the compositor both use. Programs that draw their own title bars (GTK 4, libadwaita, WPS's own windows) keep them. Click to focus and raise; the title bar moves a window (a maximized one is restored once dragged), the border sizes it, double-clicking the title maximizes and restores, the buttons minimize, maximize and close (double-clicking the system menu button closes, as on OS/2); Alt+Tab brings the window behind forward (a minimized one is restored); Alt+F4 closes; Ctrl+Alt+BackSpace ends the compositor. WPS's desktop (app ID org.wps.Desktop) covers the screen below every window, the LaunchPad (org.wps.LaunchPad) floats above them at the bottom centre; with no desktop the background is OS/2's teal. Runs nested in a session (`tools/try-compositor.sh`) or headless for tests (`WPS_COMP_HEADLESS`, with a script of input and screenshots: `tests/comp/run.sh`; `build/tests/ssd-client` asks for a server-side frame). Findings: a screen with no physical size or refresh rate (headless) made GTK 4 die of SIGFPE (it divides by the refresh rate), so the compositor gives such screens 60 Hz; SIGCHLD ignored in the compositor must be reset for the programs it starts (dbus-run-session could not wait for its child). Still to come: keyboard input to programs in the test script, scaled (HiDPI) screens.
- **Step 2, WPS's shell inside** (`compositor/desktop.c`). The compositor starts WPS's desktop (`-d 'wps-shell --desktop FOLDER'`) on a Wayland connection of its own (a socket pair: only that process can be the desktop, the LaunchPad or the Window List; another program naming itself so is an ordinary window) and talks to it exactly as the GNOME extension does, so the shell needed no change: window events and window lines (with the app's icon, from its .desktop file and the icon themes; an SVG made into a PNG by glycin) on its standard input, `K window-list` for Ctrl+Esc (the Window List then has the keyboard), and its commands back (activate, restore, minimize, maximize, close, terminate; Shut down's Log off ends the session; lock: step 4). If it ends it is started again, after a delay that grows while it keeps failing. xdg-activation brings a window forward when its program asks. Framed windows have their system menu (Maximize or Restore, Minimize, Close), from the button or Shift+Esc (Alt+Space). Tested headless: the shell's own Window List self-test (`DESKENV=WPS_SHELL_SELFTEST=winlist tests/comp/run.sh`) minimizes, restores, maximizes and terminates windows through it.
- **Step 3, X11 programs** (XWayland, started when the first one connects; `DISPLAY` is set for everything the compositor starts). An X11 window is a window like the others: WPS's frame unless it asks for no decorations (Motif hints), moved, sized, maximized, minimized and closed (WM_DELETE_WINDOW) the same way, and told its position on the screen whenever it changes (X11 programs open their menus by it); it is in the Window List, and Terminate ends its own process (_NET_WM_PID; never XWayland's, which would end every X11 program). Override-redirect windows (X11 menus, tooltips) are shown where they say, above everything. Window code goes through `view_*` helpers that work for both kinds. Headless, the compositor has a keyboard of its own: XWayland will not start without one, and the test scripts now type into programs too (`key` sends to the focused window what is not a compositor key). Found: SIGCHLD ignored in the compositor is inherited by XWayland (wlroots starts it), whose keymap compiler then 'fails'; the compositor no longer ignores SIGCHLD and starts its own programs with a double fork instead. `build/tests/x11-client` is an X11 test window (with `popup`, an override-redirect one too).
- **Step 4, the WPS session.** `tools/install-session.sh` (with sudo) puts "WPS" on the login screen (`/usr/share/wayland-sessions/wps.desktop` running `/usr/local/bin/wps-session`, which runs `tools/wps-session` from the checkout) and the lock screen's PAM service (`/etc/pam.d/wps-lock`: the system's own authentication); `remove` takes them away. `tools/wps-session` sets the session's names (XDG_CURRENT_DESKTOP=WPS), takes the keyboard layout and the pointer's theme and size from GNOME's settings, starts wpsd (the user service) and the compositor with WPS's desktop on the Desktop folder, and once it is up gives D-Bus and systemd services its display. Other programs follow the WPS scheme's light or dark choice through the desktop portal: for the WPS desktop it uses GNOME's GTK backend (`~/.config/xdg-desktop-portal/wps-portals.conf`), whose Settings read GNOME's settings, which the desktop keeps in step with the WPS scheme (G3). The compositor's frames follow the desktop's scheme, font and frame options (it sends `scheme`, `font`, `frame` lines; GNOME's extension ignores them). On a console the compositor has the session's screens and input devices (libseat), and Ctrl+Alt+F1..F12 switch consoles. The lock screen (`compositor/lock.c`; OS/2's Lockup, from the LaunchPad or Super+L) covers every screen and takes all input until the user's password is typed; PAM checks it in a child process, so the compositor never waits. Shut down and Restart power off or restart the machine (systemctl) in the session, Log off ends it. Its log: `~/.local/state/wps/session.log`. While the session runs, `wps-session.target` (written by `tools/wps-session`) keeps systemd's `graphical-session.target` up, which the desktop portal needs; a watcher (a transient user service) ends both however the session ends, stopping `graphical-session.target` outright as GNOME's session does (services such as the portal otherwise keep it, and gnome-session then refuses the next GNOME login). Tried by the user on a VirtualBox VM: GNOME → WPS → GNOME logins work. Virtual machines' screens (Virtual-N) get a pointer drawn in software (their cursor planes show none). `~/.config/wps/session.env` holds settings of one's own for the session (e.g. `WLR_RENDERER=pixman`, `WPS_COMP_DEBUG=1`: a trace of keys, clicks and focus, a heartbeat, and a watchdog that logs the stacks of a stuck loop).
- **After step 4** (tried on the VirtualBox VM, 2026-11-20). VirtualBox's and VMware's 3D (vmwgfx) corrupted windows' pictures, so there the compositor draws in software (`WLR_RENDERER=gles2` asks for the GPU); frames come no faster than the screen refreshes; programs' GPU buffers are not test-imported on vmwgfx (GTK 4 programs were disconnected). wpsd gives programs the desktop's `DISPLAY` and starts each in a systemd scope of its own (`app-wps-*.scope`). Drag and drop: the compositor starts drags from a held button, shows the dragged icon, and shows on the pointer what the drop does (the Mouse object's copy, shadow and drop-menu keys). The session runs XDG autostart entries meant for it (VirtualBox's clipboard helper among them) and offers the data-control protocols and the primary selection. Idle (`compositor/idle.c`): screens off after GNOME's blank-screen time, then the lock if automatic lock is on; idle inhibitors (video) keep them on; logind's lock request and sleep lock the screen. Fullscreen: a window asking for it covers its screen without a frame, above the LaunchPad while active, and gets its box back after.

## Change log

| Version | Date | Changes |
| --- | --- | --- |
| 0.6 | 2026-11-18 | G2 progress: free icon placement, drag and drop, notebooks, tree view, rename; findings listed under G2 progress |
| 0.5 | 2026-11-18 | Decided: every mouse action configurable as on OS/2's Mouse object; mouse.conf until the settings object (G3). Renaming an icon renames the file (object core v0.14) |
| 0.4 | 2026-11-18 | G1 done: libwpsui (frames, icon and details views, menus), wps-shell folder windows; scroll layers; findings listed under G1 |
| 0.3 | 2026-11-18 | Architecture accepted. G0 done: 60 fps over a million entries; damage-based drawing required for 2× full screen; sound written within 5 ms |
| 0.2 | 2026-11-18 | Decided: both modes in stages (inside GNOME first, then a WPS session), settings shared with GNOME; our own drawing. Proposed: libraries, processes, threads, widget set, drag and drop, milestones G0–G5 |
| 0.1 | 2026-11-18 | Direction: OS/2 Warp 4–4.52 base with modern features; configurable appearance with a square, explicit default; action events for sounds and animation hooks |
