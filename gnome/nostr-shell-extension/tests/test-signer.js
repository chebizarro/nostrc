// SPDX-License-Identifier: MIT
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';
import {SignerMonitor} from '../lib/signer.js';

const NAME = 'org.nostr.Signer';
const PATH = '/org/nostr/signer/grotto';
const IFACE = 'org.nostr.Grotto.Control1';
const SHELL_NAME = 'org.nostr.Shell.Signer1';
const APP_NAME = 'org.nostr.Grotto';
const APP_PATH = '/org/nostr/Grotto';
const ctx = GLib.MainContext.default();
let checks = 0;
function assert(value, message) {
    if (!value)
        throw new Error(message);
    checks++;
}
function waitFor(predicate, message) {
    let expired = false;
    const timeout = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 5000, () => {
        expired = true;
        return GLib.SOURCE_REMOVE;
    });
    try {
        while (!predicate() && !expired)
            ctx.iteration(true);
        assert(!expired, `Timed out: ${message}`);
    } finally {
        if (!expired)
            GLib.source_remove(timeout);
    }
}
function connection() {
    return Gio.DBusConnection.new_for_address_sync(GLib.getenv('DBUS_SESSION_BUS_ADDRESS'),
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        null, null);
}
function hasName(bus, name) {
    return bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
        'org.freedesktop.DBus', 'NameHasOwner', new GLib.Variant('(s)', [name]),
        new GLib.VariantType('(b)'), Gio.DBusCallFlags.NONE, 5000, null).deepUnpack()[0];
}
function own(bus, name) {
    let acquired = false;
    const id = Gio.bus_own_name_on_connection(bus, name, Gio.BusNameOwnerFlags.NONE,
        () => { acquired = true; }, null);
    waitFor(() => acquired, `acquire ${name}`);
    return id;
}
const [, xmlBytes] = GLib.file_get_contents('../../apps/grotto/data/org.nostr.Grotto.Control1.xml');
const controlXML = new TextDecoder().decode(xmlBytes);
let version = 1;
let count = 0;
let paused = false;
let bunkerEnabled = false;
let bunkerState = 'DISABLED';
let failStatus = false;
let failPause = false;
let hold = false;
let held = null;
let revision = 1;
const calls = [];
function snapshot() {
    return new GLib.Variant('(a{sv})', [{
        control_version: new GLib.Variant('u', version),
        paused: new GLib.Variant('b', paused),
        pending_count: new GLib.Variant('u', count),
        bunker_enabled: new GLib.Variant('b', bunkerEnabled),
        bunker_state: new GLib.Variant('s', bunkerState),
        capabilities: new GLib.Variant('a{sb}', {pause: true}),
        // A daemon extension must not leak through the monitor's whitelist.
        identity: new GLib.Variant('s', 'must-not-reach-panel'),
    }]);
}
function exportControl(bus) {
    const object = Gio.DBusExportedObject.wrapJSObject(controlXML, {
        GetStatusAsync(params, invocation) {
            calls.push('GetStatus');
            if (hold) {
                held = {invocation, reply: snapshot()};
                return;
            }
            if (failStatus)
                invocation.return_dbus_error('org.nostr.Grotto.Error.NotReady', 'unavailable');
            else
                invocation.return_value(snapshot());
        },
        SetPausedAsync(params, invocation) {
            calls.push(['SetPaused', ...params]);
            if (failPause) {
                invocation.return_dbus_error('org.nostr.Grotto.Error.NotReady', 'pause failed');
                return;
            }
            if (params[0] !== true) {
                invocation.return_dbus_error('org.nostr.Grotto.Error.NotAuthorized', 'resume forbidden');
                return;
            }
            paused = true;
            invocation.return_value(new GLib.Variant('()', []));
            bus.emit_signal(null, PATH, IFACE, 'StatusChanged', new GLib.Variant('(t)', [revision++]));
        },
    });
    object.export(bus, PATH);
    return object;
}
function changed(bus, signal = 'RequestsChanged') {
    bus.emit_signal(null, PATH, IFACE, signal, new GLib.Variant('(t)', [revision++]));
}
const appCalls = [];
const appXML = '<node><interface name="org.freedesktop.Application">' +
    '<method name="Activate"><arg type="a{sv}" direction="in"/></method>' +
    '<method name="ActivateAction"><arg type="s" direction="in"/><arg type="av" direction="in"/>' +
    '<arg type="a{sv}" direction="in"/></method></interface></node>';

const client = connection();
const service = connection();
const replacement = connection();
const app = connection();
const objects = [];
const nameIds = [];
let emissions = 0;
const states = [];
let monitor = new SignerMonitor(state => { emissions++; states.push(state); }, {bus: client});
let failure = null;
try {
    monitor.start();
    waitFor(() => emissions > 0 && hasName(client, SHELL_NAME), 'monitor and suppression name');
    assert(monitor.state.phase === 'stopped', 'absent signer is not running');
    assert(!GLib.file_test(`${GLib.getenv('NSE_TEST_TMP')}/activated`, GLib.FileTest.EXISTS),
        'status proxy must not auto-start the signer');
    assert(!monitor.state.canPause && monitor.state.badge === '', 'absent signer has no action/badge');

    objects.push(exportControl(service));
    const serviceName = own(service, NAME);
    nameIds.push(serviceName);
    waitFor(() => monitor.state.phase === 'ready', 'daemon appears');
    assert(!JSON.stringify(monitor.state).includes('must-not-reach-panel'), 'identity-free state whitelist');
    count = 3;
    changed(service);
    waitFor(() => monitor.state.badge === '3', 'RequestsChanged updates badge');
    assert(monitor.state.phase === 'waiting' && monitor.state.pendingCount === 3, 'waiting count');
    count = 0;
    changed(service);
    waitFor(() => monitor.state.phase === 'ready' && monitor.state.badge === '', 'badge clears');

    bunkerState = 'OFFLINE';
    changed(service, 'StatusChanged');
    const before = calls.length;
    waitFor(() => calls.length > before, 'disabled bunker status read');
    // Wait for the corresponding reply through a subsequent known change.
    count = 1;
    changed(service);
    waitFor(() => monitor.state.pendingCount === 1, 'status reply');
    assert(!monitor.state.bunkerProblem, 'disabled bunker never reports offline');
    bunkerEnabled = true;
    bunkerState = 'DEGRADED';
    count = 0;
    changed(service, 'StatusChanged');
    waitFor(() => monitor.state.phase === 'bunker', 'enabled bunker degradation');
    assert(monitor.state.bunkerState === 'DEGRADED', 'coarse degraded state');
    bunkerState = 'OFFLINE';
    changed(service, 'StatusChanged');
    waitFor(() => monitor.state.bunkerState === 'OFFLINE', 'enabled bunker offline');
    bunkerState = 'LISTENING';
    changed(service, 'StatusChanged');
    waitFor(() => monitor.state.phase === 'ready', 'bunker recovers');

    failPause = true;
    monitor.pause();
    waitFor(() => monitor.state.actionError === 'pause' && !monitor.state.busy, 'pause failure visible');
    assert(!monitor.state.paused, 'failed pause never claims success');
    failPause = false;
    monitor.pause();
    waitFor(() => monitor.state.phase === 'paused' && !monitor.state.busy, 'pause round trip');
    assert(calls.filter(call => Array.isArray(call)).every(call => call[1] === true), 'only SetPaused(true)');

    const appObject = Gio.DBusExportedObject.wrapJSObject(appXML, {
        Activate(params) { appCalls.push(['Activate', params]); },
        ActivateAction(action, targets, platform) { appCalls.push([action, targets, platform]); },
    });
    appObject.export(app, APP_PATH);
    objects.push(appObject);
    nameIds.push(own(app, APP_NAME));
    monitor.review();
    waitFor(() => appCalls.length === 1 && !monitor.state.busy, 'review activation');
    assert(appCalls[0][0] === 'review-request' && appCalls[0][1].length === 1 &&
        appCalls[0][1][0].unpack() === '', 'review has the required empty-string target');
    monitor.resume();
    waitFor(() => appCalls.length === 2 && !monitor.state.busy, 'resume activation');
    assert(appCalls[1][0] === 'resume-signing' && appCalls[1][1].length === 0,
        'resume goes only through the GUI action');
    assert(monitor.state.paused, 'GUI activation itself does not resume the daemon');
    monitor.open();
    waitFor(() => appCalls.length === 3 && !monitor.state.busy, 'open activation');
    assert(appCalls[2][0] === 'Activate', 'Open Grotto uses Activate');

    paused = false;
    version = 2;
    changed(service, 'StatusChanged');
    waitFor(() => monitor.state.phase === 'unavailable', 'unsupported Control1 is read-only');
    assert(!monitor.state.canPause && monitor.state.badge === '', 'skew clears status/action');
    version = 1;
    changed(service, 'StatusChanged');
    waitFor(() => monitor.state.phase === 'ready', 'version recovers');
    failStatus = true;
    changed(service, 'StatusChanged');
    waitFor(() => monitor.state.phase === 'unavailable', 'status read failure');
    failStatus = false;
    changed(service, 'StatusChanged');
    waitFor(() => monitor.state.phase === 'ready', 'status read recovers');

    // Supersede an in-flight snapshot with a newer RequestsChanged revision.
    hold = true;
    count = 100;
    monitor.refresh();
    waitFor(() => held !== null, 'held snapshot');
    let observed = false;
    const signalId = client.signal_subscribe(null, IFACE, 'RequestsChanged', PATH, null,
        Gio.DBusSignalFlags.NONE, () => { observed = true; });
    count = 8;
    const stateStart = states.length;
    changed(service);
    waitFor(() => observed, 'new revision delivered before stale reply');
    client.signal_unsubscribe(signalId);
    hold = false;
    held.invocation.return_value(held.reply);
    held = null;
    waitFor(() => monitor.state.badge === '8', 'new snapshot replaces stale response');
    assert(states.slice(stateStart).every(state => state.pendingCount !== 100), 'stale snapshot never rendered');

    hold = true;
    monitor.refresh();
    waitFor(() => held !== null, 'old-owner snapshot held');
    Gio.bus_unown_name(serviceName);
    nameIds.splice(nameIds.indexOf(serviceName), 1);
    waitFor(() => monitor.state.phase === 'stopped', 'daemon vanishes');
    assert(monitor.state.badge === '' && !monitor.state.canPause, 'owner loss clears badge/action');
    objects.push(exportControl(replacement));
    count = 2;
    hold = false;
    nameIds.push(own(replacement, NAME));
    held.invocation.return_value(held.reply);
    held = null;
    waitFor(() => monitor.state.badge === '2', 'replacement daemon snapshot');
    assert(calls.every(call => call === 'GetStatus' || (call[0] === 'SetPaused' && call[1] === true)),
        'no approval, sensitive enumeration, or resume method calls');

    hold = true;
    monitor.refresh();
    waitFor(() => held !== null, 'disable with call in flight');
    monitor.destroy();
    monitor.destroy();
    waitFor(() => !hasName(client, SHELL_NAME), 'suppression name released');
    const disabledEmissions = emissions;
    hold = false;
    held.invocation.return_value(held.reply);
    held = null;
    changed(replacement);
    monitor = new SignerMonitor(() => {}, {bus: client});
    monitor.start();
    waitFor(() => hasName(client, SHELL_NAME) && monitor.state.badge === '2', 'clean re-enable');
    assert(emissions === disabledEmissions, 'destroyed monitor never emits again');
    monitor.destroy();
    waitFor(() => !hasName(client, SHELL_NAME), 're-enabled name released');
    assert(!GLib.file_test(`${GLib.getenv('NSE_TEST_TMP')}/activated`, GLib.FileTest.EXISTS),
        'no incidental signer activation');
    print(`signer: ${checks} checks passed`);
} catch (e) {
    failure = e;
} finally {
    monitor.destroy();
    for (const id of nameIds)
        Gio.bus_unown_name(id);
    for (const object of objects)
        object.unexport();
    for (const bus of [client, service, replacement, app])
        bus.close_sync(null);
}
if (failure) {
    printerr(failure.stack ?? String(failure));
    System.exit(1);
}
