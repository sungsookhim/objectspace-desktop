// The WPS desktop in a GNOME session (gui-design.md: Coexisting with
// GNOME, stage 1).
//
// GNOME gives ordinary Wayland clients no way to be a desktop. This
// extension starts `wps-shell --desktop` itself, as a Meta.WaylandClient,
// so that only the process it started can claim the role, and gives that
// process's desktop window (app ID org.wps.Desktop) GNOME's desktop window
// type: below all windows, on every workspace, out of Alt+Tab, the window
// list and the overview, covering the primary monitor's work area. If the
// shell exits it is started again, after a delay that grows while it
// keeps failing.
//
// The command comes from config.json next to this file (written by
// tools/install-extension.sh): {"command": ["/path/wps-shell", "--desktop"]}.
//
// Window events: every other program's windows opening, closing, being
// minimized, restored, maximized and unmaximized are written to the
// desktop process's standard input, one line each, so its sound scheme
// covers all windows (gui-design.md: Coexisting with GNOME, stage 1):
//     <event> TAB <app ID> TAB <title> TAB <x> TAB <y> TAB <width> TAB <height>
// WPS's own windows are left out: they raise their events themselves.
//
// The window list (G4): the desktop also gets every window's state, WPS's
// own included (the desktop and WPS's window lists excepted), one line per
// change:
//     W TAB add|change|remove TAB <id> TAB <app ID> TAB <title> TAB <minimized 0|1> TAB <focused 0|1>
// and sends commands back on its standard output, one line each:
//     activate TAB <id>   minimize TAB <id>   close TAB <id>
// (only this process can: the pipes are the ones the extension made).

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const DESKTOP_APP_ID = 'org.wps.Desktop';
// WPS's window list and Minimized Window Viewer: not listed in themselves.
const UNLISTED = new Set([DESKTOP_APP_ID, 'org.wps.WindowList', 'org.wps.LaunchPad']);
const LAUNCHPAD_APP_ID = 'org.wps.LaunchPad';

export default class WpsExtension extends Extension {
    enable() {
        this._on = true;
        this._signals = [];
        this._desktops = new Set();
        this._ready = new Set();   // desktops Mutter has shown
        this._delay = 1;
        this._restartId = 0;
        this._connect(global.display, 'window-created', (_d, w) => this._windowCreated(w));
        this._connect(global.display, 'workareas-changed', () => this._placeAll());
        this._connect(Main.layoutManager, 'monitors-changed', () => this._placeAll());
        const wm = global.window_manager;
        this._connect(wm, 'map', (_wm, actor) => this._windowEvent('window.open', actor));
        this._connect(wm, 'destroy', (_wm, actor) => this._windowEvent('window.close', actor));
        this._connect(wm, 'minimize', (_wm, actor) => this._windowEvent('window.minimize', actor));
        this._connect(wm, 'unminimize', (_wm, actor) => this._windowEvent('window.restore', actor));
        this._connect(wm, 'size-change', (_wm, actor, change) => {
            if (change === Meta.SizeChange.MAXIMIZE) this._windowEvent('window.maximize', actor);
            else if (change === Meta.SizeChange.UNMAXIMIZE) this._windowEvent('window.restore', actor);
        });
        this._lines = [];
        this._writing = false;
        this._listed = new Map();   // window -> its last line's fields
        this._connect(global.display, 'window-created', (_d, w) => this._track(w));
        // OS/2's Ctrl+Esc: the Window List, from anywhere (a setting).
        this._settings = this.getSettings();
        Main.wm.addKeybinding('window-list', this._settings, Meta.KeyBindingFlags.NONE, Shell.ActionMode.NORMAL,
            () => this._showWindowList());
        this._start();
    }

    disable() {
        this._on = false;
        Main.wm.removeKeybinding('window-list');
        this._settings = null;
        for (const [obj, id] of this._signals) obj.disconnect(id);
        this._signals = [];
        if (this._restartId) GLib.source_remove(this._restartId);
        this._restartId = 0;
        this._client?.get_subprocess()?.send_signal(15);
        this._client = null;
        this._out = null;
        this._lines = [];
        this._listed?.clear();
        this._tracked?.clear();
        this._desktops.clear();
        this._ready.clear();
    }

    _connect(obj, signal, fn) {
        this._signals.push([obj, obj.connect(signal, fn)]);
    }

    _command() {
        try {
            const file = this.dir.get_child('config.json');
            const [, bytes] = file.load_contents(null);
            const cfg = JSON.parse(new TextDecoder().decode(bytes));
            if (Array.isArray(cfg.command) && cfg.command.length) return cfg.command;
        } catch (e) {
            console.log(`wps: no usable config.json (${e.message}); using wps-shell from PATH`);
        }
        return ['wps-shell', '--desktop'];
    }

    _start() {
        const argv = this._command();
        const started = GLib.get_monotonic_time();
        try {
            const launcher = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDOUT_PIPE});
            this._client = Meta.WaylandClient.new_subprocess(global.context, launcher, [...argv, '--events-stdin']);
            this._out = this._client.get_subprocess().get_stdin_pipe();
            this._lines = [];
            this._writing = false;
            this._readCommands(this._client.get_subprocess().get_stdout_pipe());
            // the window list as it is now
            this._listed = new Map();
            for (const w of global.display.list_all_windows()) this._track(w);
        } catch (e) {
            console.error(`wps: cannot start ${argv[0]}: ${e.message}`);
            this._retry(started);
            return;
        }
        console.log(`wps: started ${argv.join(' ')}`);
        const client = this._client;
        client.get_subprocess().wait_async(null, (proc, res) => {
            try {
                proc.wait_finish(res);
            } catch (e) {
                // cancelled
            }
            if (client !== this._client) return;   // disabled, or replaced
            this._client = null;
            this._out = null;
            console.log(`wps: ${argv[0]} exited (status ${proc.get_if_exited() ? proc.get_exit_status() : 'signal'})`);
            this._retry(started);
        });
    }

    // Again, sooner if it ran for a while (a crash), later while it keeps failing.
    _retry(started) {
        if (!this._on) return;
        const ran = (GLib.get_monotonic_time() - started) / 1e6;
        this._delay = ran > 60 ? 1 : Math.min(this._delay * 2, 60);
        this._restartId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, this._delay, () => {
            this._restartId = 0;
            if (this._on) this._start();
            return GLib.SOURCE_REMOVE;
        });
    }

    // A window of another program did something: one line to the desktop.
    _windowEvent(name, actor) {
        const w = actor?.meta_window ?? actor?.get_meta_window?.();
        if (!w || !this._out || (this._client && this._client.owns_window(w))) return;
        const type = w.get_window_type();
        if (type !== Meta.WindowType.NORMAL && type !== Meta.WindowType.DIALOG && type !== Meta.WindowType.MODAL_DIALOG) return;
        const clean = v => String(v ?? '').replace(/[\t\n\r]/g, ' ');
        const r = w.get_frame_rect();
        this._lines.push([name, clean(w.get_wm_class()), clean(w.get_title()), r.x, r.y, r.width, r.height].join('\t') + '\n');
        if (this._lines.length > 256) this._lines.shift();   // the desktop is not reading: keep the latest
        this._flush();
    }

    // ----------------------------------------------------------- window list

    _listable(w) {
        const type = w.get_window_type();
        if (type !== Meta.WindowType.NORMAL && type !== Meta.WindowType.DIALOG && type !== Meta.WindowType.MODAL_DIALOG) return false;
        return !UNLISTED.has(w.get_wm_class()) && !w.is_skip_taskbar();
    }

    // Follow one window for the list: its title, minimized and focus.
    _track(w) {
        if (this._focusList && this._client?.owns_window(w)) {
            const id = w.connect('shown', () => {
                w.disconnect(id);
                if (w.get_wm_class() === 'org.wps.WindowList' && this._focusList) {
                    this._focusList = false;
                    w.activate(global.display.get_current_time_roundtrip());
                }
            });
        }
        if (this._tracked?.has(w)) { this._listLine(w); return; }
        this._tracked ??= new Set();
        this._tracked.add(w);
        for (const sig of ['notify::title', 'notify::minimized', 'notify::appears-focused', 'notify::wm-class'])
            this._connect(w, sig, () => this._listLine(w));
        this._connect(w, 'unmanaged', () => {
            this._tracked.delete(w);
            if (this._listed.has(w)) {
                this._listed.delete(w);
                this._send(['W', 'remove', String(w.get_id()), '', '', '0', '0']);
            }
        });
        this._listLine(w);
    }

    _listLine(w) {
        if (!this._listable(w)) {
            if (this._listed.has(w)) { this._listed.delete(w); this._send(['W', 'remove', String(w.get_id()), '', '', '0', '0']); }
            return;
        }
        const clean = v => String(v ?? '').replace(/[\t\n\r]/g, ' ');
        const f = [String(w.get_id()), clean(w.get_wm_class()), clean(w.get_title()), w.minimized ? '1' : '0',
            w.appears_focused ? '1' : '0'];
        const was = this._listed.get(w);
        if (was && was.join('\t') === f.join('\t')) return;
        this._listed.set(w, f);
        this._send(['W', was ? 'change' : 'add', ...f]);
    }

    _send(fields) {
        if (!this._out) return;
        this._lines.push(fields.join('\t') + '\n');
        if (this._lines.length > 1024) this._lines.shift();
        this._flush();
    }

    // Ctrl+Esc: the list window if it is there (brought forward), else the
    // desktop opens it and it is brought forward when it appears.
    _showWindowList() {
        const now = global.display.get_current_time_roundtrip();
        const open = global.display.list_all_windows().find(w => w.get_wm_class() === 'org.wps.WindowList' &&
            w.get_title() === 'Window List');
        if (open) {
            if (open.minimized) open.unminimize();
            open.activate(now);
            return;
        }
        this._focusList = true;
        this._send(['K', 'window-list']);
    }

    // Commands from the desktop: activate, minimize, close a window by ID.
    _readCommands(stream) {
        const data = new Gio.DataInputStream({base_stream: stream});
        const next = () => data.read_line_async(GLib.PRIORITY_DEFAULT, null, (d, res) => {
            let line;
            try {
                [line] = d.read_line_finish_utf8(res);
            } catch (e) {
                return;
            }
            if (line === null) return;   // the desktop went away
            const [cmd, id] = line.split('\t');
            // The LaunchPad's own buttons (no window): Lockup, Find, Shut down.
            if (cmd === 'lock') { Main.screenShield?.lock(true); next(); return; }
            if (cmd === 'find') { Main.overview.show(); next(); return; }
            if (cmd === 'shutdown') {
                try {
                    GLib.spawn_command_line_async('gnome-session-quit --power-off');
                } catch (e) {
                    console.log(`wps: cannot ask to shut down: ${e.message}`);
                }
                next();
                return;
            }
            const w = global.display.list_all_windows().find(x => String(x.get_id()) === id);
            if (GLib.getenv('WPS_DEBUG_COMMANDS')) console.log(`wps: command ${cmd} ${id}: ${w ? w.get_title() : 'no such window'}`);
            if (w) {
                // a time GNOME takes as now (the event's own may be 0)
                const now = global.display.get_current_time_roundtrip();
                if (cmd === 'activate') {
                    if (w.minimized) w.unminimize();
                    w.activate(now);
                }
                else if (cmd === 'minimize') w.minimize();
                else if (cmd === 'close') w.delete(now);
            }
            next();
        });
        next();
    }

    // Writes one at a time (a stream takes one outstanding write), never
    // blocking the Shell.
    _flush() {
        if (this._writing || !this._out || !this._lines.length) return;
        this._writing = true;
        const out = this._out;
        const bytes = new GLib.Bytes(new TextEncoder().encode(this._lines.shift()));
        out.write_bytes_async(bytes, GLib.PRIORITY_DEFAULT, null, (o, res) => {
            try {
                o.write_bytes_finish(res);
            } catch (e) {
                if (out === this._out) this._out = null;   // the desktop went away
            }
            this._writing = false;
            this._flush();
        });
    }

    _windowCreated(w) {
        if (!this._client || !this._client.owns_window(w)) return;
        const lp = () => {
            if (w.get_wm_class() !== LAUNCHPAD_APP_ID || this._launchpads?.has(w)) return;
            (this._launchpads ??= new Set()).add(w);
            // The LaunchPad floats above windows, on every workspace, out of
            // Alt+Tab; it starts at the bottom centre of the screen.
            w.make_above();
            w.stick();
            w.hide_from_window_list();
            this._connect(w, 'unmanaged', () => this._launchpads.delete(w));
            const id = w.connect('shown', () => {
                w.disconnect(id);
                const mon = Main.layoutManager.primaryIndex;
                const wa = w.get_work_area_for_monitor(mon), r = w.get_frame_rect();
                w.move_frame(false, wa.x + Math.floor((wa.width - r.width) / 2), wa.y + wa.height - r.height - 8);
            });
        };
        lp();
        this._connect(w, 'notify::wm-class', lp);
        if (this._isDesktop(w)) this._makeDesktop(w);
        else this._connect(w, 'notify::wm-class', () => this._isDesktop(w) && this._makeDesktop(w));
    }

    _isDesktop(w) {
        return w.get_wm_class() === DESKTOP_APP_ID || w.get_gtk_application_id?.() === DESKTOP_APP_ID;
    }

    _makeDesktop(w) {
        if (this._desktops.has(w)) return;
        this._desktops.add(w);
        w.set_type(Meta.WindowType.DESKTOP);
        w.stick();
        w.hide_from_window_list();
        this._connect(w, 'unmanaged', () => {
            this._desktops.delete(w);
            this._ready.delete(w);
        });
        // Sized once Mutter has shown it (not before: it is not ready), and
        // whenever the work area changes.
        const id = w.connect('shown', () => {
            w.disconnect(id);
            this._ready.add(w);
            this._place(w);
        });
        const r = w.get_frame_rect();
        console.log(`wps: desktop window: type ${w.get_window_type()}, on all workspaces ${w.is_on_all_workspaces()}, ` +
            `skip taskbar ${w.is_skip_taskbar()}, ${r.width}x${r.height}+${r.x}+${r.y}`);
    }

    _place(w) {
        if (!this._ready.has(w)) return;
        const mon = Main.layoutManager.primaryIndex;
        if (mon < 0) return;
        const wa = w.get_work_area_for_monitor(mon);
        if (w.get_maximize_flags()) w.unmaximize();   // it asks to be maximized when on its own
        w.move_resize_frame(false, wa.x, wa.y, wa.width, wa.height);
    }

    _placeAll() {
        for (const w of this._desktops) this._place(w);
    }
}
