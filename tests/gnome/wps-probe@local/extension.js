import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
export default class Probe {
    enable() {
        GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 4, () => { Main.overview.hide(); return GLib.SOURCE_REMOVE; });
        this._id = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 14, () => {
            const ws = global.workspace_manager.get_active_workspace();
            const tab = global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null).map(w => w.get_title());
            const all = global.display.list_all_windows();
            const stack = global.display.sort_windows_by_stacking(all).map(w => `${w.get_title()}[type ${w.get_window_type()}]`);
            console.log(`probe: tab list: ${JSON.stringify(tab)}`);
            console.log(`probe: stacking bottom to top: ${JSON.stringify(stack)}`);
            for (const w of all) {
                const r = w.get_frame_rect();
                console.log(`probe: window "${w.get_title()}" app ${w.get_wm_class()} type ${w.get_window_type()} all-ws ${w.is_on_all_workspaces()} skip-taskbar ${w.is_skip_taskbar()} ${r.width}x${r.height}+${r.x}+${r.y}`);
            }
            // switch workspace: the desktop must still be there
            const n = global.workspace_manager.get_n_workspaces();
            console.log(`probe: workspaces ${n}`);
            const out = GLib.getenv('PROBE_SHOT');
            if (out) {
                const shot = new Shell.Screenshot();
                const stream = Gio.File.new_for_path(out).replace(null, false, Gio.FileCreateFlags.NONE, null);
                shot.screenshot(false, stream, (o, res) => {
                    try { o.screenshot_finish(res); console.log('probe: screenshot saved'); } catch (e) { console.log(`probe: screenshot failed ${e.message}`); }
                    stream.close(null);
                });
            }
            return GLib.SOURCE_REMOVE;
        });
    }
    disable() { GLib.source_remove(this._id); }
}
