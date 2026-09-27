// SPDX-License-Identifier: MIT
//
// WalletMonitor — cached org.nostr.Wallet1 balance for the panel.
//
// Never prompts on its own. Automatic reads use GetBalanceNonInteractive:
// the agent answers when gnome-shell holds a read grant, and otherwise
// fails with org.nostr.Wallet1.Error.InteractionRequired without showing
// anything (nostrc-prqu.19). So the panel needs no copy of the agent's
// caller identification or its grant store. Without a grant the balance
// is "—" with an "Allow balance access…" item; requestAccess() — only ever
// called from that explicit click — is the one path that may make the
// agent show its dialog. The grant can also be given in Nostr Settings ›
// Wallet (its GNOME Shell row); the agent then emits AppsChanged and the
// monitor asks again.
//
// The proxy is created with DO_NOT_AUTO_START: showing the panel never
// launches the agent. Balance is fetched when the agent appears, the
// pairing or a grant changes, on PaymentReceived/PaymentSent, and on
// refresh() when the cached value is older than the refresh interval.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {errorMessage, isCancelled, remoteErrorName} from './dbus.js';
import {classifyWalletError} from './state.js';

const W_NAME = 'org.nostr.Wallet1';
const W_PATH = '/org/nostr/Wallet1';
const W_IFACE = 'org.nostr.Wallet1';

// The agent's own request timeout is 60 s (NWC round trip over relays).
const BALANCE_TIMEOUT = 70000;
// User-initiated: approval dialog (120 s) + request.
const GRANT_TIMEOUT = 200000;

export class WalletMonitor {
    /**
     * @param {(state: object) => void} onChanged
     * @param {object} [opts]
     * @param {Gio.DBusConnection} [opts.bus]
     */
    constructor(onChanged, {bus = null} = {}) {
        this._onChanged = onChanged;
        this._bus = bus ?? Gio.DBus.session;
        this._cancellable = new Gio.Cancellable();
        this._proxy = null;
        this._proxyIds = [];
        this._readAllowed = false;
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
            readAllowed: this._readAllowed,
            balanceMsat: this._balanceMsat,
            error: this._error,
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
                        if (signal === 'PaymentReceived' || signal === 'PaymentSent' ||
                            signal === 'AppsChanged') {
                            if (!this._readAllowed && signal !== 'AppsChanged')
                                this._balanceMsat = null; // cannot refresh: never shown stale
                            this._maybeFetch();
                            this._emit();
                        }
                    }),
                    proxy.connect('notify::g-name-owner', () => {
                        this._balanceMsat = null;
                        this._readAllowed = false;
                        this._error = null;
                        this._maybeFetch();
                        this._emit();
                    }),
                ];
                this._maybeFetch();
                this._emit();
            });
    }

    setShowBalance(show) {
        this._showBalance = !!show;
        if (!this._showBalance)
            this._balanceMsat = null;
        this._maybeFetch();
        this._emit();
    }

    /** One-shot refresh (menu opened): refetch if stale or not yet granted. */
    refresh(maxAgeSeconds) {
        const ageUs = GLib.get_monotonic_time() - this._fetchedAt;
        if (this._balanceMsat === null || ageUs > maxAgeSeconds * GLib.USEC_PER_SEC)
            this._maybeFetch();
    }

    /**
     * Explicit user action only ("Allow balance access…"): one plain
     * GetBalance that nostr-wallet-agent may answer with its approval dialog,
     * where the user can tick "Always allow this app".
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
                    this._error = null;
                } catch (e) {
                    if (isCancelled(e))
                        return;
                    this._error = errorMessage(e);
                }
                // Whether that answer came with a standing grant:
                this._maybeFetch();
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
        this._onChanged = null;
    }

    // ── internals ──────────────────────────────────────────────────────

    _emit() {
        if (!this._destroyed)
            this._onChanged?.(this.state);
    }

    _maybeFetch() {
        const s = this.state;
        if (this._destroyed || !this._proxy || this._granting || !s.showBalance ||
            !s.agentRunning || s.paired !== true)
            return;
        if (this._fetching) {
            this._fetchAgain = true;
            return;
        }
        this._fetching = true;
        this._proxy.call('GetBalanceNonInteractive', null, Gio.DBusCallFlags.NO_AUTO_START,
            BALANCE_TIMEOUT, this._cancellable, (p, res) => {
                if (this._destroyed)
                    return;
                this._fetching = false;
                try {
                    const [msat] = p.call_finish(res).deepUnpack();
                    this._balanceMsat = msat;
                    this._fetchedAt = GLib.get_monotonic_time();
                    this._readAllowed = true;
                    this._error = null;
                } catch (e) {
                    if (isCancelled(e))
                        return;
                    const kind = classifyWalletError(remoteErrorName(e));
                    this._readAllowed = false;
                    if (kind === 'needs-grant') {
                        // Keep a balance the user approved once; drop it on the
                        // next payment (it cannot be refreshed without asking).
                        this._error = null;
                    } else {
                        this._balanceMsat = null;
                        if (kind !== 'unpaired' && kind !== 'not-running')
                            this._error = errorMessage(e);
                    }
                }
                this._emit();
                if (this._fetchAgain) {
                    this._fetchAgain = false;
                    this._maybeFetch();
                }
            });
    }
}
