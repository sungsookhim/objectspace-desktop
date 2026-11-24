// Test probe for tests/gnome/run.sh (never installed for real use).
//
// PROBE_MODE unset: after 14 s, log what GNOME sees (Alt+Tab list,
// stacking, window types and sizes, monitors) and take a screenshot to
// $PROBE_SHOT.
// PROBE_MODE=keys: Ctrl+Esc, then what has the focus; with PROBE_MENU=1
// (or =aside: the folder window moved from under the list first) button 2
// on the Window List's first row and its Maximize.
// PROBE_MODE=interact: drive a virtual pointer like a user: double-click
// the desktop's notes.txt and Projects, then drag notes.txt with button 2
// (the right button, OS/2's drag button), with screenshots before, during
// and after ($PROBE_SHOT-<n>.png), logging the windows as it goes.

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

function shoot(path) {
    const shot = new Shell.Screenshot();
    const stream = Gio.File.new_for_path(path).replace(null, false, Gio.FileCreateFlags.NONE, null);
    shot.screenshot(false, stream, (o, res) => {
        try {
            o.screenshot_finish(res);
            console.log(`probe: screenshot ${path}`);
        } catch (e) {
            console.log(`probe: screenshot failed ${e.message}`);
        }
        stream.close(null);
    });
}

function windows(tag) {
    const all = global.display.list_all_windows();
    const tab = global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null).map(w => w.get_title());
    console.log(`probe: [${tag}] tab list: ${JSON.stringify(tab)}`);
    console.log(`probe: [${tag}] stacking bottom to top: ${JSON.stringify(global.display.sort_windows_by_stacking(all).map(w => `${w.get_title()}[type ${w.get_window_type()}]`))}`);
    for (const w of all) {
        const r = w.get_frame_rect();
        console.log(`probe: [${tag}] window "${w.get_title()}" app ${w.get_wm_class()} type ${w.get_window_type()} all-ws ${w.is_on_all_workspaces()} skip-taskbar ${w.is_skip_taskbar()} ${r.width}x${r.height}+${r.x}+${r.y}`);
    }
}

export default class Probe {
    enable() {
        this._ids = [];
        const at = (secs, fn) => this._ids.push(GLib.timeout_add(GLib.PRIORITY_DEFAULT, secs * 1000, () => {
            fn();
            return GLib.SOURCE_REMOVE;
        }));
        const out = GLib.getenv('PROBE_SHOT');
        // Leave the overview GNOME starts in (asking again: a request during
        // its opening animation is ignored).
        for (const t of [3, 4, 4.5, 5, 5.5])
            at(t, () => { if (Main.overview.visible) Main.overview.hide(); });
        at(5.8, () => console.log(`probe: overview visible ${Main.overview.visible}`));
        if (GLib.getenv('PROBE_MODE') === 'keys') {
            // Ctrl+Esc on a virtual keyboard: the Objectspace Window List, focused.
            const seat = global.backend?.get_default_seat?.() ?? global.stage.context.get_backend().get_default_seat();
            const kb = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
            this._dev = kb;
            const key = (sym, down) => kb.notify_keyval(GLib.get_monotonic_time(), sym, down ? Clutter.KeyState.PRESSED : Clutter.KeyState.RELEASED);
            at(9, () => {
                key(Clutter.KEY_Control_L, true); key(Clutter.KEY_Escape, true);
                key(Clutter.KEY_Escape, false); key(Clutter.KEY_Control_L, false);
                console.log('probe: pressed Ctrl+Esc');
            });
            if (GLib.getenv('PROBE_MENU')) {
                // then button 2 on the list's first row: its menu
                // (made after Ctrl+Esc: a pointer device made earlier lost
                // the key press in headless GNOME)
                let ptr;
                at(9.5, () => {
                    ptr = this._ptr = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE);
                    ptr.notify_absolute_motion(GLib.get_monotonic_time(), 640, 400);   // off the hot corner
                });
                // A left click first (it selects the row): the first click
                // of a new device is lost in headless GNOME (see drawer).
                const rowClick = (b, tag) => {
                    const wl = global.display.list_all_windows().find(w => w.get_title() === 'Window List');
                    if (!wl) return;
                    const r = wl.get_frame_rect();
                    ptr.notify_absolute_motion(GLib.get_monotonic_time(), r.x + 80, r.y + 4 + 22 + 10);
                    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, () => {
                        ptr.notify_button(GLib.get_monotonic_time(), b, Clutter.ButtonState.PRESSED);
                        GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, () => {
                            ptr.notify_button(GLib.get_monotonic_time(), b, Clutter.ButtonState.RELEASED);
                            return GLib.SOURCE_REMOVE;
                        });
                        return GLib.SOURCE_REMOVE;
                    });
                    console.log(`probe: ${tag} on the Window List`);
                };
                if (GLib.getenv('PROBE_MENU') === 'aside')   // nothing but the desktop under the list
                    at(9.2, () => global.display.list_all_windows().find(w => w.get_wm_class() === 'io.github.sungsookhim.Objectspace.shell')?.move_frame(true, 0, 400));
                at(10, () => rowClick(1, 'button 1'));
                at(10.6, () => rowClick(1, 'button 1'));
                at(11.2, () => rowClick(3, 'button 2'));
                // its Maximize (the fourth item; the menu opens at the click)
                at(12, () => {
                    const wl = global.display.list_all_windows().find(w => w.get_title() === 'Window List');
                    if (!wl) return;
                    const r = wl.get_frame_rect();
                    ptr.notify_absolute_motion(GLib.get_monotonic_time(), r.x + 80 + 50, r.y + 36 + 89);
                    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, () => {
                        ptr.notify_button(GLib.get_monotonic_time(), 1, Clutter.ButtonState.PRESSED);
                        ptr.notify_button(GLib.get_monotonic_time(), 1, Clutter.ButtonState.RELEASED);
                        return GLib.SOURCE_REMOVE;
                    });
                });
                at(12.8, () => {
                    const p = global.display.list_all_windows().find(w => w.get_wm_class() === 'io.github.sungsookhim.Objectspace.shell');
                    console.log(`probe: after the menu: Projects maximized ${p?.is_maximized()}`);
                });
            }
            at(GLib.getenv('PROBE_MENU') ? 13 : 11, () => {
                const w = global.display.focus_window;
                console.log(`probe: focused window: ${w ? `"${w.get_title()}" app ${w.get_wm_class()}` : 'none'}`);
                windows('keys');
                if (out) shoot(out);
            });
            return;
        }
        if (GLib.getenv('PROBE_MODE') === 'drawer') {
            // The Launcher's first drawer arrow: open, close by a click
            // elsewhere; open, close by the arrow again.
            const seat = global.backend?.get_default_seat?.() ?? global.stage.context.get_backend().get_default_seat();
            const dev = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE);
            this._dev = dev;
            const t = () => GLib.get_monotonic_time();
            const click = (x, y) => {
                // moved there, a pause, pressed and held a moment, as a hand
                // does (a popup's grab needs the button down)
                dev.notify_absolute_motion(t(), x, y);
                GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, () => {
                    dev.notify_button(t(), 1, Clutter.ButtonState.PRESSED);
                    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 150, () => {
                        dev.notify_button(t(), 1, Clutter.ButtonState.RELEASED);
                        return GLib.SOURCE_REMOVE;
                    });
                    return GLib.SOURCE_REMOVE;
                });
            };
            const arrow = () => {
                const lp = global.display.list_all_windows().find(w => w.get_wm_class() === 'io.github.sungsookhim.Objectspace.Launcher');
                if (!lp) return null;
                const r = lp.get_frame_rect();
                return [r.x + 12 + 4 * 40 + 8 + 27, r.y + 3 + 5];
            };
            const step = (s, f) => at(s, () => { const a = arrow(); console.log(`probe: drawer step at ${s}: arrow ${a}`); if (a) f(a); });
            at(8, () => dev.notify_absolute_motion(t(), 640, 400));   // off the hot corner
            // The first click (step 10) is lost in headless GNOME: the hidden
            // overview's dash (its app icons at the bottom centre, under the
            // Launcher) still takes it, and may start Text Editor or
            // Software; step 13 on works. Not seen in a real session.
            step(10, a => click(...a));                  // open the first drawer
            at(11, () => { if (out) shoot(out.replace('.png', '-open.png')); });
            step(11.5, a => click(1150, 500));           // closed by a click elsewhere
            step(13, a => click(...a));                  // open it again
            step(14.5, a => click(a[0] + 56, a[1]));     // the next button's arrow, the first drawer still open
            at(15.5, () => { if (out) shoot(out.replace('.png', '-open2.png')); });
            step(16, a => click(a[0] + 56, a[1]));       // that arrow again: closed
            at(17, () => { windows('drawer'); if (out) shoot(out); });
            return;
        }
        if (GLib.getenv('PROBE_MODE') === 'imtype') {
            // Ctrl+F in the folder window (Find), then "note" typed: with
            // an input method running, the text comes through text-input.
            const seat = global.backend?.get_default_seat?.() ?? global.stage.context.get_backend().get_default_seat();
            const kb = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
            this._dev = kb;
            const t = () => GLib.get_monotonic_time();
            const key = (sym, down) => kb.notify_keyval(t(), sym, down ? Clutter.KeyState.PRESSED : Clutter.KeyState.RELEASED);
            at(9, () => {
                const w = global.display.list_all_windows().find(x => x.get_wm_class() === 'io.github.sungsookhim.Objectspace.shell');
                w?.activate(global.display.get_current_time_roundtrip());
            });
            at(10, () => { key(Clutter.KEY_Control_L, true); key(Clutter.KEY_f, true); key(Clutter.KEY_f, false); key(Clutter.KEY_Control_L, false); });
            [...'note'].forEach((ch, i) => at(12 + i * 0.3, () => { key(Clutter[`KEY_${ch}`], true); key(Clutter[`KEY_${ch}`], false); }));
            at(14, () => { const f = global.display.focus_window; console.log(`probe: focused ${f?.get_title()}`); if (out) shoot(out); });
            return;
        }
        if (GLib.getenv('PROBE_MODE') === 'lpkeys') {
            // The keyboard alone: the Launcher (focused), Home, Right twice to Shut down,
            // Enter: its dialog; Shift+Esc: the dialog's system menu; Escape twice.
            const seat = global.backend?.get_default_seat?.() ?? global.stage.context.get_backend().get_default_seat();
            const dev = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE);
            const kb = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
            this._dev = dev;
            this._ptr = kb;
            const t = () => GLib.get_monotonic_time();
            const click = (x, y) => {
                dev.notify_absolute_motion(t(), x, y);
                GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, () => {
                    dev.notify_button(t(), 1, Clutter.ButtonState.PRESSED);
                    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, () => { dev.notify_button(t(), 1, Clutter.ButtonState.RELEASED); return GLib.SOURCE_REMOVE; });
                    return GLib.SOURCE_REMOVE;
                });
            };
            const key = (sym, mod) => {
                if (mod) kb.notify_keyval(t(), mod, Clutter.KeyState.PRESSED);
                kb.notify_keyval(t(), sym, Clutter.KeyState.PRESSED);
                kb.notify_keyval(t(), sym, Clutter.KeyState.RELEASED);
                if (mod) kb.notify_keyval(t(), mod, Clutter.KeyState.RELEASED);
            };
            const lp = () => global.display.list_all_windows().find(w => w.get_wm_class() === 'io.github.sungsookhim.Objectspace.Launcher')?.get_frame_rect();
            dev.notify_absolute_motion(t(), 640, 400);   // off the hot corner from the start
            // focused as a click gives it (clicks are unreliable headless)
            at(10.5, () => global.display.list_all_windows().find(w => w.get_wm_class() === 'io.github.sungsookhim.Objectspace.Launcher')?.activate(global.display.get_current_time_roundtrip()));
            at(11.5, () => { console.log(`probe: focused ${global.display.focus_window?.get_title()}`); key(Clutter.KEY_Home); key(Clutter.KEY_Right); key(Clutter.KEY_Right); });
            at(12, () => { if (out) shoot(out.replace('.png', '-focus.png')); });
            at(12.5, () => key(Clutter.KEY_Return));
            // Shift+Esc in the dialog: its system menu (Alt+Space is GNOME's own window menu)
            // (held a moment, as a hand does: a menu's grab needs the key's serial to be the latest)
            at(13.5, () => {
                windows('dialog');
                kb.notify_keyval(t(), Clutter.KEY_Shift_L, Clutter.KeyState.PRESSED);
                kb.notify_keyval(t(), Clutter.KEY_Escape, Clutter.KeyState.PRESSED);
                GLib.timeout_add(GLib.PRIORITY_DEFAULT, 150, () => {
                    kb.notify_keyval(t(), Clutter.KEY_Escape, Clutter.KeyState.RELEASED);
                    kb.notify_keyval(t(), Clutter.KEY_Shift_L, Clutter.KeyState.RELEASED);
                    return GLib.SOURCE_REMOVE;
                });
            });
            at(14.5, () => { if (out) shoot(out); key(Clutter.KEY_Escape); });
            at(15.5, () => key(Clutter.KEY_Escape));   // the menu, then the dialog
            at(16.5, () => windows('after escape'));
            return;
        }
        if (GLib.getenv('PROBE_MODE') === 'shutdown') {
            // The Launcher's Shut down: its dialog, then Log off by keys
            // (Right twice, Enter). OSD_DEBUG_COMMANDS (run.sh) makes the
            // extension log the command rather than run it.
            const seat = global.backend?.get_default_seat?.() ?? global.stage.context.get_backend().get_default_seat();
            const dev = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE);
            const kb = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
            this._dev = dev;
            this._ptr = kb;
            const t = () => GLib.get_monotonic_time();
            const click = (x, y) => {
                dev.notify_absolute_motion(t(), x, y);
                GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, () => {
                    dev.notify_button(t(), 1, Clutter.ButtonState.PRESSED);
                    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, () => {
                        dev.notify_button(t(), 1, Clutter.ButtonState.RELEASED);
                        return GLib.SOURCE_REMOVE;
                    });
                    return GLib.SOURCE_REMOVE;
                });
            };
            const key = sym => { kb.notify_keyval(t(), sym, Clutter.KeyState.PRESSED); kb.notify_keyval(t(), sym, Clutter.KeyState.RELEASED); };
            const lp = () => global.display.list_all_windows().find(w => w.get_wm_class() === 'io.github.sungsookhim.Objectspace.Launcher')?.get_frame_rect();
            at(8, () => dev.notify_absolute_motion(t(), 640, 400));   // off the hot corner
            // a first click on Window list (the first is lost headless; if not, a list opens)
            at(10, () => { const r = lp(); if (r) click(r.x + 12 + 3 * 40 + 18, r.y + r.height / 2 + 5); });
            at(11, () => { const r = lp(); if (r) click(r.x + 12 + 2 * 40 + 18, r.y + r.height / 2 + 5); console.log('probe: clicked Shut down'); });
            at(12.5, () => { windows('dialog'); if (out) shoot(out.replace('.png', '-dialog.png')); });
            at(13.5, () => { key(Clutter.KEY_Right); key(Clutter.KEY_Right); key(Clutter.KEY_Return); console.log('probe: Log off chosen'); });
            at(15, () => { windows('end'); if (out) shoot(out); });
            return;
        }
        if (GLib.getenv('PROBE_MODE') === 'quiet') {
            at(Number(GLib.getenv('PROBE_SHOT_AT') ?? 13), () => { if (out) shoot(out); });
            at(14, () => windows('end'));
            return;
        }
        if (GLib.getenv('PROBE_MODE') === 'frame') {
            // Click the folder window's maximize button (in its frame: border
            // 4, title bar 22, buttons 18 wide 2 apart, 3 from the right end).
            const seat = global.backend?.get_default_seat?.() ?? global.stage.context.get_backend().get_default_seat();
            const dev = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE);
            this._dev = dev;
            const other = () => global.display.list_all_windows().find(w => w.get_wm_class() === 'io.github.sungsookhim.Objectspace.shell');
            at(2, () => dev.notify_absolute_motion(GLib.get_monotonic_time(), 640, 700));
            at(8.5, () => { if (out) shoot(`${out}-before.png`); });
            at(9, () => {
                const w = other();
                if (!w) return;
                const r = w.get_frame_rect();
                console.log(`probe: folder window ${r.width}x${r.height}+${r.x}+${r.y}, maximized ${w.is_maximized()}`);
                const x = r.x + r.width - 4 - 3 - 2 * 20 + 9, y = r.y + 4 + 11;
                dev.notify_absolute_motion(GLib.get_monotonic_time(), x, y);
                dev.notify_button(GLib.get_monotonic_time(), Clutter.BUTTON_PRIMARY, Clutter.ButtonState.PRESSED);
                dev.notify_button(GLib.get_monotonic_time(), Clutter.BUTTON_PRIMARY, Clutter.ButtonState.RELEASED);
                console.log(`probe: clicked ${x},${y}`);
            });
            at(11, () => {
                const w = other();
                const r = w?.get_frame_rect();
                console.log(`probe: after the click: ${r?.width}x${r?.height}+${r?.x}+${r?.y}, maximized ${w?.is_maximized()}`);
                if (out) shoot(out);
            });
            return;
        }
        if (GLib.getenv('PROBE_MODE') !== 'interact') {
            // Another program's window (the folder window run.sh opens) through
            // minimize, restore, maximize and unmaximize: window events.
            const other = () => global.display.list_all_windows().find(w => w.get_wm_class() === 'io.github.sungsookhim.Objectspace.shell');
            at(9, () => other()?.minimize());
            at(10, () => other()?.unminimize());
            at(11, () => other()?.maximize());
            at(12, () => other()?.unmaximize());
            at(14, () => {
                windows('end');
                console.log(`probe: workspaces ${global.workspace_manager.get_n_workspaces()}`);
                console.log(`probe: monitors ${JSON.stringify(Main.layoutManager.monitors.map(m => `${m.width}x${m.height}+${m.x}+${m.y}`))}, primary ${Main.layoutManager.primaryIndex}`);
                if (out) shoot(out);
            });
            return;
        }
        const seat = global.backend?.get_default_seat?.() ?? global.stage.context.get_backend().get_default_seat();
        const dev = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE);
        this._dev = dev;
        const now = () => GLib.get_monotonic_time();
        const move = (x, y) => dev.notify_absolute_motion(now(), x, y);
        const button = (b, down) => dev.notify_button(now(), b, down ? Clutter.ButtonState.PRESSED : Clutter.ButtonState.RELEASED);
        const click = b => { button(b, true); button(b, false); };
        // Desktop icons (the desktop starts below the 32-pixel top bar):
        // Projects at x 54, notes.txt at x 154; icon centres at y 32 + 24.
        const notes = [154, 56], projects = [54, 56];
        // Where the drag ends: PROBE_DROP=x,y (default: inside the folder window that opened)
        const dropAt = (GLib.getenv('PROBE_DROP') ?? '400,450').split(',').map(Number);
        at(2, () => move(640, 400));   // away from the hot corner at 0,0 (it opens the overview)
        at(6, () => { move(...notes); click(Clutter.BUTTON_PRIMARY); });            // select
        at(6.5, () => { if (out) shoot(`${out}-1-start.png`); windows('start'); });
        at(7, () => { move(...notes); click(Clutter.BUTTON_PRIMARY); click(Clutter.BUTTON_PRIMARY); console.log('probe: double-clicked notes.txt'); });
        at(9, () => { move(...projects); click(Clutter.BUTTON_PRIMARY); click(Clutter.BUTTON_PRIMARY); console.log('probe: double-clicked Projects'); });
        at(11, () => { windows('after opening'); if (out) shoot(`${out}-2-opened.png`); });
        // Drag notes.txt with button 2 to (400, 450), in steps.
        at(12, () => { move(...notes); button(Clutter.BUTTON_SECONDARY, true); console.log('probe: drag starts'); });
        for (let k = 1; k <= 10; k++)
            at(12 + k * 0.05, () => move(notes[0] + (dropAt[0] - notes[0]) * k / 10, notes[1] + (dropAt[1] - notes[1]) * k / 10));
        at(13, () => { if (out) shoot(`${out}-3-dragging.png`); });
        at(13.5, () => { button(Clutter.BUTTON_SECONDARY, false); console.log('probe: dropped'); });
        at(13.8, () => { if (out) shoot(`${out}-4-dropped.png`); });
        at(16, () => { if (out) shoot(`${out}-5-later.png`); windows('end'); });
    }

    disable() {
        for (const id of this._ids) GLib.source_remove(id);
        this._ids = [];
        this._dev = null;
        this._ptr = null;
    }
}
