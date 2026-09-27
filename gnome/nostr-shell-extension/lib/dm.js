// SPDX-License-Identifier: MIT
//
// DmMonitor — unread encrypted direct messages from nostr-notify-daemon's
// org.nostr.NotifyDaemon1 (/org/nostr/NotifyDaemon1 on the daemon's name
// org.nostr.NotifyDaemon; gnome/dbus/org.nostr.NotifyDaemon1.xml,
// nostrc-prqu.18). A count only: the daemon never exposes content, sender
// or conversation.
//
// DO_NOT_AUTO_START: the panel never launches the daemon; while it is not
// on the bus the count is null and the row falls back to the service state.

import Gio from 'gi://Gio';

import {isCancelled} from './dbus.js';

const N_NAME = 'org.nostr.NotifyDaemon';
const N_PATH = '/org/nostr/NotifyDaemon1';
const N_IFACE = 'org.nostr.NotifyDaemon1';

export class DmMonitor {
    constructor(onChanged, {bus = null} = {}) {
        this._onChanged = onChanged;
        this._bus = bus ?? Gio.DBus.session;
        this._cancellable = new Gio.Cancellable();
        this._proxy = null;
        this._ids = [];
        this._destroyed = false;
    }

    /** {unread: number|null} — null while the daemon is not on the bus. */
    get state() {
        if (!this._proxy?.get_name_owner())
            return {unread: null};
        const v = this._proxy.get_cached_property('UnreadDirectMessages');
        return {unread: v ? v.unpack() : null};
    }

    start() {
        Gio.DBusProxy.new(this._bus, Gio.DBusProxyFlags.DO_NOT_AUTO_START, null,
            N_NAME, N_PATH, N_IFACE, this._cancellable, (src, res) => {
                let proxy;
                try {
                    proxy = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    if (!isCancelled(e))
                        logError(e);
                    return;
                }
                if (this._destroyed)
                    return;
                this._proxy = proxy;
                this._ids = [
                    proxy.connect('g-properties-changed', () => this._emit()),
                    proxy.connect('notify::g-name-owner', () => this._emit()),
                ];
                this._emit();
            });
    }

    destroy() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        this._cancellable.cancel();
        for (const id of this._ids)
            this._proxy?.disconnect(id);
        this._ids = [];
        this._proxy = null;
        this._onChanged = null;
    }

    _emit() {
        if (!this._destroyed)
            this._onChanged?.(this.state);
    }
}
