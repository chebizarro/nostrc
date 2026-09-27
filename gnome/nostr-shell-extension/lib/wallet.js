// SPDX-License-Identifier: MIT
//
// WalletMonitor — cached org.nostr.Wallet1 balance for the panel.
//
// Never prompts on its own. nostr-wallet-agent asks the user ("use your
// wallet?") for any read by an application it has not been told to "Always
// allow", and has no non-interactive mode. So before calling GetBalance
// automatically this monitor checks the agent's own grant store,
// $XDG_STATE_HOME/nostr-wallet/budgets.json, for `allow_read` on the identity
// the agent assigns to this process (state.js walletCallerId, a port of
// nwa-caller.c). Without that grant the balance is "—"; requestAccess() —
// only ever called from an explicit click — is the one path that may make
// the agent show its dialog.
//
// Residual risk (cannot be detected client-side): if the agent fails to
// identify gnome-shell even though budgets.json grants it (e.g. its PID
// recheck fails), an automatic read becomes a prompt. A Denied answer
// latches `_denied`, so that can happen at most once until the grant file
// changes.
//
// The proxy is created with DO_NOT_AUTO_START: showing the panel never
// launches the agent. Balance is fetched when the grant, the pairing or the
// agent's presence changes, on PaymentReceived/PaymentSent, and on
// refresh() when the cached value is older than the refresh interval.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {errorMessage, isCancelled, remoteErrorName} from './dbus.js';
import {classifyWalletError, readAllowed, walletCallerId} from './state.js';

const W_NAME = 'org.nostr.Wallet1';
const W_PATH = '/org/nostr/Wallet1';
const W_IFACE = 'org.nostr.Wallet1';

// The agent's own request timeout is 60 s (NWC round trip over relays).
const BALANCE_TIMEOUT = 70000;
// User-initiated: approval dialog (120 s) + request.
const GRANT_TIMEOUT = 200000;
const RELOAD_DEBOUNCE_MS = 500;

function loadText(file, cancellable) {
    return new Promise((resolve, reject) => {
        file.load_contents_async(cancellable, (f, res) => {
            try {
                const [, bytes] = f.load_contents_finish(res);
                resolve(new TextDecoder().decode(bytes));
            } catch (e) {
                reject(e);
            }
        });
    });
}

function symlinkTarget(file, cancellable) {
    return new Promise((resolve, reject) => {
        file.query_info_async('standard::symlink-target',
            Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, GLib.PRIORITY_DEFAULT, cancellable,
            (f, res) => {
                try {
                    resolve(f.query_info_finish(res).get_symlink_target());
                } catch (e) {
                    reject(e);
                }
            });
    });
}

export class WalletMonitor {
    /**
     * @param {(state: object) => void} onChanged
     * @param {object} [opts]
     * @param {Gio.DBusConnection} [opts.bus]
     * @param {string} [opts.budgetsPath]  override for tests
     */
    constructor(onChanged, {bus = null, budgetsPath = null} = {}) {
        this._onChanged = onChanged;
        this._bus = bus ?? Gio.DBus.session;
        this._budgetsFile = Gio.File.new_for_path(budgetsPath ??
            GLib.build_filenamev([GLib.get_user_state_dir(), 'nostr-wallet', 'budgets.json']));
        this._cancellable = new Gio.Cancellable();
        this._proxy = null;
        this._proxyIds = [];
        this._monitor = null;
        this._monitorId = 0;
        this._reloadId = 0;
        this._callerId = null;
        this._readAllowed = false;
        this._denied = false;
        this._showBalance = false;
        this._balanceMsat = null;
        this._fetchedAt = 0;
        this._fetching = false;
        this._fetchAgain = false;
        this._granting = false;
        this._error = null;
        this._destroyed = false;
    }

    get state() {
        const owner = this._proxy?.get_name_owner() ?? null;
        const paired = owner ? this._proxy.get_cached_property('Paired')?.unpack() ?? null : null;
        return {
            showBalance: this._showBalance,
            agentRunning: !!owner,
            paired,
            readAllowed: this._readAllowed && !this._denied,
            balanceMsat: this._balanceMsat,
            error: this._error,
            callerId: this._callerId,
            granting: this._granting,
        };
    }

    start({showBalance}) {
        this._showBalance = !!showBalance;

        Gio.DBusProxy.new(this._bus, Gio.DBusProxyFlags.DO_NOT_AUTO_START, null,
            W_NAME, W_PATH, W_IFACE, this._cancellable, (src, res) => {
                let proxy;
                try {
                    proxy = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    if (!isCancelled(e) && !this._destroyed) {
                        this._error = errorMessage(e);
                        this._emit();
                    }
                    return;
                }
                if (this._destroyed)
                    return;
                this._proxy = proxy;
                this._proxyIds = [
                    proxy.connect('g-properties-changed', (p, changed) => {
                        if (changed.lookup_value('Paired', null)) {
                            this._balanceMsat = null;
                            this._maybeFetch();
                        }
                        this._emit();
                    }),
                    proxy.connect('g-signal', (p, sender, signal) => {
                        if (signal === 'PaymentReceived' || signal === 'PaymentSent') {
                            if (!this.state.readAllowed)
                                this._balanceMsat = null; // cannot refresh: never shown stale
                            this._maybeFetch();
                            this._emit();
                        }
                    }),
                    proxy.connect('notify::g-name-owner', () => {
                        this._balanceMsat = null;
                        this._error = null;
                        this._maybeFetch();
                        this._emit();
                    }),
                ];
                this._maybeFetch();
                this._emit();
            });

        try {
            this._monitor = this._budgetsFile.monitor_file(Gio.FileMonitorFlags.NONE, null);
            this._monitorId = this._monitor.connect('changed', () => this._scheduleReload());
        } catch {
            this._monitor = null; // no monitoring; grant changes seen on refresh()
        }

        this._resolveIdentity();
    }

    setShowBalance(show) {
        this._showBalance = !!show;
        if (!this._showBalance)
            this._balanceMsat = null;
        this._maybeFetch();
        this._emit();
    }

    /** One-shot refresh (menu opened): re-read the grant, refetch if stale. */
    refresh(maxAgeSeconds) {
        this._reloadGrant();
        const ageUs = GLib.get_monotonic_time() - this._fetchedAt;
        if (this._balanceMsat === null || ageUs > maxAgeSeconds * GLib.USEC_PER_SEC)
            this._maybeFetch();
    }

    /**
     * Explicit user action only ("Allow balance access…"): one GetBalance
     * that nostr-wallet-agent may answer with its approval dialog, where the
     * user can tick "Always allow this app".
     */
    requestAccess() {
        if (this._destroyed || !this._proxy || this._granting)
            return;
        this._granting = true;
        this._emit();
        this._proxy.call('GetBalance', null, Gio.DBusCallFlags.NO_AUTO_START, GRANT_TIMEOUT,
            this._cancellable, (p, res) => {
                if (this._destroyed)
                    return;
                this._granting = false;
                try {
                    const [msat] = p.call_finish(res).deepUnpack();
                    this._balanceMsat = msat;
                    this._fetchedAt = GLib.get_monotonic_time();
                    this._denied = false;
                    this._error = null;
                } catch (e) {
                    if (isCancelled(e))
                        return;
                    if (classifyWalletError(remoteErrorName(e)) === 'denied')
                        this._denied = true; // sticky until budgets.json changes
                    this._error = errorMessage(e);
                }
                this._reloadGrant();
                this._emit();
            });
    }

    destroy() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        this._cancellable.cancel();
        if (this._proxy) {
            for (const id of this._proxyIds)
                this._proxy.disconnect(id);
        }
        this._proxyIds = [];
        this._proxy = null;
        if (this._monitor) {
            this._monitor.disconnect(this._monitorId);
            this._monitor.cancel();
            this._monitor = null;
            this._monitorId = 0;
        }
        if (this._reloadId) {
            GLib.Source.remove(this._reloadId);
            this._reloadId = 0;
        }
        this._onChanged = null;
    }

    // ── internals ──────────────────────────────────────────────────────

    _emit() {
        if (!this._destroyed)
            this._onChanged?.(this.state);
    }

    async _resolveIdentity() {
        let cgroup = null, exe = null;
        try {
            cgroup = await loadText(Gio.File.new_for_path('/proc/self/cgroup'), this._cancellable);
        } catch (e) {
            if (isCancelled(e))
                return;
        }
        try {
            exe = await symlinkTarget(Gio.File.new_for_path('/proc/self/exe'), this._cancellable);
        } catch (e) {
            if (isCancelled(e))
                return;
        }
        if (this._destroyed)
            return;
        this._callerId = walletCallerId({cgroup, exe});
        this._emit();
        this._reloadGrant();
    }

    _scheduleReload() {
        if (this._destroyed || this._reloadId)
            return;
        this._reloadId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, RELOAD_DEBOUNCE_MS, () => {
            this._reloadId = 0;
            this._reloadGrant();
            return GLib.SOURCE_REMOVE;
        });
    }

    async _reloadGrant() {
        if (this._destroyed || !this._callerId)
            return;
        let text = null;
        try {
            text = await loadText(this._budgetsFile, this._cancellable);
        } catch (e) {
            if (isCancelled(e))
                return;
        }
        if (this._destroyed)
            return;
        const allowed = readAllowed(text, this._callerId);
        const changed = allowed !== this._readAllowed;
        this._readAllowed = allowed;
        if (changed) {
            this._denied = false;
            if (!allowed)
                this._balanceMsat = null;
            this._maybeFetch();
            this._emit();
        }
    }

    _maybeFetch() {
        const s = this.state;
        if (this._destroyed || !this._proxy || this._granting || !s.showBalance ||
            !s.agentRunning || s.paired !== true || !s.readAllowed)
            return;
        if (this._fetching) {
            this._fetchAgain = true;
            return;
        }
        this._fetching = true;
        this._proxy.call('GetBalance', null, Gio.DBusCallFlags.NO_AUTO_START, BALANCE_TIMEOUT,
            this._cancellable, (p, res) => {
                if (this._destroyed)
                    return;
                this._fetching = false;
                try {
                    const [msat] = p.call_finish(res).deepUnpack();
                    this._balanceMsat = msat;
                    this._fetchedAt = GLib.get_monotonic_time();
                    this._error = null;
                } catch (e) {
                    if (isCancelled(e))
                        return;
                    this._balanceMsat = null;
                    const kind = classifyWalletError(remoteErrorName(e));
                    if (kind === 'denied')
                        this._denied = true; // grant store and agent disagree: stop asking
                    else if (kind !== 'unpaired' && kind !== 'not-running')
                        this._error = errorMessage(e);
                }
                this._emit();
                if (this._fetchAgain) {
                    this._fetchAgain = false;
                    this._maybeFetch();
                }
            });
    }
}
