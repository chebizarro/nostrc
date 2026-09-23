// nostr-login-qr@nostrc — greeter/lock-screen extension
//
// While the nostr-authd broker has a pending NIP-46 QR-login pairing for the
// seat, it publishes a manifest + PNG under /run/nostr-auth/greeter/.  This
// extension watches for that manifest and, while it is live, replaces the
// password entry in the greeter's LoginDialog (or the lock screen's
// UnlockDialog) with a centered QR card carrying the QR image, the pairing
// code, and a short hint.  When the manifest retires (login done, timeout,
// cancel / Escape) the original entry is restored exactly as it was.
//
// The extension NEVER mutates the login/unlock dialog outside the window
// during which the artifact is live, and if the dialog's AuthPrompt cannot
// be located (unknown shell version, transient state) it falls back to a
// non-intrusive floating card off to the right of the primary monitor so
// the QR is still scannable.  The underlying PAM stack is never touched.
//
// Producer contract (see README.md): the manifest is a JSON object with at
// least {tx_id, png, pairing_code, expires_at}.  All other fields are
// tolerated but not required.  A missing / malformed / empty manifest is
// treated as "nothing to show" — never as an error that could break the
// login/unlock dialog.

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
// QR display size in px.  260 keeps the card + pairing code line under the
// AuthPrompt vertical band at 1280x800 without colliding with the Ubuntu
// branding logo directly beneath the greeter dialog.  Still nearest-
// neighbour, still comfortably scannable by a phone camera at that pixel
// density (verified with `zbarimg` on the framebuffer capture).
const IMAGE_DISPLAY_PX = 260;
// When the manifest carries an `account` block we render an avatar +
// display_name + identifier above the QR (replacing the dialog's own
// avatar / username, which was looked up before PAM canonicalised the
// name — see docs/reviews/greeter-qr-account-identity-2026-09-22.md).
// Shrink the QR a touch to keep the whole stack clear of the Ubuntu
// branding logo at 1280x800; 220 still decodes cleanly with `zbarimg`.
const IMAGE_DISPLAY_PX_WITH_ACCOUNT = 220;
const AVATAR_DISPLAY_PX = 100;
// Bounds on user-supplied identity strings from the manifest.  Kept short
// so a rogue producer can't push a giant label into the AuthPrompt band.
const ACCOUNT_LABEL_MAX = 128;
const ACCOUNT_IDENTIFIER_MAX = 128;
const ACCOUNT_USERNAME_MAX = 64;
// Outside margin between the floating fallback card and the monitor's right
// edge (also used as a minimum gap from the login dialog on narrow screens).
const EDGE_MARGIN_PX = 40;
// Internal padding built into the stylesheet's `.nostr-login-qr-panel-floating`
// (18px * 2) — kept in sync here so the panel width math is exact.
const PANEL_CHROME_PX = 36;
const MAX_MANIFEST_BYTES = 8 * 1024;
// After a publish, the AuthPrompt may not be on-screen yet (e.g. the greeter
// is still on the user list, or a stale unlock dialog is being rebuilt).
// Try to re-attach a few times, spaced apart, before giving up and falling
// back to the floating card.
const CENTER_ATTACH_RETRY_MS = 300;
const CENTER_ATTACH_RETRIES = 8;

// Style classes we look for inside the shell's LoginDialog / UnlockDialog.
// Both dialogs host an AuthPrompt whose top-level BoxLayout carries
// `login-dialog-prompt-layout`, and whose St.Entry (either _passwordEntry
// or _textEntry) carries `login-dialog-prompt-entry`.  Confirmed against
// gnome-shell 46's compiled libshell-14.so.
const AUTH_PROMPT_CLASS = 'login-dialog-prompt-layout';
const AUTH_ENTRY_CLASS = 'login-dialog-prompt-entry';
// UserWidget (avatar + username) that AuthPrompt renders above the entry.
// gnome-shell's userWidget.js applies `user-widget` on the container that
// holds the avatar + label; shell 46's greeter carries it verbatim.  We
// hide any descendant carrying this class while an account artifact is
// live so the dialog does not double-render the identity above our own
// avatar block.  Restored verbatim on retire.
const USER_WIDGET_CLASSES = ['user-widget'];
// Label inside a gnome-shell UserWidget — `_label` in userWidget.js carries
// this class.  We read its text to decide whether the native widget already
// names the resolved account (tile-click flow) or is showing a stale typed
// string (NIP-05 flow).
const USER_WIDGET_LABEL_CLASS = 'user-widget-label';
// Message labels the shell renders as PAM_TEXT_INFO / PAM_ERROR_MSG.
// We hide any that duplicate the pairing code the card already displays.
const AUTH_MESSAGE_CLASSES = [
    'login-dialog-message',
    'login-dialog-message-hint',
    'login-dialog-message-warning',
];

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

function _hasStyleClass(actor, cls) {
    if (!actor) return false;
    let s;
    try { s = actor.style_class; } catch (_e) { return false; }
    if (typeof s !== 'string' || s.length === 0) return false;
    // St actor style_class is a space-separated list; match by token, not
    // by substring, so `login-dialog-prompt-entry` doesn't accidentally
    // match `login-dialog-prompt-layout`.
    return s.split(/\s+/).indexOf(cls) !== -1;
}

function _findDescendantByStyleClass(root, cls, skip) {
    if (!root || root === skip) return null;
    if (_hasStyleClass(root, cls)) return root;
    let children = [];
    try { children = root.get_children(); } catch (_e) { return null; }
    for (const c of children) {
        const hit = _findDescendantByStyleClass(c, cls, skip);
        if (hit) return hit;
    }
    return null;
}

function _collectDescendantsByAnyClass(root, classes, out, skip) {
    if (!root || root === skip) return;
    for (const cls of classes) {
        if (_hasStyleClass(root, cls)) {
            out.push(root);
            break;
        }
    }
    let children = [];
    try { children = root.get_children(); } catch (_e) { return; }
    for (const c of children)
        _collectDescendantsByAnyClass(c, classes, out, skip);
}

export default class NostrLoginQrExtension extends Extension {
    enable() {
        this._container = null;
        this._imageBin = null;
        this._codeLabel = null;
        this._hintLabel = null;
        this._accountBox = null;
        this._avatarBin = null;
        this._displayNameLabel = null;
        this._identifierLabel = null;
        this._fileMonitor = null;
        this._dirMonitor = null;
        this._expiryTimeoutId = 0;
        this._debounceTimeoutId = 0;
        this._monitorsChangedId = 0;
        this._retryTimeoutId = 0;
        this._retryCount = 0;
        this._centered = false;
        this._centerHost = null;
        this._hiddenActors = [];
        this._suppressedMessages = [];
        this._suppressedUserWidgets = [];
        this._entryDestroyId = 0;
        this._hostEntry = null;
        this._hintPrevVisible = undefined;
        // Set by _applyAccount from the current manifest so _attachCentered
        // knows whether to also hide the dialog's own avatar / username.
        // Null → v2 behaviour (leave dialog identity alone).
        this._pendingAccount = null;
        // Actual QR size in effect for the current publish; may drop from
        // IMAGE_DISPLAY_PX to IMAGE_DISPLAY_PX_WITH_ACCOUNT when an account
        // block is above it so the whole stack still clears the Ubuntu logo.
        this._currentQrSize = IMAGE_DISPLAY_PX;

        try {
            this._buildContainer();
            this._startWatching();
            this._monitorsChangedId = Main.layoutManager.connect(
                'monitors-changed', () => this._positionFloating());
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
        this._clearRetryTimeout();
        this._stopWatching();
        if (this._monitorsChangedId) {
            Main.layoutManager.disconnect(this._monitorsChangedId);
            this._monitorsChangedId = 0;
        }
        this._detachCentered();
        if (this._container) {
            if (this._container.get_parent())
                this._container.get_parent().remove_child(this._container);
            this._container.destroy();
            this._container = null;
        }
        this._imageBin = null;
        this._codeLabel = null;
        this._hintLabel = null;
        this._accountBox = null;
        this._avatarBin = null;
        this._displayNameLabel = null;
        this._identifierLabel = null;
    }

    _buildContainer() {
        const box = new St.BoxLayout({
            vertical: true,
            style_class: 'nostr-login-qr-panel nostr-login-qr-panel-floating',
            reactive: false,
            can_focus: false,
            track_hover: false,
            visible: false,
            x_align: Clutter.ActorAlign.CENTER,
        });
        // Account identity block — avatar + display name + identifier.
        // Rendered above the QR when the manifest's `account` object is
        // present; kept hidden otherwise so a manifest with no account
        // behaves byte-identically to v2.
        this._accountBox = new St.BoxLayout({
            vertical: true,
            style_class: 'nostr-login-qr-account',
            visible: false,
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._avatarBin = new St.Bin({
            style_class: 'nostr-login-qr-avatar',
            width: AVATAR_DISPLAY_PX,
            height: AVATAR_DISPLAY_PX,
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._displayNameLabel = new St.Label({
            style_class: 'nostr-login-qr-display-name',
            text: '',
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._displayNameLabel.clutter_text.set_line_wrap(false);
        this._identifierLabel = new St.Label({
            style_class: 'nostr-login-qr-identifier',
            text: '',
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._identifierLabel.clutter_text.set_line_wrap(false);
        this._accountBox.add_child(this._avatarBin);
        this._accountBox.add_child(this._displayNameLabel);
        this._accountBox.add_child(this._identifierLabel);
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
        box.add_child(this._accountBox);
        box.add_child(this._imageBin);
        box.add_child(this._codeLabel);
        box.add_child(this._hintLabel);
        // uiGroup is the top-most stage group in the greeter/lock screen;
        // adding here gives us a floating fallback slot above the dialog
        // without touching any of gnome-shell's internal actors.  We may
        // later reparent this container into the AuthPrompt while a
        // pairing is live; on retire we move it back here.
        Main.layoutManager.uiGroup.add_child(box);
        this._container = box;
        this._positionFloating();
    }

    _positionFloating() {
        if (!this._container || this._centered) return;
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor) return;

        // Right-side fallback card, vertically centred.  The greeter/lock
        // AuthPrompt is centred, so a right-side anchor never overlaps the
        // display name or the password field.  On very narrow screens
        // (< ~900 px wide) there isn't room to sit beside the login stack
        // without crowding it; in that case we fall back to top-centre.
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

    _clearRetryTimeout() {
        if (this._retryTimeoutId) {
            GLib.source_remove(this._retryTimeoutId);
            this._retryTimeoutId = 0;
        }
        this._retryCount = 0;
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

        // Resolve the optional account block first so we can size the QR
        // down when we also have identity to render above it.  A missing,
        // malformed or empty account object leaves v2 behaviour intact.
        //
        // Then decide whether to actually override GDM's own user widget.
        // In the tile-click flow (user picked their tile at the greeter, or
        // the unlock dialog was populated from the live session) GDM's
        // avatar + name is already correct — we should leave it alone and
        // just insert the QR + pairing code below.  Only override when the
        // dialog would otherwise show the wrong identity (NIP-05 flow: the
        // typed string + a generic icon), when the visible label doesn't
        // match the resolved account, or when we can't find a native user
        // widget to defer to.  Passing null through the rest of the pipeline
        // yields v2 behaviour: no identity block from us, QR back at 260 px,
        // native user widget untouched.
        const rawAccount = this._extractAccount(manifest);
        const account = this._resolveAccountOverride(rawAccount);
        const qrSize = account ? IMAGE_DISPLAY_PX_WITH_ACCOUNT : IMAGE_DISPLAY_PX;
        this._currentQrSize = qrSize;
        try {
            this._imageBin.width = qrSize;
            this._imageBin.height = qrSize;
        } catch (_e) {}

        const imageActor = this._loadPngActor(pngPath, qrSize);
        if (!imageActor) {
            this._hide();
            return;
        }
        this._imageBin.set_child(imageActor);
        this._applyAccount(account);

        const code = _asString(manifest.pairing_code, 64);
        const hint = _asString(manifest.hint, 256) || 'Scan with your Nostr signer';
        this._codeLabel.text = code ? `Pairing code: ${code}` : '';
        this._codeLabel.visible = code.length > 0;
        this._hintLabel.text = hint;
        this._hintLabel.visible = hint.length > 0;

        // Preferred: replace the password entry inside the current dialog.
        // Fallback: keep the floating card behaviour so nothing regresses
        // if we can't find the AuthPrompt (unusual shell version, dialog
        // still being built, etc.).
        this._clearRetryTimeout();
        const attached = this._attachCentered(code);
        if (!attached) {
            this._detachCentered();
            this._positionFloating();
            this._container.show();
            this._container.visible = true;
            // Retry attaching centered for a short window in case the
            // AuthPrompt is still being built when we publish (fresh
            // unlock dialog, greeter transitioning from user list).
            this._scheduleCenterRetry(code);
        } else if (this._hintLabel) {
            // Inline mode always hides the hint — _refreshUnsafe reset it
            // to visible above and _attachCentered's early-return path
            // skips hiding when we're already centered from a prior refresh
            // (e.g. broker pushing a new pairing_code).  Re-hide here so
            // repeated refreshes stay consistent with the first attach.
            try { this._hintLabel.visible = false; } catch (_e) {}
        }

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

    _scheduleCenterRetry(code) {
        if (this._retryCount >= CENTER_ATTACH_RETRIES) return;
        this._retryCount += 1;
        this._retryTimeoutId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT, CENTER_ATTACH_RETRY_MS, () => {
                this._retryTimeoutId = 0;
                // The manifest may have retired in the meantime — only try
                // to attach if we're still supposed to be showing.
                if (!this._container || !this._container.visible) return GLib.SOURCE_REMOVE;
                if (this._attachCentered(code)) return GLib.SOURCE_REMOVE;
                this._scheduleCenterRetry(code);
                return GLib.SOURCE_REMOVE;
            });
    }

    _findAuthPrompt() {
        // Look for the currently active AuthPrompt.  Both LoginDialog (gdm
        // mode) and UnlockDialog (unlock-dialog mode) host one, both use
        // the same `login-dialog-prompt-layout` style class, and both live
        // under global.stage.  We skip our own container so a bug in
        // stylesheet.css that leaves the class token dangling can never
        // match us.
        return _findDescendantByStyleClass(
            global.stage, AUTH_PROMPT_CLASS, this._container);
    }

    _findEntryIn(authPrompt) {
        return _findDescendantByStyleClass(
            authPrompt, AUTH_ENTRY_CLASS, this._container);
    }

    _attachCentered(pairingCode) {
        if (this._centered) {
            // Already attached; nothing to do beyond re-showing.
            this._container.show();
            this._container.visible = true;
            return true;
        }
        const authPrompt = this._findAuthPrompt();
        if (!authPrompt) return false;
        // AuthPrompt must be on-screen for us to be visible inside it.  A
        // hidden AuthPrompt (greeter idling on the user list, unlock
        // dialog still building) means "try again shortly".
        let visible;
        try { visible = authPrompt.visible && authPrompt.get_stage() !== null; } catch (_e) { visible = false; }
        if (!visible) return false;

        const entry = this._findEntryIn(authPrompt);
        if (!entry) return false;

        // Walk from the entry up through its ancestor chain until we reach
        // the direct child of the AuthPrompt.  The AuthPrompt is a vertical
        // BoxLayout whose children are centered on the dialog's horizontal
        // axis (the avatar / username sit in the same stack).  The entry
        // itself lives inside an inner horizontal row that also carries the
        // "back" (`‹`) button + eye-toggle, so if we insert at the entry's
        // slot the card inherits that row's leftward offset (~24 px on
        // shell 46 at 1280x800).  Inserting at the row's slot in the
        // AuthPrompt keeps the card centered under the avatar / username.
        let host = entry;
        let hostParent = host.get_parent();
        while (hostParent && hostParent !== authPrompt) {
            host = hostParent;
            hostParent = host.get_parent();
        }
        if (hostParent !== authPrompt || !host) {
            // Entry isn't a descendant of the AuthPrompt we found —
            // extremely unlikely, but bail rather than dance.
            return false;
        }

        const siblings = authPrompt.get_children();
        let insertIndex = 0;
        for (let i = 0; i < siblings.length; i++) {
            if (siblings[i] === host) { insertIndex = i; break; }
        }

        // Track what we hide so we can restore it verbatim on retire.
        this._hiddenActors = [];
        // Hide the whole entry row (`host`).  On shell 46 this is the
        // horizontal BoxLayout containing [back-button][entry][eye-toggle].
        // Escape still cancels the flow (AuthPrompt intercepts key events
        // regardless of the row's visibility), so we don't lose the cancel
        // affordance functionally — only visually, which is the price of
        // getting the QR properly centered under the avatar.
        try { host.visible = false; } catch (_e) {}
        this._hiddenActors.push(host);

        // If we have a resolved account, hide the dialog's own avatar +
        // username block: it was populated from the string the user typed
        // at "Not listed?" (e.g. the raw NIP-05 "chebizarro@coinos.io")
        // and a generic avatar, before pam_nostr canonicalised the name
        // via the broker.  We render the true account identity ourselves
        // inside our container.  Restored verbatim on retire.
        this._suppressedUserWidgets = [];
        if (this._pendingAccount) {
            const userWidgets = [];
            _collectDescendantsByAnyClass(
                authPrompt,
                USER_WIDGET_CLASSES,
                userWidgets,
                this._container);
            for (const w of userWidgets) {
                let wasVisible = true;
                try { wasVisible = w.visible; } catch (_e) {}
                try { w.visible = false; } catch (_e) {}
                this._suppressedUserWidgets.push({actor: w, visible: wasVisible});
            }
        }

        // Hide PAM message labels that duplicate the card's pairing code
        // or its hint.  Never touch message-warning labels (real errors
        // like "Sorry, that didn't work" must stay visible).
        this._suppressedMessages = [];
        const messageActors = [];
        _collectDescendantsByAnyClass(
            authPrompt,
            ['login-dialog-message', 'login-dialog-message-hint'],
            messageActors,
            this._container);
        const pcNeedle = pairingCode ? pairingCode.toLowerCase() : '';
        for (const m of messageActors) {
            let text = '';
            try {
                text = (m.text || (m.clutter_text && m.clutter_text.text) || '').toString();
            } catch (_e) {}
            const lower = text.toLowerCase();
            const dupCode = pcNeedle && lower.indexOf(pcNeedle) !== -1;
            const dupHint = lower.indexOf('scan the qr') !== -1 || lower.indexOf('pairing code') !== -1;
            if (dupCode || dupHint) {
                let wasVisible = true;
                try { wasVisible = m.visible; } catch (_e) {}
                try { m.visible = false; } catch (_e) {}
                this._suppressedMessages.push({actor: m, visible: wasVisible});
            }
        }

        // Move our container from the floating slot into the AuthPrompt.
        try {
            if (this._container.get_parent())
                this._container.get_parent().remove_child(this._container);
        } catch (_e) {}
        try {
            this._container.remove_style_class_name('nostr-login-qr-panel-floating');
        } catch (_e) {}
        try {
            this._container.add_style_class_name('nostr-login-qr-panel-inline');
        } catch (_e) {}
        // Reset floating-mode geometry so the layout inside the AuthPrompt
        // can size us naturally.
        try {
            this._container.set_position(0, 0);
            this._container.set_size(-1, -1);
        } catch (_e) {}
        try {
            this._container.x_align = Clutter.ActorAlign.CENTER;
        } catch (_e) {}
        // Drop the hint line in inline mode.  At 1280x800 the QR + pairing
        // code + hint would collide with the Ubuntu branding logo drawn
        // directly below the dialog; the PAM stack still emits a short
        // "scan the QR" message that the shell shows elsewhere, and we
        // already suppress that above only when it duplicates the pairing
        // code.  On retire we restore the hint's prior visibility so the
        // floating fallback (when it comes back into use) still shows it.
        if (this._hintLabel) {
            let hintVis = true;
            try { hintVis = this._hintLabel.visible; } catch (_e) {}
            this._hintPrevVisible = hintVis;
            try { this._hintLabel.visible = false; } catch (_e) {}
        }
        try {
            authPrompt.insert_child_at_index(this._container, insertIndex);
        } catch (e) {
            _safeLog(`insert_child_at_index failed: ${e.message}`);
            // Best-effort restore + bail: caller will retry / fall back.
            this._restoreHiddenActors();
            if (this._hintLabel && this._hintPrevVisible !== undefined) {
                try { this._hintLabel.visible = this._hintPrevVisible; } catch (_e2) {}
                this._hintPrevVisible = undefined;
            }
            try { Main.layoutManager.uiGroup.add_child(this._container); } catch (_e2) {}
            try {
                this._container.remove_style_class_name('nostr-login-qr-panel-inline');
                this._container.add_style_class_name('nostr-login-qr-panel-floating');
            } catch (_e2) {}
            return false;
        }

        this._centered = true;
        this._centerHost = authPrompt;
        this._hostEntry = entry;
        // If the AuthPrompt is torn down (user cancels, dialog rebuilds)
        // we must proactively detach so we don't try to touch destroyed
        // actors on the retire refresh.
        try {
            this._entryDestroyId = authPrompt.connect(
                'destroy', () => this._onHostDestroyed());
        } catch (_e) {
            this._entryDestroyId = 0;
        }

        this._container.show();
        this._container.visible = true;
        return true;
    }

    _onHostDestroyed() {
        // Host went away out from under us (e.g. LoginDialog rebuilt).
        // Drop our references; the container itself is destroyed with the
        // parent so we don't need to remove it.  Clear tracking so the
        // next refresh treats us as "not attached".
        this._entryDestroyId = 0;
        this._centered = false;
        this._centerHost = null;
        this._hostEntry = null;
        this._hiddenActors = [];
        this._suppressedMessages = [];
        // User-widget tracking references the dead AuthPrompt subtree; drop
        // it too so we never try to restore visibility on a destroyed actor.
        this._suppressedUserWidgets = [];
        this._pendingAccount = null;
        this._hintPrevVisible = undefined;
        // Rebuild the container from scratch so a subsequent publish can
        // reattach.  Best-effort: if _buildContainer throws we log and
        // give up gracefully.
        this._container = null;
        this._imageBin = null;
        this._codeLabel = null;
        this._hintLabel = null;
        this._accountBox = null;
        this._avatarBin = null;
        this._displayNameLabel = null;
        this._identifierLabel = null;
        try {
            this._buildContainer();
        } catch (e) {
            logError(e, 'nostr-login-qr: container rebuild after host destroy failed');
        }
    }

    _restoreHiddenActors() {
        for (const a of this._hiddenActors) {
            try { a.visible = true; } catch (_e) {}
        }
        this._hiddenActors = [];
        for (const s of this._suppressedMessages) {
            try { s.actor.visible = s.visible; } catch (_e) {}
        }
        this._suppressedMessages = [];
        // Restore the dialog's own user widget (avatar + username) to its
        // prior visibility.  Tracked per-actor so an already-hidden widget
        // (e.g. dialog rebuilding) stays hidden rather than being turned
        // on by our restore path.
        for (const s of this._suppressedUserWidgets) {
            try { s.actor.visible = s.visible; } catch (_e) {}
        }
        this._suppressedUserWidgets = [];
    }

    _detachCentered() {
        if (!this._centered) return;
        if (this._centerHost && this._entryDestroyId) {
            try { this._centerHost.disconnect(this._entryDestroyId); } catch (_e) {}
        }
        this._entryDestroyId = 0;
        this._restoreHiddenActors();
        if (this._hintLabel && this._hintPrevVisible !== undefined) {
            try { this._hintLabel.visible = this._hintPrevVisible; } catch (_e) {}
            this._hintPrevVisible = undefined;
        }
        if (this._container) {
            try {
                if (this._container.get_parent())
                    this._container.get_parent().remove_child(this._container);
            } catch (_e) {}
            try {
                this._container.remove_style_class_name('nostr-login-qr-panel-inline');
                this._container.add_style_class_name('nostr-login-qr-panel-floating');
            } catch (_e) {}
            try {
                Main.layoutManager.uiGroup.add_child(this._container);
            } catch (_e) {}
            this._positionFloating();
        }
        this._centered = false;
        this._centerHost = null;
        this._hostEntry = null;
    }

    _hide() {
        this._clearRetryTimeout();
        this._detachCentered();
        // Clear account state so a subsequent publish starts fresh; the
        // account block collapses to invisible until _applyAccount decides
        // to show it again.
        this._applyAccount(null);
        // Restore the QR bin to the base size so a subsequent publish with
        // no account object doesn't inherit the shrunken size.
        this._currentQrSize = IMAGE_DISPLAY_PX;
        if (this._imageBin) {
            try {
                this._imageBin.width = IMAGE_DISPLAY_PX;
                this._imageBin.height = IMAGE_DISPLAY_PX;
            } catch (_e) {}
        }
        if (this._container) {
            this._container.visible = false;
            this._container.hide();
        }
        if (this._imageBin) {
            this._imageBin.set_child(null);
        }
    }

    _extractAccount(manifest) {
        // Returns {display_name, identifier, avatar_path} when the manifest
        // carries a usable `account` object, else null.  Malformed / partial
        // account data never throws — the QR flow keeps working with v2
        // behaviour (no identity block).
        const acc = manifest ? manifest.account : null;
        if (!acc || typeof acc !== 'object' || Array.isArray(acc))
            return null;

        const username = _asString(acc.username, ACCOUNT_USERNAME_MAX);
        const displayName = _asString(acc.display_name, ACCOUNT_LABEL_MAX);
        const identifier = _asString(acc.identifier, ACCOUNT_IDENTIFIER_MAX);
        // Fall back to the account username when display_name is missing —
        // matches the contract's "any field may be missing" guarantee.
        const label = displayName || username;

        let avatarPath = null;
        if (typeof acc.avatar === 'string' && acc.avatar.length > 0) {
            // Never load an image from outside the greeter drop directory:
            // basename-only (no slashes, no traversal, no NUL), then join
            // with MANIFEST_DIR.
            if (_isBasename(acc.avatar)) {
                const p = `${MANIFEST_DIR}/${acc.avatar}`;
                if (GLib.file_test(p, GLib.FileTest.EXISTS))
                    avatarPath = p;
            } else {
                _safeLog('rejecting account.avatar with unsafe value');
            }
        }

        // Require at least one field to be meaningful — an entirely empty
        // account object is treated as "no account" so we don't hide the
        // dialog's own user widget in exchange for nothing.
        if (!label && !identifier && !avatarPath)
            return null;

        return {
            display_name: label,
            identifier,
            avatar_path: avatarPath,
        };
    }

    _applyAccount(account) {
        this._pendingAccount = account || null;
        if (!this._accountBox) return;
        if (!account) {
            try { this._accountBox.visible = false; } catch (_e) {}
            try { this._avatarBin.set_child(null); } catch (_e) {}
            try { this._avatarBin.set_style(null); } catch (_e) {}
            try {
                this._displayNameLabel.text = '';
                this._displayNameLabel.visible = false;
            } catch (_e) {}
            try {
                this._identifierLabel.text = '';
                this._identifierLabel.visible = false;
            } catch (_e) {}
            return;
        }

        // Circular avatar: render the PNG as the BACKGROUND of the St.Bin
        // and let CSS's `border-radius: 999px` on `.nostr-login-qr-avatar`
        // clip it to a circle.  This is exactly the technique gnome-shell's
        // own userWidget.js uses to draw the AccountsService avatar: a
        // Clutter image actor with border-radius does NOT clip (the pixels
        // extend past the rounded box), but St's border-radius clips the
        // widget's BACKGROUND correctly.  Path safety is already enforced
        // upstream by `_extractAccount` — `account.avatar_path` only ever
        // points inside `MANIFEST_DIR` (basename-only, existence-checked).
        let avatarStyled = false;
        if (account.avatar_path) {
            try {
                const uri = Gio.File.new_for_path(account.avatar_path).get_uri();
                this._avatarBin.set_child(null);
                // background-size: cover — fill the 100 px circle regardless
                // of the source PNG's aspect ratio, matching gnome-shell's
                // own avatar rendering.  The neutral background-color from
                // the stylesheet is drawn beneath so a partially-transparent
                // PNG still reads as a filled circle rather than a hole.
                this._avatarBin.set_style(
                    `background-image: url("${uri}"); background-size: cover;`);
                avatarStyled = true;
            } catch (e) {
                _safeLog(`avatar background style failed for ${account.avatar_path}: ${e.message}`);
            }
        }
        if (!avatarStyled) {
            // Generic fallback so we still render *something* in the circle
            // rather than an empty plate when the producer omitted the PNG
            // or the URI construction failed.  Uses the shell's own symbolic
            // avatar so it themes with the surrounding dialog.  Clear any
            // prior inline background-image so a stale avatar can't bleed
            // through beneath the fallback icon.
            try { this._avatarBin.set_style(null); } catch (_e) {}
            let fallback = null;
            try {
                fallback = new St.Icon({
                    icon_name: 'avatar-default-symbolic',
                    icon_size: AVATAR_DISPLAY_PX,
                    style_class: 'nostr-login-qr-avatar-fallback',
                });
            } catch (_e) {
                fallback = null;
            }
            try { this._avatarBin.set_child(fallback); } catch (_e) {}
        }

        const label = account.display_name || '';
        try {
            this._displayNameLabel.text = label;
            this._displayNameLabel.visible = label.length > 0;
        } catch (_e) {}

        const ident = account.identifier || '';
        try {
            this._identifierLabel.text = ident;
            this._identifierLabel.visible = ident.length > 0;
        } catch (_e) {}

        try { this._accountBox.visible = true; } catch (_e) {}
    }

    _resolveAccountOverride(rawAccount) {
        // Wrap `_shouldOverrideNativeWidget` in a try/catch so a bug in
        // the detection path (unusual shell version, mid-transition dialog
        // state) can never silently drop the identity block — we default to
        // OVERRIDE on failure, which matches v3's prior behaviour and is
        // strictly safer than leaving the wrong name / generic icon on
        // screen with our QR beneath it.
        if (!rawAccount) return null;
        try {
            return this._shouldOverrideNativeWidget(rawAccount)
                ? rawAccount
                : null;
        } catch (e) {
            _safeLog(`native-widget detection failed, defaulting to override: ${e.message}`);
            return rawAccount;
        }
    }

    _shouldOverrideNativeWidget(account) {
        // Return true when we should hide GDM's own user widget and render
        // our resolved account identity above the QR; false when GDM's
        // widget is already correct (tile-click flow) and we should just
        // slot the QR + pairing code into the AuthPrompt beneath it.
        //
        // Primary signal: an `identifier` field means the login started via
        // NIP-05 at "Not listed?", so GDM's dialog is showing the raw typed
        // string (e.g. `chebizarro@coinos.io`) with a generic avatar until
        // pam_nostr canonicalises the name.  Always override in that case.
        if (account.identifier) return true;

        // Belt-and-braces: does the AuthPrompt already carry a `user-widget`
        // whose visible label names this account?  If we can't find a widget
        // to compare against we default to override so we don't regress the
        // v3 look for unusual shell versions.
        const authPrompt = this._findAuthPrompt();
        if (!authPrompt) return true;
        const userWidgets = [];
        _collectDescendantsByAnyClass(
            authPrompt,
            USER_WIDGET_CLASSES,
            userWidgets,
            this._container);
        if (userWidgets.length === 0) return true;

        // GDM's UserWidget label may hold either the canonical username
        // (`n_bizarro`) or the AccountsService real-name (`Biz`); accept
        // either as "already correct" so both tile-click variants keep the
        // native widget.  Empty strings on both sides never match.
        const wantNames = [];
        if (account.username)
            wantNames.push(account.username.toLowerCase().trim());
        if (account.display_name)
            wantNames.push(account.display_name.toLowerCase().trim());
        if (wantNames.length === 0) return true;

        for (const w of userWidgets) {
            const labelActor = _findDescendantByStyleClass(
                w, USER_WIDGET_LABEL_CLASS, this._container);
            let text = '';
            try {
                text = (labelActor && (labelActor.text
                    || (labelActor.clutter_text && labelActor.clutter_text.text))
                    || '').toString();
            } catch (_e) {}
            const norm = text.toLowerCase().trim();
            if (!norm) continue;
            if (wantNames.indexOf(norm) !== -1)
                return false;
        }
        return true;
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
