# WPS for Linux: GUI Design

Version 0.1 · 2026-11-18 · Draft: direction decided, details open.

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

## Open (needs a decision)

- **Toolkit and drawing:** GTK 4, Qt 6, or our own drawing on a small Wayland client library.
- **Session:** a full Wayland session with its own compositor and window manager (needed for our own window frames everywhere), or a shell that runs inside existing desktops (other programs keep their own frames).
- Later, once those are decided: the window manager design, drag and drop across processes, the folder view threading model, and how settings pages from plug-ins are drawn.

## Change log

| Version | Date | Changes |
| --- | --- | --- |
| 0.1 | 2026-11-18 | Direction: OS/2 Warp 4–4.52 base with modern features; configurable appearance with a square, explicit default; action events for sounds and animation hooks |
