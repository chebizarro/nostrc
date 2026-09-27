// SPDX-License-Identifier: MIT
//
// RelayMonitor — session relay state for the Quick Settings toggle.
//
// * Unit state (nostr-session-relay.{socket,service}, nostr-notify.service)
//   comes from systemd --user over org.freedesktop.systemd1. It is re-read
//   every refresh interval (never faster than 30 s), when the caller asks
//   (Quick Settings opened) and after every on/off plan. PropertiesChanged on
//   the unit objects and the Manager's UnitFilesChanged/Reloading are also
//   matched and trigger a debounced re-read — passively: systemd broadcasts
//   them only while some client has called Manager.Subscribe(), and this
//   monitor deliberately never calls Subscribe/Unsubscribe itself. gnome-shell
//   and every extension share one bus connection and systemd tracks the
//   subscription per connection, so an Unsubscribe here could silently cancel
//   somebody else's. Because StartUnit/StopUnit return once the job is
//   *queued*, a re-read that still finds a job pending (Unit.Job) or a unit
//   in a transitional state re-reads every 500 ms until it settles (at most
//   20 s) — a bounded settle after a change, not a poll.
// * Statistics come from org.nostr.SessionRelay1 only while that name has an
//   owner. Every call uses NO_AUTO_START and the relay ships no D-Bus
//   activation file, so watching the panel never starts the relay.
// * setEnabled() runs the exact plan nostr-settings runs (state.js relayPlan).
//
// Only Gio/GLib: runs inside gnome-shell and under plain gjs (tests/lab).

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {dbusCall, errorMessage, isCancelled} from './dbus.js';
import {
    NOTIFY_SERVICE, RELAY_SERVICE, RELAY_SOCKET, clampRefresh, normalizeStats, relayPlan,
} from './state.js';

const SD_NAME = 'org.freedesktop.systemd1';
const SD_PATH = '/org/freedesktop/systemd1';
const SD_MGR = 'org.freedesktop.systemd1.Manager';
const SD_UNIT = 'org.freedesktop.systemd1.Unit';
const PROPS = 'org.freedesktop.DBus.Properties';
// systemd --user always owns its name in a real session; never ask the bus
// to spawn something for it (e.g. a headless shell on a private bus).
const SD_FLAGS = Gio.DBusCallFlags.NO_AUTO_START;

const RELAY_NAME = 'org.nostr.SessionRelay1';
const RELAY_PATH = '/org/nostr/SessionRelay1';

const CALL_TIMEOUT = 5000;
// Start/stop return once the job is queued; a Type=notify relay can still
// take a moment (same value as nss-systemd.c).
const JOB_TIMEOUT = 25000;
const DEBOUNCE_MS = 250;
const SETTLE_MS = 500;
const SETTLE_MAX = 40;

const UNITS = Object.freeze({socket: RELAY_SOCKET, service: RELAY_SERVICE, notify: NOTIFY_SERVICE});

export class RelayMonitor {
    /**
     * @param {(state: object) => void} onChanged
     * @param {Gio.DBusConnection} [bus]  defaults to the session bus
     */
    constructor(onChanged, bus = null) {
        this._onChanged = onChanged;
        this._bus = bus ?? Gio.DBus.session;
        this._cancellable = new Gio.Cancellable();
        this._paths = {};
        this._units = {socket: null, service: null, notify: null};
        this._available = false;
        this._stats = null;
        this._relayOwned = false;
        this._busy = null;
        this._error = null;
        this._signalIds = [];
        this._watchId = 0;
        this._timerId = 0;
        this._debounceId = 0;
        this._settleId = 0;
        this._settleCount = 0;
        this._interval = clampRefresh(60);
        this._resyncing = false;
        this._resyncAgain = false;
        this._statsInFlight = false;
        this._destroyed = false;
    }

    get state() {
        return {
            available: this._available,
            socket: this._units.socket,
            service: this._units.service,
            notify: this._units.notify,
            stats: this._relayOwned ? this._stats : null,
            busy: this._busy,
            error: this._error,
        };
    }

    start(intervalSeconds) {
        this._interval = clampRefresh(intervalSeconds);

        this._signalIds.push(this._bus.signal_subscribe(SD_NAME, SD_MGR, 'UnitFilesChanged',
            SD_PATH, null, Gio.DBusSignalFlags.NONE, () => this._scheduleResync()));
        this._signalIds.push(this._bus.signal_subscribe(SD_NAME, SD_MGR, 'Reloading',
            SD_PATH, null, Gio.DBusSignalFlags.NONE, () => this._scheduleResync()));

        this._watchId = Gio.bus_watch_name_on_connection(this._bus, RELAY_NAME,
            Gio.BusNameWatcherFlags.NONE,
            () => {
                this._relayOwned = true;
                this._fetchStats();
            },
            () => {
                this._relayOwned = false;
                this._stats = null;
                this._emit();
            });

        this._startTimer();
        this._resync();
    }

    setRefreshInterval(seconds) {
        const s = clampRefresh(seconds);
        if (s === this._interval)
            return;
        this._interval = s;
        if (this._timerId) {
            GLib.Source.remove(this._timerId);
            this._timerId = 0;
        }
        this._startTimer();
    }

    /** One-shot refresh, e.g. when Quick Settings opens. Not a poll. */
    refresh() {
        this._resync();
        this._fetchStats();
    }

    /** Switch the relay on (enable + start socket) or off (disable, stop both). */
    async setEnabled(on) {
        if (this._busy || this._destroyed)
            return;
        this._busy = on ? 'on' : 'off';
        this._error = null;
        this._emit();
        try {
            for (const op of relayPlan(on))
                // eslint-disable-next-line no-await-in-loop
                await this._runOp(op);
        } catch (e) {
            if (isCancelled(e) || this._destroyed)
                return;
            this._error = errorMessage(e);
        }
        this._busy = null;
        this._settleCount = 0;
        this._emit();
        this._resync();
    }

    destroy() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        this._cancellable.cancel();
        for (const id of this._signalIds)
            this._bus.signal_unsubscribe(id);
        this._signalIds = [];
        if (this._watchId) {
            Gio.bus_unwatch_name(this._watchId);
            this._watchId = 0;
        }
        if (this._timerId) {
            GLib.Source.remove(this._timerId);
            this._timerId = 0;
        }
        if (this._debounceId) {
            GLib.Source.remove(this._debounceId);
            this._debounceId = 0;
        }
        if (this._settleId) {
            GLib.Source.remove(this._settleId);
            this._settleId = 0;
        }
        this._onChanged = null;
    }

    // ── internals ──────────────────────────────────────────────────────

    _emit() {
        if (!this._destroyed)
            this._onChanged?.(this.state);
    }

    _startTimer() {
        this._timerId = GLib.timeout_add_seconds(GLib.PRIORITY_LOW, this._interval, () => {
            this._resync();
            this._fetchStats();
            return GLib.SOURCE_CONTINUE;
        });
    }

    _scheduleResync() {
        if (this._destroyed || this._debounceId)
            return;
        this._debounceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, DEBOUNCE_MS, () => {
            this._debounceId = 0;
            this._resync();
            return GLib.SOURCE_REMOVE;
        });
    }

    async _unitPath(key) {
        if (this._paths[key])
            return this._paths[key];
        // LoadUnit (not GetUnit): works for units not loaded right now and
        // reports LoadState "not-found" for units that do not exist.
        const r = await dbusCall(this._bus, {
            name: SD_NAME, path: SD_PATH, iface: SD_MGR, method: 'LoadUnit',
            params: new GLib.Variant('(s)', [UNITS[key]]), reply: '(o)', flags: SD_FLAGS,
            timeout: CALL_TIMEOUT, cancellable: this._cancellable,
        });
        const [path] = r.deepUnpack();
        if (this._destroyed)
            return path;
        this._paths[key] = path;
        this._signalIds.push(this._bus.signal_subscribe(SD_NAME, PROPS, 'PropertiesChanged',
            path, SD_UNIT, Gio.DBusSignalFlags.NONE, () => this._scheduleResync()));
        return path;
    }

    async _readUnit(key) {
        const path = await this._unitPath(key);
        const r = await dbusCall(this._bus, {
            name: SD_NAME, path, iface: PROPS, method: 'GetAll',
            params: new GLib.Variant('(s)', [SD_UNIT]), reply: '(a{sv})', flags: SD_FLAGS,
            timeout: CALL_TIMEOUT, cancellable: this._cancellable,
        });
        const [props] = r.deepUnpack();
        const val = k => (props[k] ? props[k].deepUnpack() : null);
        const job = val('Job'); // (uo): [id, path], id 0 = no job
        return {
            loadState: val('LoadState'),
            activeState: val('ActiveState'),
            subState: val('SubState'),
            unitFileState: val('UnitFileState'),
            jobPending: Array.isArray(job) && job[0] !== 0,
        };
    }

    async _resync() {
        if (this._destroyed)
            return;
        if (this._resyncing) {
            this._resyncAgain = true;
            return;
        }
        this._resyncing = true;
        try {
            const keys = Object.keys(UNITS);
            const states = await Promise.all(keys.map(k => this._readUnit(k)));
            if (this._destroyed)
                return;
            keys.forEach((k, i) => {
                this._units[k] = states[i];
            });
            this._available = true;
        } catch (e) {
            if (isCancelled(e) || this._destroyed)
                return;
            this._available = false;
            this._units = {socket: null, service: null, notify: null};
        } finally {
            this._resyncing = false;
        }
        this._emit();
        this._maybeSettle();
        if (this._resyncAgain) {
            this._resyncAgain = false;
            this._resync();
        }
    }

    _maybeSettle() {
        const transitional = Object.values(this._units).some(u => u &&
            (u.jobPending || u.activeState === 'activating' || u.activeState === 'deactivating'));
        if (!transitional) {
            this._settleCount = 0;
            return;
        }
        if (this._destroyed || this._settleId || this._settleCount >= SETTLE_MAX)
            return;
        this._settleCount++;
        this._settleId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, SETTLE_MS, () => {
            this._settleId = 0;
            this._resync();
            return GLib.SOURCE_REMOVE;
        });
    }

    async _fetchStats() {
        if (this._destroyed || !this._relayOwned || this._statsInFlight)
            return;
        this._statsInFlight = true;
        try {
            const r = await dbusCall(this._bus, {
                name: RELAY_NAME, path: RELAY_PATH, iface: RELAY_NAME, method: 'GetStats',
                reply: '(a{sv})', flags: Gio.DBusCallFlags.NO_AUTO_START,
                timeout: CALL_TIMEOUT, cancellable: this._cancellable,
            });
            if (this._destroyed)
                return;
            this._stats = normalizeStats(r.recursiveUnpack()[0]);
        } catch (e) {
            if (isCancelled(e) || this._destroyed)
                return;
            this._stats = null;
        } finally {
            this._statsInFlight = false;
        }
        this._emit();
    }

    _runOp(op) {
        const base = {
            name: SD_NAME, path: SD_PATH, iface: SD_MGR, method: op.method, flags: SD_FLAGS,
            timeout: CALL_TIMEOUT, cancellable: this._cancellable,
        };
        switch (op.method) {
        case 'EnableUnitFiles':
            return dbusCall(this._bus, {
                ...base, params: new GLib.Variant('(asbb)', [[op.unit], false, false]),
                reply: '(ba(sss))',
            });
        case 'DisableUnitFiles':
            return dbusCall(this._bus, {
                ...base, params: new GLib.Variant('(asb)', [[op.unit], false]),
                reply: '(a(sss))',
            });
        case 'Reload':
            return dbusCall(this._bus, {...base, reply: '()', timeout: JOB_TIMEOUT});
        case 'StartUnit':
        case 'StopUnit':
            return dbusCall(this._bus, {
                ...base, params: new GLib.Variant('(ss)', [op.unit, 'replace']),
                reply: '(o)', timeout: JOB_TIMEOUT,
            });
        default:
            return Promise.reject(new Error(`unknown systemd operation ${op.method}`));
        }
    }
}
