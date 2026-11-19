# G1 results (2026-11-18)

G1 builds the first part of the widget set ([gui-design.md](../docs/gui-design.md), Widget set 1–3) and `wps-shell` folder windows over `wpsd`. Measured on Fedora with GNOME Shell 51.1 (Wayland, 60 Hz, monitor at 1×; 2× forced with `WPS_UI_SCALE=2`).

## What is in G1

- `libwpsui` (`gui/ui_*.c`): Wayland connection, windows and popups, three buffers per surface, damage-based drawing, scroll layers, input (double clicks, key repeat, cursors, OS/2 button numbering: button 2 is the right button), schemes (classic and dark), text with HarfBuzz and FreeType and a cache of shaped runs.
- Window frame: sizing border with resize cursors, title bar, system menu button (Maximize/Restore, Minimize, Close), minimize, maximize and close buttons. Square by default; `WPS_UI_CORNER_RADIUS` and `WPS_UI_BUTTONS=hover` try the appearance settings until the scheme objects exist (G3).
- Containers: icon view (arranged in a grid) and details view (Title, Name, Size, Last write), over a model of any size. Selection with click, Ctrl, Shift, rubber band (icon view) and keyboard (arrows, Page Up/Down, Home/End, Space, Ctrl+/ and Ctrl+A select all, Ctrl+\ deselect all, Enter opens, Shift+F10 or Menu key for the pop-up menu). Warp-style scroll bar with arrow buttons.
- Pop-up menus from the service's menu data, with cascades, keyboard navigation and first-letter selection. Folders now have an "Open" cascade (Icon view, Tree view, Details view) as in Warp 4. The GUI carries out well-known items itself (`classes/wps_menu_ids.h`); others go to the service as `menu_select`.
- `wps-shell FOLDER...`: folder windows that stream the folder from `wpsd`, fetch details for what is on screen, open folders in new windows (double click or Enter), open files and programs through the service, and show the object's title. Ctrl+1 / Ctrl+2 switch between icon and details view; Ctrl+W closes.

## Speed: a folder of 1,000,000 entries

`wps-shell --bench 15`: a fling of three rows a frame for 3 s, then a jump to a random place every 0.5 s (as if dragging the scroll bar thumb).

| Scale, window | View | fps | Our time per frame p50 / p99 / max | Frame interval p99 | Late frames |
| --- | --- | --- | --- | --- | --- |
| 1×, 760 × 560 | details | 59.8 | 0.78 / 3.2 / 4.9 ms | 19.4 ms | 3 of 898 |
| 1×, 760 × 560 | icon | 59.8 | 0.77 / 4.1 / 5.6 ms | 18.7 ms | 2 of 898 |
| 2×, 760 × 560 | details | 59.8 | 1.6 / 6.7 / 8.1 ms | 19.1 ms | 3 of 897 |
| 2×, 760 × 560 | icon | 59.8 | 1.5 / 9.6 / 12.5 ms | 18.4 ms | 2 of 897 |
| 1×, 1920 × 1048 | details | 59.8 | 0.42 / 4.0 / 5.3 ms | 17.9 ms | 1 of 898 |
| 2×, 1920 × 1048 | details | 59.5 | **1.7** / 14.3 / 16.5 ms | 20.5 ms | 1 of 893 |
| 2×, 1920 × 1048 | icon | 57.3 | **2.3** / 23.2 / 41.2 ms | 37.5 ms | 23 of 860 |

G0 drew the same 2× full-screen window in 14.5 ms a frame (47 fps). The target set after G0 was under 4 ms; the median is now 1.7–2.3 ms. The p99 values are the jumps, which redraw the whole list; in the icon view at 2× full screen each jump redraws about 270 labels (about 20 ms), so one frame is dropped per jump. A scroll bar drag that lands somewhere new is the only case.

## Correctness

`WPS_SHELL_VERIFY=<prefix>` makes the benchmark rebuild, every 30 frames, what the compositor shows (the window's last buffer with the layer's visible slice placed on it) and compare it pixel by pixel with the whole window drawn afresh, saving both images on a mismatch. In all eight combinations (with and without layers, 1× and 2×, both views), 152 checks during scrolling and jumping all matched exactly.

`tests/test_ui_damage.c` simulates 60,000 frames of random scrolls, row changes and buffer ages and checks every drawn buffer against the truth.

## What the measurements changed

1. **Moving pixels is too slow at 2× full screen.** Damage-based drawing that scrolled by moving the buffer's rows still cost 7–8 ms a frame: the move alone (28 MB, cold in the cache) took 6.8 ms, though raw copying runs at 11.7 GB/s here. So the list area became a **scroll layer** (`gui/ui_layer.c`): a ring buffer of the visible rows, in which scrolling draws only the rows that come into view, shown through a subsurface and `wp_viewporter`. Nothing is moved.
2. **Two subsurfaces halved GNOME's frame rate.** Showing the ring's two halves as two subsurfaces sharing one buffer dropped to 48 fps. The ring is now twice the area's height with every row stored twice, so any window of rows is one contiguous slice and one subsurface shows it. Damaging only the slice shown, not the whole doubled buffer, brought it back to 58–60 fps.
3. **Damage must describe what differs from the frame on screen.** After a scroll that is the whole scrolled area, not just the strip redrawn; the first version reported only the strip, which a compositor caching per surface would have shown wrongly. Redraw planning and the damage sent to the compositor are now separate.
4. **HarfBuzz and Cairo must not share a FreeType face.** Cairo sets the face to the device size when it draws (2× on a 2× screen); HarfBuzz followed, so strings shaped at different moments differed by up to half a pixel. Found by the pixel check; each now has its own face.
5. **List damage must stay inside the list.** Icon cells half scrolled out of view damaged the title bar above them.
6. **Bugs found by AddressSanitizer:** the damage list overflowed a fixed array when many small cells were damaged and then scrolled (now bounded, with a test that fills it).
7. Icons are rendered once per kind, size and scale and painted as images; drawing their paths for every row cost several milliseconds at 2×.

## Found by hand (2026-11-18)

- **Clicking the system menu button while its menu was open killed the shell** (`xdg_wm_base error 2: parent not top most surface`). The click dismissed the menu, then reached the button, which opened a new menu while the dismissed one still existed; destroying the old one afterwards broke Wayland's rule that popups go topmost first. Now at most one menu tree exists: opening a menu destroys any other at once, and a click on the button whose menu it just dismissed closes it without reopening. `WPS_SHELL_SELFTEST=menus` replays these orders (the old code fails it with the same error).

- **A menu could not be closed by clicking elsewhere in the window.** GNOME dismisses a menu only for clicks on other programs' windows; clicks on the program's own windows are delivered to it, and it must close its menus itself. Now a press anywhere outside the open menu closes it and is used up (as in OS/2), keys go to the open menu wherever the keyboard focus is (Escape works), and menus close when their window loses focus.

- **Second round of hand testing:**
  - A double click on the system menu button did nothing: the second click only closed the menu. The second click of a double click now reaches the frame, which closes the window (as in OS/2).
  - Button 2 on the title bar opened the system menu; it no longer does.
  - **Closing one folder window crashed the shell, taking every window with it.** The close button freed the window inside its own click handler, which then used it. Closing now happens on the next loop turn; menus opened from a window are closed before it goes.
  - Pop-up menus on items and the background never appeared. They open on button release, after asking the service for their items, and GNOME refuses an input grab once no button is held, dismissing the menu at once. These menus now open without a grab (the shell closes its own menus on clicks elsewhere and when the window loses focus); the system menu, opened on press, still grabs.
  - New files did not appear until the folder was opened again. Windows now subscribe to their folder's events and re-read it in the background after a change, swapping the new contents in with details for the rows on screen, keeping scroll position, selection and cursor. A burst of changes costs at most two re-reads. (Incremental updates for very large folders: G2.)
  - Self-tests: `WPS_SHELL_SELFTEST=menus`, `=live` (files created and deleted appear and disappear) and `=close` (closing one of two windows leaves the other and the process running).

## Not yet (later milestones)

- Interactive use has not been checked by a person: menus, clicks, keyboard, resizing and opening windows were built against the protocol but only the benchmark has driven them. Please try `wps-shell` by hand.
- Tree view, in-place rename, free icon placement and Arrange (G2).
- The settings notebook (Properties) and confirmation dialogs (G2).
- Incremental live updates for very large folders (G2); for now a change re-reads the folder.
- Drag and drop (G2).
