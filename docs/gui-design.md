# WPS for Linux: GUI Design

Version 0.2 · 2026-11-18 · Draft: direction, session model and toolkit decided; architecture proposed.

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

## Architecture (proposed)

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

## Milestones (proposed)

| Milestone | Contents | Mode |
| --- | --- | --- |
| G0 | Prototypes: Cairo + HarfBuzz drawing on a Wayland surface; a folder window over a 1,000,000-entry folder at 60 fps; sound latency through PipeWire | Stage 1 |
| G1 | Widget set 1–3: frames, containers, menus; one folder window from `wpsd` | Stage 1 |
| G2 | Desktop, all three folder views, notebooks, drag and drop, templates, shadows, trash | Stage 1 |
| G3 | Action events, sound schemes, scheme objects, appearance settings, GNOME settings sync, the GNOME Shell extension | Stage 1 |
| G4 | Window list, launch bar, minimized window viewer, search, accessibility, input methods | Stage 1 |
| G5 | `wps-compositor` on wlroots: frames for all apps, the WPS login session, portal settings | Stage 2 |

## Open

- Review the proposed libraries, process layout and milestones above.
- Later: animation hook API in detail; the GUI plug-in host; how plug-in settings pages draw custom controls.

## Change log

| Version | Date | Changes |
| --- | --- | --- |
| 0.2 | 2026-11-18 | Decided: both modes in stages (inside GNOME first, then a WPS session), settings shared with GNOME; our own drawing. Proposed: libraries, processes, threads, widget set, drag and drop, milestones G0–G5 |
| 0.1 | 2026-11-18 | Direction: OS/2 Warp 4–4.52 base with modern features; configurable appearance with a square, explicit default; action events for sounds and animation hooks |
