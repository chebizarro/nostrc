// SPDX-License-Identifier: MIT
//
// Small async helpers over Gio.DBusConnection.call(). Callback form, so they
// work whether or not gnome-shell has promisified Gio.DBusConnection.call.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

/**
 * @returns {Promise<GLib.Variant>}
 */
export function dbusCall(bus, {name, path, iface, method, params = null, reply = null,
    flags = Gio.DBusCallFlags.NONE, timeout = 5000, cancellable = null}) {
    return new Promise((resolve, reject) => {
        bus.call(name, path, iface, method, params,
            reply ? new GLib.VariantType(reply) : null,
            flags, timeout, cancellable, (conn, res) => {
                try {
                    resolve(conn.call_finish(res));
                } catch (e) {
                    reject(e);
                }
            });
    });
}

export function isCancelled(e) {
    return e instanceof GLib.Error && e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED);
}

/** "org.nostr.Wallet1.Error.Denied" or null. */
export function remoteErrorName(e) {
    try {
        return e instanceof GLib.Error ? Gio.DBusError.get_remote_error(e) : null;
    } catch {
        return null;
    }
}

/** Error message without the "GDBus.Error:name: " prefix. */
export function errorMessage(e) {
    if (e instanceof GLib.Error) {
        const copy = e.message.replace(/^GDBus\.Error:[^ ]+: /, '');
        return copy || String(e);
    }
    return String(e?.message ?? e);
}
