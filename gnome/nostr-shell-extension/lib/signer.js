// SPDX-License-Identifier: MIT
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {dbusCall, isCancelled} from './dbus.js';

const NAME = 'org.nostr.Signer';
const PATH = '/org/nostr/signer/grotto';
const IFACE = 'org.nostr.Grotto.Control1';
const APP_NAME = 'org.nostr.Grotto';
const APP_PATH = '/org/nostr/Grotto';
const SHELL_NAME = 'org.nostr.Shell.Signer1';

/** Coarse session status only. No request or identity enumeration. */
export class SignerMonitor {
    constructor(onChanged, {bus = null} = {}) {
        this._bus = bus ?? Gio.DBus.session;
        this._onChanged = onChanged;
        this._cancellable = new Gio.Cancellable();
        this._proxy = null;
        this._proxyIds = [];
        this._nameId = 0;
        this._serial = 0;
        this._fetching = false;
        this._fetchAgain = false;
        this._status = null;
        this._actionError = null;
        this._busy = false;
        this._destroyed = false;
    }

    get state() {
        const running = !!this._proxy?.get_name_owner();
        const status = running ? this._status : null;
        const pendingCount = status?.pendingCount ?? 0;
        const paused = status?.paused ?? false;
        const bunkerState = status?.bunkerState ?? 'DISABLED';
        const bunkerProblem = !!status?.bunkerEnabled && bunkerState !== 'LISTENING';
        const phase = !running ? 'stopped' : !status ? 'unavailable' :
            paused ? 'paused' : pendingCount ? 'waiting' : bunkerProblem ? 'bunker' : 'ready';
        return {
            running, phase, pendingCount, badge: pendingCount ? String(pendingCount) : '',
            paused, bunkerState, bunkerProblem,
            canPause: !!status?.canPause && !paused && !this._busy,
            busy: this._busy,
            actionError: this._actionError,
        };
    }

    start() {
        if (this._destroyed || this._nameId)
            return;
        this._nameId = Gio.bus_own_name_on_connection(this._bus, SHELL_NAME,
            Gio.BusNameOwnerFlags.NONE, null, null);
        Gio.DBusProxy.new(this._bus,
            Gio.DBusProxyFlags.DO_NOT_AUTO_START | Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES,
            null, NAME, PATH, IFACE, this._cancellable, (src, res) => {
                let proxy;
                try {
                    proxy = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    if (!isCancelled(e))
                        this._emit();
                    return;
                }
                if (this._destroyed)
                    return;
                this._proxy = proxy;
                this._proxyIds = [
                    proxy.connect('g-signal', (p, sender, signal) => {
                        if (signal === 'RequestsChanged' || signal === 'StatusChanged')
                            this.refresh();
                    }),
                    proxy.connect('notify::g-name-owner', () => {
                        this._status = null;
                        this._actionError = null;
                        this.refresh();
                        this._emit();
                    }),
                ];
                this.refresh();
                this._emit();
            });
    }

    refresh() {
        if (this._destroyed)
            return;
        this._serial++;
        if (this._fetching) {
            this._fetchAgain = true;
            return;
        }
        this._fetchStatus();
    }

    async _fetchStatus() {
        const owner = this._proxy?.get_name_owner();
        if (!owner || this._destroyed)
            return;
        const serial = this._serial;
        this._fetching = true;
        try {
            // Pin the call to the observed owner: a replacement cannot answer it.
            const reply = await dbusCall(this._bus, {
                name: owner, path: PATH, iface: IFACE, method: 'GetStatus', reply: '(a{sv})',
                flags: Gio.DBusCallFlags.NO_AUTO_START, cancellable: this._cancellable,
            });
            const row = reply.get_child_value(0);
            const value = (key, type) => row.lookup_value(key, new GLib.VariantType(type))?.deepUnpack();
            const version = value('control_version', 'u');
            const paused = value('paused', 'b');
            const pendingCount = value('pending_count', 'u');
            const bunkerEnabled = value('bunker_enabled', 'b');
            const bunkerState = value('bunker_state', 's');
            if (version !== 1 || paused === undefined || pendingCount === undefined ||
                bunkerEnabled === undefined || bunkerState === undefined)
                throw new Error('Unsupported coarse status');
            if (this._current(owner, serial)) {
                this._status = {paused, pendingCount, bunkerEnabled, bunkerState,
                    canPause: value('capabilities', 'a{sb}')?.pause === true};
                this._emit();
            }
        } catch (e) {
            if (!isCancelled(e) && this._current(owner, serial)) {
                this._status = null;
                this._emit();
            }
        } finally {
            this._fetching = false;
            if (this._fetchAgain && !this._destroyed) {
                this._fetchAgain = false;
                this._fetchStatus();
            }
        }
    }

    _current(owner, serial) {
        return !this._destroyed && serial === this._serial &&
            owner === this._proxy?.get_name_owner();
    }

    async pause() {
        if (this._destroyed || !this.state.canPause)
            return;
        const owner = this._proxy.get_name_owner();
        await this._action({name: owner, path: PATH, iface: IFACE, method: 'SetPaused',
            params: new GLib.Variant('(b)', [true]), flags: Gio.DBusCallFlags.NO_AUTO_START},
        'pause');
    }

    review() {
        // G07's action requires a string target; the empty string opens the inbox.
        return this._activateAction('review-request', [new GLib.Variant('s', '')]);
    }

    resume() {
        // Only the trusted GUI may resume, after its confirmation.
        return this._activateAction('resume-signing', []);
    }

    open() {
        return this._action({name: APP_NAME, path: APP_PATH,
            iface: 'org.freedesktop.Application', method: 'Activate',
            params: new GLib.Variant('(a{sv})', [{}])}, 'open');
    }

    _activateAction(action, targets) {
        return this._action({name: APP_NAME, path: APP_PATH,
            iface: 'org.freedesktop.Application', method: 'ActivateAction',
            params: new GLib.Variant('(sava{sv})', [action, targets, {}])}, action);
    }

    async _action(call, action) {
        if (this._destroyed || this._busy)
            return;
        this._busy = true;
        this._actionError = null;
        this._emit();
        try {
            await dbusCall(this._bus, {...call, reply: '()', cancellable: this._cancellable});
        } catch (e) {
            if (!isCancelled(e) && !this._destroyed)
                this._actionError = action;
        } finally {
            this._busy = false;
            if (!this._destroyed) {
                this.refresh();
                this._emit();
            }
        }
    }

    _emit() {
        if (!this._destroyed)
            this._onChanged?.(this.state);
    }

    destroy() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        this._serial++;
        this._cancellable.cancel();
        for (const id of this._proxyIds)
            this._proxy.disconnect(id);
        this._proxyIds = [];
        this._proxy = null;
        if (this._nameId)
            Gio.bus_unown_name(this._nameId);
        this._nameId = 0;
        this._status = null;
        this._onChanged = null;
    }
}
