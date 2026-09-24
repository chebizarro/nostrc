// nostr-home-status@nostrc — user-session panel indicator.
//
// Watches $XDG_STATE_HOME/nostr-homed/porthome-status.json (falling
// back to ~/.local/state/nostr-homed/porthome-status.json) via
// Gio.FileMonitor and renders a compact status glyph plus a popup
// with mount state, sync state, cache bytes, and any conflict count.
//
// NEVER surfaces secret material — the status file schema is a
// deliberately narrow set of counters/state strings; if the daemon
// ever expands the schema this extension only reads the well-known
// keys defined below.
//
// Bead: nostrc-h10m.1 (Phase 5 I1).

import St from 'gi://St';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Clutter from 'gi://Clutter';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main         from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu    from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu    from 'resource:///org/gnome/shell/ui/popupMenu.js';

const MAX_FILE_BYTES = 128 * 1024;

// Pull XDG_STATE_HOME or fall back to ~/.local/state.
function _statusPath() {
    const xdg = GLib.getenv('XDG_STATE_HOME');
    const home = GLib.get_home_dir();
    if (xdg && xdg.length > 0) {
        return `${xdg}/nostr-homed/porthome-status.json`;
    }
    return `${home}/.local/state/nostr-homed/porthome-status.json`;
}

function _readFileText(path) {
    try {
        const file = Gio.File.new_for_path(path);
        const [ok, bytes] = file.load_contents(null);
        if (!ok || !bytes) return null;
        if (bytes.length > MAX_FILE_BYTES) return null;
        const decoder = new TextDecoder();
        return decoder.decode(bytes);
    } catch (_e) {
        return null;
    }
}

function _safeParse(text) {
    if (!text) return null;
    try { return JSON.parse(text); }
    catch (_e) { return null; }
}

function _human(n) {
    if (typeof n !== 'number' || !Number.isFinite(n)) return '—';
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
    if (n < 1024 * 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} MiB`;
    return `${(n / (1024 * 1024 * 1024)).toFixed(2)} GiB`;
}

// Choose a panel glyph for a given syncd state.
function _syncGlyph(state) {
    switch (state) {
        case 'ready':   return '●';
        case 'partial': return '◐';
        case 'limited': return '○';
        default:        return '?';
    }
}

class Indicator extends PanelMenu.Button {
    constructor(ext) {
        super(0.0, 'nostr-home-status', false);
        this._ext = ext;
        this._monitor = null;

        const box = new St.BoxLayout({
            style_class: 'panel-status-menu-box',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._icon = new St.Label({
            text: '?',
            style_class: 'system-status-icon nostr-home-status-glyph',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._label = new St.Label({
            text: 'home',
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'nostr-home-status-label',
        });
        box.add_child(this._icon);
        box.add_child(this._label);
        this.add_child(box);

        // Popup rows are simple label pairs; rebuild on refresh.
        this._rows = {};
        for (const k of ['sync', 'mount', 'cache', 'conflicts',
                          'generation', 'last_error']) {
            const item = new PopupMenu.PopupMenuItem('', {reactive: false});
            item.label.text = `${k}: —`;
            this._rows[k] = item;
            this.menu.addMenuItem(item);
        }

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._pathItem = new PopupMenu.PopupMenuItem(_statusPath(),
                                                     {reactive: false});
        this._pathItem.label.style_class = 'nostr-home-status-pathrow';
        this.menu.addMenuItem(this._pathItem);
    }

    startWatching() {
        const path = _statusPath();
        try {
            const file = Gio.File.new_for_path(path);
            this._monitor = file.monitor(Gio.FileMonitorFlags.NONE, null);
            this._monitor.connect('changed', () => this.refresh());
        } catch (_e) {
            // Best-effort — poll below covers the missing-monitor case.
        }
        this._pollId = GLib.timeout_add_seconds(
            GLib.PRIORITY_DEFAULT, 15,
            () => { this.refresh(); return GLib.SOURCE_CONTINUE; });
        this.refresh();
    }

    stopWatching() {
        if (this._monitor) { this._monitor.cancel(); this._monitor = null; }
        if (this._pollId)   { GLib.source_remove(this._pollId); this._pollId = 0; }
    }

    refresh() {
        const doc = _safeParse(_readFileText(_statusPath())) || {};
        const syncd = doc.syncd || {};
        const fuse  = doc.fuse  || {};

        const syncState = typeof syncd.state === 'string' ? syncd.state : '—';
        this._icon.text  = _syncGlyph(syncState);
        this._label.text = syncState !== '—' ? syncState : 'home';

        this._rows.sync.label.text  = `sync: ${syncState}`;
        this._rows.mount.label.text = `mount: ${fuse.mounted === true ? 'mounted' : 'unmounted'}` +
                                       (typeof fuse.mountpoint === 'string' && fuse.mountpoint.length > 0 ?
                                          `  (${fuse.mountpoint})` : '');
        this._rows.cache.label.text = `cache: ${_human(syncd.cache_bytes)}` +
                                       (typeof syncd.quota_bytes === 'number' && syncd.quota_bytes > 0 ?
                                          ` / ${_human(syncd.quota_bytes)}` : '');
        this._rows.conflicts.label.text = `conflicts: ${syncd.conflicts_active_count || 0}`;
        this._rows.generation.label.text =
            `generation: push=${syncd.last_push_gen ?? '—'} pull=${syncd.last_pull_gen ?? '—'}` +
            (typeof fuse.generation === 'number' ? ` fuse=${fuse.generation}` : '');
        this._rows.last_error.label.text = `last_error: ${syncd.last_error_class || 'none'}`;
    }
}

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
