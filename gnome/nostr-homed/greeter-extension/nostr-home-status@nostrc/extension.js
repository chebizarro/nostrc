// nostr-home-status@nostrc — user-session panel indicator.
//
// Watches $XDG_STATE_HOME/nostr-homed/porthome-status.json (falling back
// to ~/.local/state/nostr-homed/porthome-status.json) and shows:
//   - sync state, and home-loading progress while the provisioner runs
//   - cache and home-size usage against their limits
//   - unresolved conflicts (personal home and shared folders), each with
//     the choices nostr-home-syncd accepts for it
//
// Choosing a resolution runs `nostr-home-status conflicts resolve` (argv,
// no shell). That only queues the request; nostr-home-syncd applies and
// publishes it. The extension never edits files itself.
//
// NEVER surfaces secret material — only the well-known status keys are
// read (see model.js).
//
// Beads: nostrc-h10m.1 (Phase 5 I1), nostrc-h10m.6.

import St from 'gi://St';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Clutter from 'gi://Clutter';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main         from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu    from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu    from 'resource:///org/gnome/shell/ui/popupMenu.js';

import {buildModel, resolveArgv, resolveOutcome} from './model.js';

const MAX_FILE_BYTES = 512 * 1024;
const BAR_WIDTH = 220;

function _statusPath() {
    const xdg = GLib.getenv('XDG_STATE_HOME');
    if (xdg && xdg.length > 0)
        return `${xdg}/nostr-homed/porthome-status.json`;
    return `${GLib.get_home_dir()}/.local/state/nostr-homed/porthome-status.json`;
}

function _readStatus(path) {
    try {
        const [ok, bytes] = Gio.File.new_for_path(path).load_contents(null);
        if (!ok || !bytes || bytes.length > MAX_FILE_BYTES) return null;
        return JSON.parse(new TextDecoder().decode(bytes));
    } catch (_e) {
        return null;
    }
}

// A labelled usage/progress row: text plus a decorative bar (the text
// carries the numbers for screen readers).
const BarItem = GObject.registerClass(
class BarItem extends PopupMenu.PopupBaseMenuItem {
    _init() {
        super._init({reactive: false, can_focus: false});
        const box = new St.BoxLayout({vertical: true, x_expand: true});
        this.label = new St.Label({style_class: 'nostr-home-status-text'});
        this.label.clutter_text.line_wrap = true;
        this._track = new St.Widget({style_class: 'nostr-home-status-bar',
            style: `width: ${BAR_WIDTH}px;`});
        this._fill = new St.Widget({style_class: 'nostr-home-status-bar-fill'});
        this._track.add_child(this._fill);
        box.add_child(this.label);
        box.add_child(this._track);
        this.add_child(box);
        this.label_actor = this.label;
    }

    update(text, fraction, over = false) {
        this.visible = !!text;
        this.label.text = text ?? '';
        this._track.visible = typeof fraction === 'number';
        if (typeof fraction === 'number') {
            this._fill.style = `width: ${Math.round(BAR_WIDTH * fraction)}px;`;
            if (over) this._fill.add_style_class_name('over');
            else this._fill.remove_style_class_name('over');
        }
    }
});

const Indicator = GObject.registerClass(
class Indicator extends PanelMenu.Button {
    _init(ext) {
        super._init(0.0, 'Portable home status', false);
        this._ext = ext;
        this._monitor = null;
        this._busy = new Set();
        this._cancellable = new Gio.Cancellable();
        this._conflictSignature = null;

        const box = new St.BoxLayout({style_class: 'panel-status-menu-box',
            y_align: Clutter.ActorAlign.CENTER});
        this._icon = new St.Label({text: '?', y_align: Clutter.ActorAlign.CENTER,
            style_class: 'system-status-icon nostr-home-status-glyph'});
        this._label = new St.Label({text: 'home', y_align: Clutter.ActorAlign.CENTER,
            style_class: 'nostr-home-status-label'});
        box.add_child(this._icon);
        box.add_child(this._label);
        this.add_child(box);

        this._syncItem = new PopupMenu.PopupMenuItem('', {reactive: false});
        this.menu.addMenuItem(this._syncItem);
        this._atRestItem = new PopupMenu.PopupMenuItem('', {reactive: false});
        this._atRestItem.label.clutter_text.line_wrap = true;
        this.menu.addMenuItem(this._atRestItem);
        this._provisionItem = new BarItem();
        this.menu.addMenuItem(this._provisionItem);
        this._cacheItem = new BarItem();
        this.menu.addMenuItem(this._cacheItem);
        this._homeItem = new BarItem();
        this.menu.addMenuItem(this._homeItem);

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._conflictHeader = new PopupMenu.PopupMenuItem('', {reactive: false});
        this.menu.addMenuItem(this._conflictHeader);
        this._conflictSection = new PopupMenu.PopupMenuSection();
        this.menu.addMenuItem(this._conflictSection);

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._pathItem = new PopupMenu.PopupMenuItem(_statusPath(), {reactive: false});
        this._pathItem.label.add_style_class_name('nostr-home-status-pathrow');
        this.menu.addMenuItem(this._pathItem);
    }

    startWatching() {
        try {
            // Watch the directory: writers rename a new file into place.
            const dir = Gio.File.new_for_path(_statusPath()).get_parent();
            this._monitor = dir.monitor_directory(Gio.FileMonitorFlags.WATCH_MOVES, null);
            this._monitor.connect('changed', (_m, file) => {
                if (file && file.get_basename() === 'porthome-status.json') this.refresh();
            });
        } catch (_e) {
            // Directory missing until the first writer runs: the backstop covers it.
        }
        // Backstop for a missing directory or a dropped notification.
        this._pollId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 15,
            () => { this.refresh(); return GLib.SOURCE_CONTINUE; });
        this.refresh();
    }

    stopWatching() {
        this._cancellable.cancel();
        if (this._monitor) { this._monitor.cancel(); this._monitor = null; }
        if (this._pollId) { GLib.source_remove(this._pollId); this._pollId = 0; }
    }

    refresh() {
        const m = buildModel(_readStatus(_statusPath()));
        this._model = m;
        this._icon.text = m.glyph;
        this._label.text = m.label;
        this.accessible_name = m.accessibleName;

        this._syncItem.label.text = m.syncText;
        this._atRestItem.label.text = m.atRestText;
        this._provisionItem.update(m.provision.text, m.provision.fraction);
        this._cacheItem.update(m.cache?.text ?? null, m.cache?.fraction ?? null, m.cache?.over);
        this._homeItem.update(m.home?.text ?? null, m.home?.fraction ?? null, m.home?.over);

        this._conflictHeader.label.text = m.available ? m.conflictText : 'Conflicts: unavailable';
        // Rebuild the list only when it changed, so an open submenu is not
        // torn down under the pointer by an unrelated status write.
        const signature = JSON.stringify(m.conflicts.map(c =>
            [c.id, c.pending, this._busy.has(c.id), c.actions.map(a => a.choice)]));
        if (signature !== this._conflictSignature) {
            this._conflictSignature = signature;
            this._renderConflicts(m.conflicts);
        }
    }

    _renderConflicts(conflicts) {
        this._conflictSection.removeAll();
        for (const c of conflicts) {
            const where = c.folder ? ` — ${c.folder}` : '';
            const sub = new PopupMenu.PopupSubMenuMenuItem(`${c.title}${where}`);
            sub.label.accessible_name = `Conflict: ${c.path}`;

            const text = new PopupMenu.PopupMenuItem(c.description, {reactive: false});
            text.label.clutter_text.line_wrap = true;
            text.label.add_style_class_name('nostr-home-status-text');
            sub.menu.addMenuItem(text);

            const busy = this._busy.has(c.id);
            for (const action of c.actions) {
                const item = new PopupMenu.PopupMenuItem(busy ? `${action.label} (working…)`
                    : action.label);
                item.setSensitive(!busy);
                item.connect('activate', () => this._resolve(c, action.choice));
                sub.menu.addMenuItem(item);
            }
            const show = new PopupMenu.PopupMenuItem(c.savedPath ? 'Show the saved copy in Files'
                : 'Show in Files');
            show.connect('activate', () => this._showFolder(c.showFolder));
            sub.menu.addMenuItem(show);
            this._conflictSection.addMenuItem(sub);
        }
    }

    _showFolder(rel) {
        try {
            const path = rel ? `${GLib.get_home_dir()}/${rel}` : GLib.get_home_dir();
            Gio.AppInfo.launch_default_for_uri(GLib.filename_to_uri(path, null), null);
        } catch (e) {
            Main.notifyError('Could not open the folder', e.message);
        }
    }

    _resolve(conflict, choice) {
        let argv;
        try {
            argv = resolveArgv(conflict, choice);
        } catch (e) {
            Main.notifyError('Could not resolve the conflict', e.message);
            return;
        }
        this._busy.add(conflict.id);
        this.refresh();
        let proc;
        try {
            proc = Gio.Subprocess.new(argv,
                Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE);
        } catch (e) {
            this._busy.delete(conflict.id);
            this.refresh();
            Main.notifyError('Could not resolve the conflict',
                `nostr-home-status could not be started: ${e.message}`);
            return;
        }
        proc.communicate_utf8_async(null, this._cancellable, (p, res) => {
            let out = '';
            try {
                [, out] = p.communicate_utf8_finish(res);
            } catch (_e) {
                return; // cancelled: the extension was disabled
            }
            this._busy.delete(conflict.id);
            const outcome = resolveOutcome(p.get_exit_status(), out);
            if (outcome.ok) Main.notify(outcome.title, outcome.body);
            else Main.notifyError(outcome.title, outcome.body);
            this.refresh();
        });
    }
});

export default class NostrHomeStatusExtension extends Extension {
    enable() {
        this._indicator = new Indicator(this);
        Main.panel.addToStatusArea('nostr-home-status', this._indicator);
        this._indicator.startWatching();
    }

    disable() {
        if (this._indicator) {
            this._indicator.stopWatching();
            this._indicator.destroy();
            this._indicator = null;
        }
    }
}
