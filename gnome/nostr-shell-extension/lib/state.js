// SPDX-License-Identifier: MIT
//
// Pure state and formatting logic for the Nostr Shell extension (bead
// nostrc-33f1). No `gi://` imports: this module runs unchanged under
// gnome-shell, plain gjs and Node (tests/run-tests.js).
//
// The relay model mirrors gnome/nostr-settings/src/core/nss-systemd.c so the
// Quick Settings toggle and the Settings "Relays" page always agree:
//   * the toggle's checked state is the persistent intent — the socket unit's
//     UnitFileState (enabled / enabled-runtime / linked / linked-runtime);
//   * the subtitle is runtime truth (Off, Listening, Running, Failed, …);
//   * On  = EnableUnitFiles([socket]) → Reload → StartUnit(socket)
//     Off = DisableUnitFiles([socket]) → Reload → StopUnit(service) → StopUnit(socket)

let _ = s => s;
const plainNgettext = (s, p, n) => (n === 1 ? s : p);
let ngettext = plainNgettext;

/** Install the extension's gettext / ngettext (called from extension.js). */
export function setTranslator(fn, nfn = null) {
    _ = typeof fn === 'function' ? fn : s => s;
    ngettext = typeof nfn === 'function' ? nfn : plainNgettext;
}

export const RELAY_SOCKET = 'nostr-session-relay.socket';
export const RELAY_SERVICE = 'nostr-session-relay.service';
export const NOTIFY_SERVICE = 'nostr-notify.service';

export const MIN_REFRESH_SECONDS = 30;
export const MAX_REFRESH_SECONDS = 3600;

/** Clamp a refresh interval: never poll faster than every 30 s. */
export function clampRefresh(seconds) {
    const n = Number.isFinite(seconds) ? Math.floor(seconds) : MIN_REFRESH_SECONDS;
    return Math.min(MAX_REFRESH_SECONDS, Math.max(MIN_REFRESH_SECONDS, n));
}

// ── systemd status model (nss_service_status) ─────────────────────────────

export const Status = Object.freeze({
    UNKNOWN: 'unknown',             // systemd --user not reachable
    NOT_INSTALLED: 'not-installed',
    MASKED: 'masked',
    OFF: 'off',
    STOPPED: 'stopped',             // enabled but not listening
    LISTENING: 'listening',
    STARTING: 'starting',
    RUNNING: 'running',
    FAILED: 'failed',
});

/** UnitFileState values that mean "enabled" (nss_unit_file_enabled). */
export function unitFileEnabled(unit) {
    const u = unit?.unitFileState;
    return u === 'enabled' || u === 'enabled-runtime' ||
        u === 'linked' || u === 'linked-runtime';
}

function masked(unit) {
    return !!unit && (unit.loadState === 'masked' ||
        unit.unitFileState === 'masked' || unit.unitFileState === 'masked-runtime');
}

/**
 * Runtime status from the socket + service unit states.
 * @param {?{loadState,activeState,subState,unitFileState}} socket
 * @param {?{loadState,activeState,subState,unitFileState}} service
 */
export function serviceStatus(socket, service) {
    const primary = socket ?? service;
    if (!primary || !primary.loadState || primary.loadState === 'not-found')
        return Status.NOT_INSTALLED;
    if (masked(socket) || masked(service))
        return Status.MASKED;
    if (service && (service.activeState === 'failed' || service.subState === 'auto-restart'))
        return Status.FAILED;
    if (service && (service.activeState === 'activating' || service.activeState === 'reloading'))
        return Status.STARTING;
    if (service?.activeState === 'active')
        return Status.RUNNING;
    if (socket?.activeState === 'failed')
        return Status.FAILED;
    if (socket?.activeState === 'active')
        return Status.LISTENING;
    return unitFileEnabled(primary) ? Status.STOPPED : Status.OFF;
}

// ── On/off plans (nss_relay_plan) ─────────────────────────────────────────

/**
 * The exact org.freedesktop.systemd1.Manager call sequence for switching the
 * session relay on or off. Disabling first means a client connecting in
 * between cannot socket-activate the relay after it was switched off.
 * @returns {Array<{method: string, unit: ?string}>}
 */
export function relayPlan(on) {
    return on
        ? [
            {method: 'EnableUnitFiles', unit: RELAY_SOCKET},
            {method: 'Reload', unit: null},
            {method: 'StartUnit', unit: RELAY_SOCKET},
        ]
        : [
            {method: 'DisableUnitFiles', unit: RELAY_SOCKET},
            {method: 'Reload', unit: null},
            {method: 'StopUnit', unit: RELAY_SERVICE},
            {method: 'StopUnit', unit: RELAY_SOCKET},
        ];
}

// ── Formatting ────────────────────────────────────────────────────────────

/** Integer with locale digit grouping ("12,345"). */
export function formatInteger(n, locale) {
    const v = Math.trunc(Number(n) || 0);
    try {
        return new Intl.NumberFormat(locale, {maximumFractionDigits: 0}).format(v);
    } catch {
        return String(v);
    }
}

/**
 * Byte size in decimal units like g_format_size() ("1.2 MB"); `bytes` may be
 * a Number or a BigInt.
 */
export function formatBytes(bytes) {
    const b = Number(bytes);
    if (!Number.isFinite(b) || b < 0)
        return _('unknown size');
    if (b < 1000)
        return b === 1 ? _('1 byte') : _('%d bytes').replace('%d', String(Math.trunc(b)));
    const units = ['kB', 'MB', 'GB', 'TB', 'PB'];
    let v = b / 1000;
    let i = 0;
    while (v >= 1000 && i < units.length - 1) {
        v /= 1000;
        i++;
    }
    // One decimal like GLib; 999.95 kB rounds up into the next unit.
    let r = Math.round(v * 10) / 10;
    if (r >= 1000 && i < units.length - 1) {
        r = Math.round(r / 100) / 10;
        i++;
    }
    return `${r.toFixed(1)} ${units[i]}`;
}

/** Uptime in the two largest units ("3 h 12 min", "45 s"). */
export function formatDuration(seconds) {
    let s = Math.max(0, Math.floor(Number(seconds) || 0));
    const d = Math.floor(s / 86400);
    s -= d * 86400;
    const h = Math.floor(s / 3600);
    s -= h * 3600;
    const m = Math.floor(s / 60);
    s -= m * 60;
    if (d > 0)
        return h > 0 ? `${d} d ${h} h` : `${d} d`;
    if (h > 0)
        return m > 0 ? `${h} h ${m} min` : `${h} h`;
    if (m > 0)
        return `${m} min`;
    return `${s} s`;
}

/**
 * Wallet balance from msat. Whole sats are shown; a sub-sat remainder is
 * dropped, never rounded up (a balance is never overstated).
 */
export function formatSats(msat, locale) {
    let m;
    try {
        m = BigInt(msat);
    } catch {
        return '—';
    }
    if (m < 0n)
        return '—';
    const sats = m / 1000n;
    if (sats === 1n)
        return _('1 sat');
    if (sats > BigInt(Number.MAX_SAFE_INTEGER))
        return _('%s sats').replace('%s', sats.toString());
    return _('%s sats').replace('%s', formatInteger(Number(sats), locale));
}

// ── Relay toggle state ────────────────────────────────────────────────────

/**
 * Normalise an org.nostr.SessionRelay1.GetStats a{sv} (unpacked to plain JS)
 * into the fields the panel shows. Missing or mistyped values → null.
 */
export function normalizeStats(raw) {
    if (!raw || typeof raw !== 'object')
        return null;
    const num = v => (typeof v === 'number' || typeof v === 'bigint') ? Number(v) : null;
    return {
        eventCount: num(raw.event_count),
        storageBytes: num(raw.storage_bytes),
        connectedClients: num(raw.connected_clients),
        uptime: num(raw.uptime),
        storageBackend: typeof raw.storage_backend === 'string' ? raw.storage_backend : null,
    };
}

/**
 * "1,234 events · 12.3 MB" — the honest storage line of nostr-settings: a
 * cache-less relay says "No storage", an unknown count (-1) is never shown as
 * a number.
 */
export function statsSummary(stats, locale) {
    if (!stats)
        return null;
    if (stats.storageBackend === 'none')
        return _('No storage');
    const size = stats.storageBytes !== null ? formatBytes(stats.storageBytes) : null;
    if (stats.eventCount === null || stats.eventCount < 0)
        return size ? _('%s on disk').replace('%s', size) : null;
    const events = stats.eventCount === 1
        ? _('1 event')
        : _('%s events').replace('%s', formatInteger(stats.eventCount, locale));
    return size ? `${events} · ${size}` : events;
}

/** Second menu line: clients and uptime. */
export function statsDetail(stats, locale) {
    if (!stats)
        return null;
    const parts = [];
    if (stats.connectedClients !== null) {
        parts.push(stats.connectedClients === 1
            ? _('1 client')
            : _('%s clients').replace('%s', formatInteger(stats.connectedClients, locale)));
    }
    if (stats.uptime !== null)
        parts.push(_('up %s').replace('%s', formatDuration(stats.uptime)));
    return parts.length ? parts.join(' · ') : null;
}

/**
 * Everything the Quick Settings toggle shows for the session relay.
 *
 * @param {object} s
 * @param {boolean} s.available   systemd --user answered
 * @param {?object} s.socket      state of nostr-session-relay.socket
 * @param {?object} s.service     state of nostr-session-relay.service
 * @param {?object} s.stats       normalizeStats() result; null while the relay
 *                                does not own org.nostr.SessionRelay1
 * @param {?string} s.busy        'on' | 'off' while a plan runs
 * @param {?string} s.error       last plan failure
 */
export function relayView(s, locale) {
    const status = s.available ? serviceStatus(s.socket, s.service) : Status.UNKNOWN;
    const checked = s.available ? unitFileEnabled(s.socket) : false;
    const reactive = !s.busy &&
        status !== Status.UNKNOWN && status !== Status.NOT_INSTALLED && status !== Status.MASKED;

    const subtitles = {
        [Status.UNKNOWN]: _('Status unavailable'),
        [Status.NOT_INSTALLED]: _('Not installed'),
        [Status.MASKED]: _('Disabled by administrator'),
        [Status.OFF]: _('Off'),
        [Status.STOPPED]: _('Enabled, not listening'),
        [Status.LISTENING]: _('Listening'),
        [Status.STARTING]: _('Starting…'),
        [Status.RUNNING]: _('Running'),
        [Status.FAILED]: _('Failed'),
    };
    const statusLines = {
        [Status.UNKNOWN]: _('systemd user manager not reachable'),
        [Status.NOT_INSTALLED]: _('nostr-session-relay.socket is not installed'),
        [Status.MASKED]: _('The relay units are masked'),
        [Status.OFF]: _('Off'),
        [Status.STOPPED]: _('Enabled but not listening'),
        [Status.LISTENING]: _('Listening — starts when an app connects'),
        [Status.STARTING]: _('Starting…'),
        [Status.RUNNING]: _('Running'),
        [Status.FAILED]: _('Failed — see journalctl --user'),
    };

    let subtitle = subtitles[status];
    let detailLine = null;
    if (status === Status.RUNNING && s.stats) {
        subtitle = statsSummary(s.stats, locale) ?? subtitle;
        detailLine = statsDetail(s.stats, locale);
    }
    if (s.busy === 'on')
        subtitle = _('Turning on…');
    else if (s.busy === 'off')
        subtitle = _('Turning off…');
    else if (s.error)
        subtitle = _('Error');

    return {
        status,
        checked,
        reactive,
        subtitle,
        statusLine: !s.busy && s.error
            ? _('Could not switch the relay: %s').replace('%s', s.error)
            : statusLines[status],
        detailLine,
        showIndicator: status === Status.RUNNING || status === Status.LISTENING ||
            status === Status.STARTING,
    };
}

// ── Direct messages (nostr-notify) ────────────────────────────────────────

/**
 * The direct-message row: the unread count from org.nostr.NotifyDaemon1 when
 * the notifier is running and on the bus (`unread`, from lib/dm.js), else
 * whether DM notifications are delivered at all. Returns null to hide the
 * row. Counts only — never content or senders.
 */
export function dmView({available, notify}, unread = null) {
    if (!available || !notify || !notify.loadState || notify.loadState === 'not-found')
        return null;
    if (masked(notify))
        return _('Message notifications: disabled by administrator');
    switch (notify.activeState) {
    case 'active':
    case 'reloading':
        if (Number.isInteger(unread) && unread > 0)
            return ngettext('%d unread direct message', '%d unread direct messages', unread)
                .replace('%d', String(unread));
        if (unread === 0)
            return _('No unread direct messages');
        return _('Message notifications: on');
    case 'activating':
        return _('Message notifications: starting…');
    case 'failed':
        return _('Message notifications: failed');
    default:
        return _('Message notifications: off');
    }
}

// ── Wallet ────────────────────────────────────────────────────────────────

export const WalletState = Object.freeze({
    HIDDEN: 'hidden',          // show-balance is off
    NOT_RUNNING: 'not-running',
    UNPAIRED: 'unpaired',
    NEEDS_GRANT: 'needs-grant',
    LOADING: 'loading',
    BALANCE: 'balance',
    ERROR: 'error',
});

/**
 * The wallet row. The panel never causes an approval prompt by itself:
 * without a read grant the balance is "—" and the grant action is offered;
 * only the user clicking it lets the agent ask.
 *
 * @param {object} w
 * @param {boolean} w.showBalance
 * @param {boolean} w.agentRunning   org.nostr.Wallet1 has an owner
 * @param {?boolean} w.paired
 * @param {boolean} w.readAllowed
 * @param {?(number|bigint)} w.balanceMsat  cached balance, null if none
 * @param {boolean} [w.granting]      a user-initiated request is pending
 * @param {?string} w.error
 */
export function walletView(w, locale) {
    if (!w.showBalance)
        return {state: WalletState.HIDDEN, label: null, canGrant: false};
    if (!w.agentRunning)
        return {state: WalletState.NOT_RUNNING, label: _('Wallet: agent not running'), canGrant: false};
    if (w.paired !== true)
        return {state: WalletState.UNPAIRED, label: _('Wallet: not paired'), canGrant: false};
    // A known balance is shown even without a standing grant: it came from a
    // request the user approved once. It is dropped (never shown stale) on
    // the next payment signal, since it cannot be refreshed without asking.
    if (w.balanceMsat !== null && w.balanceMsat !== undefined) {
        return {
            state: WalletState.BALANCE,
            label: _('Wallet: %s').replace('%s', formatSats(w.balanceMsat, locale)),
            canGrant: false,
        };
    }
    if (!w.readAllowed) {
        return {
            state: WalletState.NEEDS_GRANT,
            label: w.granting ? _('Wallet: waiting for approval…') : _('Wallet: —'),
            canGrant: !w.granting,
        };
    }
    if (w.error)
        return {state: WalletState.ERROR, label: _('Wallet: —'), canGrant: false};
    return {state: WalletState.LOADING, label: _('Wallet: …'), canGrant: false};
}

/** Map a D-Bus error name from GetBalance to how the panel reacts. */
export function classifyWalletError(dbusErrorName) {
    switch (dbusErrorName) {
    case 'org.nostr.Wallet1.Error.NotPaired':
        return 'unpaired';
    // The agent would have to ask (no read grant) — or it predates the
    // non-interactive methods: either way, never fall back to prompting.
    case 'org.nostr.Wallet1.Error.InteractionRequired':
    case 'org.freedesktop.DBus.Error.UnknownMethod':
        return 'needs-grant';
    case 'org.nostr.Wallet1.Error.Denied':
    case 'org.nostr.Wallet1.Error.RateLimited':
        return 'denied';
    case 'org.freedesktop.DBus.Error.ServiceUnknown':
    case 'org.freedesktop.DBus.Error.NameHasNoOwner':
        return 'not-running';
    default:
        return 'error';
    }
}
