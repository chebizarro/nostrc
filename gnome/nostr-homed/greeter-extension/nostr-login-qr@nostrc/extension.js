// nostr-login-qr@nostrc — greeter/lock-screen extension
//
// While the nostr-authd broker has a pending NIP-46 QR-login pairing for the
// seat, it publishes a manifest + PNG under /run/nostr-auth/greeter/.  This
// extension watches for that manifest and shows the QR image + pairing code
// + hint on the greeter (and lock screen, if enabled) while the manifest is
// live; it hides again when the manifest disappears or its expires_at
// deadline passes.
//
// Producer contract (see README.md): the manifest is a JSON object with at
// least {tx_id, png, pairing_code, expires_at}.  All other fields are
// tolerated but not required.  A missing / malformed / empty manifest is
// treated as "nothing to show" — never as an error that could break the
// login dialog.

import St from 'gi://St';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Clutter from 'gi://Clutter';
import GdkPixbuf from 'gi://GdkPixbuf';
import Cogl from 'gi://Cogl';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const MANIFEST_DIR = '/run/nostr-auth/greeter';
const MANIFEST_NAME = 'current.json';
const DEFAULT_PNG_NAME = 'current.png';
const MANIFEST_PATH = `${MANIFEST_DIR}/${MANIFEST_NAME}`;
// QR display size in px.  ~300 gives a comfortable phone-scan target at
// 1280x800 while leaving room for the pairing code + hint under it without
// the card growing taller than the greeter's vertical safe area.
const IMAGE_DISPLAY_PX = 300;
// Outside margin between the card and the monitor's right edge (also used
// as a minimum gap from the login dialog on narrow screens).
const EDGE_MARGIN_PX = 40;
// Internal padding built into the stylesheet's `.nostr-login-qr-panel`
// (18px * 2) — kept in sync here so the panel width math is exact.
const PANEL_CHROME_PX = 36;
const MAX_MANIFEST_BYTES = 8 * 1024;

function _safeLog(msg) {
    try {
        log(`nostr-login-qr: ${msg}`);
    } catch (_e) {
        // GJS's global log() is always present in extensions; guard anyway.
    }
}

function _isBasename(name) {
    return typeof name === 'string'
        && name.length > 0
        && name.length < 128
        && !name.includes('/')
        && !name.includes('\0')
        && name !== '.'
        && name !== '..';
}

function _asString(v, max) {
    if (typeof v !== 'string') return '';
    if (v.length > max) return v.slice(0, max);
    return v;
}

export default class NostrLoginQrExtension extends Extension {
    enable() {
        this._container = null;
        this._imageBin = null;
        this._codeLabel = null;
        this._hintLabel = null;
        this._fileMonitor = null;
        this._dirMonitor = null;
        this._expiryTimeoutId = 0;
        this._debounceTimeoutId = 0;
        this._monitorsChangedId = 0;

        try {
            this._buildContainer();
            this._startWatching();
            this._monitorsChangedId = Main.layoutManager.connect(
                'monitors-changed', () => this._positionContainer());
            this._refresh();
        } catch (e) {
            // Defensive: never let a startup failure take down the greeter.
            logError(e, 'nostr-login-qr: enable() failed');
            this.disable();
        }
    }

    disable() {
        this._clearExpiryTimeout();
        this._clearDebounceTimeout();
        this._stopWatching();
        if (this._monitorsChangedId) {
            Main.layoutManager.disconnect(this._monitorsChangedId);
            this._monitorsChangedId = 0;
        }
        if (this._container) {
            if (this._container.get_parent())
                this._container.get_parent().remove_child(this._container);
            this._container.destroy();
            this._container = null;
        }
        this._imageBin = null;
        this._codeLabel = null;
        this._hintLabel = null;
    }

    _buildContainer() {
        const box = new St.BoxLayout({
            vertical: true,
            style_class: 'nostr-login-qr-panel',
            reactive: false,
            can_focus: false,
            track_hover: false,
            visible: false,
        });
        this._imageBin = new St.Bin({
            style_class: 'nostr-login-qr-image',
            width: IMAGE_DISPLAY_PX,
            height: IMAGE_DISPLAY_PX,
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._codeLabel = new St.Label({
            style_class: 'nostr-login-qr-pairing',
            text: '',
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._codeLabel.clutter_text.set_line_wrap(false);
        this._hintLabel = new St.Label({
            style_class: 'nostr-login-qr-hint',
            text: '',
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._hintLabel.clutter_text.set_line_wrap(true);
        box.add_child(this._imageBin);
        box.add_child(this._codeLabel);
        box.add_child(this._hintLabel);
        // uiGroup is the top-most stage group in the greeter; adding here
        // gives us a floating widget above the login dialog without touching
        // any of gnome-shell's internal actors.
        Main.layoutManager.uiGroup.add_child(box);
        this._container = box;
        this._positionContainer();
    }

    _positionContainer() {
        if (!this._container) return;
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor) return;

        // The QR card lives as its own block to the RIGHT of the primary
        // monitor, vertically centred.  The greeter's user/entry stack is
        // centred, so a right-side anchor never overlaps the display name
        // or the password field (which the earlier top-centre anchor did on
        // 1280x800 — see greeter-02-prompt.png in the QR extension review).
        //
        // On very narrow screens (< ~900 px wide) there isn't room to sit
        // beside the login stack without crowding it; in that case we fall
        // back to top-centre — which is only reached on unusual setups
        // because GDM itself needs comfortable width to render.
        const panelWidth = IMAGE_DISPLAY_PX + PANEL_CHROME_PX;
        const panelHeight = this._container.get_preferred_height(panelWidth)[1]
            || (IMAGE_DISPLAY_PX + 120);
        const narrow = monitor.width < panelWidth * 2 + 340;

        let x;
        let y;
        if (narrow) {
            x = monitor.x + Math.max(0, Math.floor((monitor.width - panelWidth) / 2));
            y = monitor.y + EDGE_MARGIN_PX;
        } else {
            x = monitor.x + monitor.width - panelWidth - EDGE_MARGIN_PX;
            y = monitor.y + Math.max(EDGE_MARGIN_PX,
                Math.floor((monitor.height - panelHeight) / 2));
        }
        this._container.set_position(x, y);
        this._container.set_width(panelWidth);
    }

    _startWatching() {
        // Watch the directory so we get CREATED events even when
        // /run/nostr-auth/greeter/current.json doesn't exist yet at enable().
        try {
            const dir = Gio.File.new_for_path(MANIFEST_DIR);
            if (dir.query_exists(null)) {
                this._dirMonitor = dir.monitor_directory(
                    Gio.FileMonitorFlags.WATCH_MOVES, null);
                this._dirMonitor.connect('changed',
                    (_m, file, _other, event) => this._onDirEvent(file, event));
            }
        } catch (e) {
            _safeLog(`dir monitor unavailable: ${e.message}`);
        }
        try {
            const manifest = Gio.File.new_for_path(MANIFEST_PATH);
            this._fileMonitor = manifest.monitor_file(
                Gio.FileMonitorFlags.NONE, null);
            this._fileMonitor.connect('changed',
                (_m, _f, _o, event) => this._onManifestEvent(event));
        } catch (e) {
            _safeLog(`file monitor unavailable: ${e.message}`);
        }
    }

    _stopWatching() {
        if (this._fileMonitor) {
            try { this._fileMonitor.cancel(); } catch (_e) {}
            this._fileMonitor = null;
        }
        if (this._dirMonitor) {
            try { this._dirMonitor.cancel(); } catch (_e) {}
            this._dirMonitor = null;
        }
    }

    _onDirEvent(file, event) {
        if (!file) {
            this._scheduleRefresh();
            return;
        }
        const name = file.get_basename();
        if (name === MANIFEST_NAME || name === DEFAULT_PNG_NAME)
            this._scheduleRefresh();
    }

    _onManifestEvent(_event) {
        this._scheduleRefresh();
    }

    _scheduleRefresh() {
        // Debounce: FileMonitor typically fires CREATED + CHANGED + CHANGES_DONE_HINT
        // for a single publish; coalesce them into one refresh 100ms later.
        this._clearDebounceTimeout();
        this._debounceTimeoutId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT, 100, () => {
                this._debounceTimeoutId = 0;
                this._refresh();
                return GLib.SOURCE_REMOVE;
            });
    }

    _clearDebounceTimeout() {
        if (this._debounceTimeoutId) {
            GLib.source_remove(this._debounceTimeoutId);
            this._debounceTimeoutId = 0;
        }
    }

    _clearExpiryTimeout() {
        if (this._expiryTimeoutId) {
            GLib.source_remove(this._expiryTimeoutId);
            this._expiryTimeoutId = 0;
        }
    }

    _refresh() {
        this._clearExpiryTimeout();
        try {
            this._refreshUnsafe();
        } catch (e) {
            logError(e, 'nostr-login-qr: refresh failed');
            this._hide();
        }
    }

    _refreshUnsafe() {
        const manifestFile = Gio.File.new_for_path(MANIFEST_PATH);
        if (!manifestFile.query_exists(null)) {
            this._hide();
            return;
        }
        let manifest;
        try {
            const [ok, data] = manifestFile.load_contents(null);
            if (!ok || !data || data.length === 0 || data.length > MAX_MANIFEST_BYTES) {
                this._hide();
                return;
            }
            const text = new TextDecoder('utf-8', {fatal: false}).decode(data);
            manifest = JSON.parse(text);
        } catch (e) {
            _safeLog(`manifest read/parse failed: ${e.message}`);
            this._hide();
            return;
        }
        if (manifest === null || typeof manifest !== 'object' || Array.isArray(manifest)) {
            this._hide();
            return;
        }

        const nowSec = Math.floor(GLib.get_real_time() / 1000000);
        const expiresAt = Number(manifest.expires_at);
        const hasExpiry = Number.isFinite(expiresAt) && expiresAt > 0;
        if (hasExpiry && expiresAt <= nowSec) {
            this._hide();
            return;
        }

        // Resolve PNG name: default to current.png, but honour manifest.png
        // if it is a plain basename (no slashes, no traversal).
        let pngName = DEFAULT_PNG_NAME;
        if (typeof manifest.png === 'string' && manifest.png.length > 0) {
            if (_isBasename(manifest.png)) {
                pngName = manifest.png;
            } else {
                _safeLog(`rejecting png field with unsafe value; falling back to ${DEFAULT_PNG_NAME}`);
            }
        }
        const pngPath = `${MANIFEST_DIR}/${pngName}`;
        if (!GLib.file_test(pngPath, GLib.FileTest.EXISTS)) {
            this._hide();
            return;
        }
        const imageActor = this._loadPngActor(pngPath, IMAGE_DISPLAY_PX);
        if (!imageActor) {
            this._hide();
            return;
        }
        this._imageBin.set_child(imageActor);

        const code = _asString(manifest.pairing_code, 64);
        const hint = _asString(manifest.hint, 256) || 'Scan with your Nostr signer';
        this._codeLabel.text = code ? `Pairing code: ${code}` : '';
        this._codeLabel.visible = code.length > 0;
        this._hintLabel.text = hint;
        this._hintLabel.visible = hint.length > 0;

        this._positionContainer();
        this._container.show();
        this._container.visible = true;

        if (hasExpiry) {
            const remainingSec = Math.max(1, expiresAt - nowSec);
            // Clamp to a sane upper bound so a rogue manifest can't schedule
            // a multi-day timeout.
            const ms = Math.min(remainingSec, 24 * 3600) * 1000;
            this._expiryTimeoutId = GLib.timeout_add(
                GLib.PRIORITY_DEFAULT, ms, () => {
                    this._expiryTimeoutId = 0;
                    this._refresh();
                    return GLib.SOURCE_REMOVE;
                });
        }
    }

    _hide() {
        if (this._container) {
            this._container.visible = false;
            this._container.hide();
        }
        if (this._imageBin) {
            this._imageBin.set_child(null);
        }
    }

    _loadPngActor(path, displaySize) {
        let pixbuf;
        try {
            pixbuf = GdkPixbuf.Pixbuf.new_from_file(path);
        } catch (e) {
            _safeLog(`pixbuf load failed for ${path}: ${e.message}`);
            return null;
        }
        if (!pixbuf) return null;
        const width = pixbuf.get_width();
        const height = pixbuf.get_height();
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            _safeLog(`refusing implausible pixbuf size ${width}x${height}`);
            return null;
        }
        const image = new Clutter.Image();
        const hasAlpha = pixbuf.get_has_alpha();
        const fmt = hasAlpha
            ? Cogl.PixelFormat.RGBA_8888
            : Cogl.PixelFormat.RGB_888;
        let ok = false;
        try {
            ok = image.set_data(
                pixbuf.get_pixels(),
                fmt,
                width,
                height,
                pixbuf.get_rowstride());
        } catch (e) {
            _safeLog(`Clutter.Image.set_data threw: ${e.message}`);
            return null;
        }
        if (!ok) {
            _safeLog('Clutter.Image.set_data returned false');
            return null;
        }
        const actor = new Clutter.Actor({
            width: displaySize,
            height: displaySize,
            reactive: false,
        });
        actor.set_content(image);
        // Nearest-neighbour so QR modules stay crisp when we upscale from
        // e.g. 41x41 native to 300x300 display.
        try {
            actor.set_content_scaling_filters(
                Clutter.ScalingFilter.NEAREST,
                Clutter.ScalingFilter.NEAREST);
        } catch (_e) {
            // Older Clutter may only expose per-filter setters; ignore.
        }
        return actor;
    }
}
