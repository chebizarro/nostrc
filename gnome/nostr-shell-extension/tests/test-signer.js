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
// Adjacent uint64 revisions above 2^53 must remain distinguishable.
let requestsRevision = 9007199254740993n;
let statusRevision = 9007199254741093n;
let replyRequestsRevision = null;
let replyStatusRevision = null;
function uint64(value) {
    return GLib.Variant.parse(new GLib.VariantType('t'), String(value), null, null);
}
const calls = [];
function snapshot() {
    return new GLib.Variant('(a{sv})', [{
        control_version: new GLib.Variant('u', version),
        paused: new GLib.Variant('b', paused),
        pending_count: new GLib.Variant('u', count),
        bunker_enabled: new GLib.Variant('b', bunkerEnabled),
        bunker_state: new GLib.Variant('s', bunkerState),
        capabilities: new GLib.Variant('a{sb}', {pause: true}),
        requests_revision: uint64(replyRequestsRevision ?? requestsRevision),
        status_revision: uint64(replyStatusRevision ?? statusRevision),
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
            changed(bus, 'StatusChanged');
        },
    });
    object.export(bus, PATH);
    return object;
}
function emitRevision(bus, signal, revision) {
    const params = GLib.Variant.parse(new GLib.VariantType('(t)'), `(${revision},)`, null, null);
    bus.emit_signal(null, PATH, IFACE, signal, params);
}
function changed(bus, signal = 'RequestsChanged') {
    const revision = signal === 'RequestsChanged' ? ++requestsRevision : ++statusRevision;
    emitRevision(bus, signal, revision);
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
    monitor.quit();
    waitFor(() => monitor.state.actionError === 'quit' && !monitor.state.busy, 'absent GUI quit settles');
    assert(!hasName(client, APP_NAME) &&
        !GLib.file_test(`${GLib.getenv('NSE_TEST_TMP')}/gui-activated`, GLib.FileTest.EXISTS),
    'Quit approval UI never auto-starts an absent GUI');

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
    waitFor(() => monitor.state.actionError === 'pause' && monitor.state.canPause, 'pause failure visible');
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
    waitFor(() => appCalls.length === 1 && !monitor.state.busy && monitor.state.paused, 'review activation');
    assert(appCalls[0][0] === 'review-request' && appCalls[0][1].length === 1 &&
        appCalls[0][1][0].unpack() === '', 'review has the required empty-string target');
    monitor.resume();
    waitFor(() => appCalls.length === 2 && !monitor.state.busy && monitor.state.paused, 'resume activation');
    assert(appCalls[1][0] === 'resume-signing' && appCalls[1][1].length === 0,
        'resume goes only through the GUI action');
    assert(monitor.state.paused, 'GUI activation itself does not resume the daemon');
    monitor.open();
    waitFor(() => appCalls.length === 3 && !monitor.state.busy && monitor.state.paused, 'open activation');
    assert(appCalls[2][0] === 'Activate', 'Open Grotto uses Activate');
    monitor.quit();
    waitFor(() => appCalls.length === 4 && !monitor.state.busy && monitor.state.paused, 'quit activation');
    assert(appCalls[3][0] === 'quit' && appCalls[3][1].length === 0,
        'Quit approval UI goes only through the GUI action');
    assert(hasName(client, NAME) && paused, 'quit dispatch does not stop or resume the signer');

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

    let observedSignals = 0;
    const signalId = client.signal_subscribe(null, IFACE, null, PATH, null,
        Gio.DBusSignalFlags.NONE, () => { observedSignals++; });

    // Known 0->N and N->0 counts become unknown immediately on invalidation.
    // Multiple signals during one held read require exactly one successor read.
    for (const [from, latest] of [[0, 8], [5, 0]]) {
        count = from;
        changed(service);
        waitFor(() => monitor.state.pendingCount === from, `initial count ${from}`);
        hold = true;
        count = 100;
        monitor.refresh();
        waitFor(() => held !== null, 'held snapshot');
        const readStart = calls.filter(call => call === 'GetStatus').length;
        const stateStart = states.length;
        const signalStart = observedSignals;
        const emissionStart = emissions;
        for (let i = 0; i < 3; i++) {
            count = latest + 2 - i;
            changed(service);
        }
        waitFor(() => observedSignals >= signalStart + 3 && emissions >= emissionStart + 3,
            'all newer revisions invalidate while the snapshot is pending');
        assert(monitor.state.phase === 'refreshing' && monitor.state.pendingCount === null &&
            monitor.state.badge === '' && !monitor.state.canPause,
        'invalidation presents unknown, never the old count or a false Ready/zero');
        assert(calls.filter(call => call === 'GetStatus').length === readStart,
            'signals do not start overlapping reads');
        hold = false;
        held.invocation.return_value(held.reply);
        held = null;
        waitFor(() => monitor.state.pendingCount === latest, 'successor reaches latest announced revision');
        assert(monitor.state.badge === (latest ? String(latest) : '') &&
            monitor.state.phase === (latest ? 'waiting' : 'ready'), 'badge follows latest revision');
        assert(calls.filter(call => call === 'GetStatus').length === readStart + 1,
            'three invalidations coalesce into one successor read');
        assert(states.slice(stateStart).every(state => state.pendingCount === null ||
            state.pendingCount === latest), 'no superseded snapshot or fabricated zero is rendered');
    }

    // Equal/older signals do not invalidate an accepted snapshot or cause reads.
    for (const signal of ['RequestsChanged', 'StatusChanged']) {
        const revision = signal === 'RequestsChanged' ? requestsRevision : statusRevision;
        const signalStart = observedSignals;
        const readStart = calls.length;
        const emissionStart = emissions;
        emitRevision(service, signal, revision - 1n);
        emitRevision(service, signal, revision);
        waitFor(() => observedSignals >= signalStart + 2, 'lower/equal revisions delivered');
        while (ctx.pending())
            ctx.iteration(false);
        assert(calls.length === readStart && emissions === emissionStart && monitor.state.phase === 'ready',
            'lower/equal revision signals are ignored');
    }

    // A same-owner latest call may not return below either announced watermark.
    for (const signal of ['RequestsChanged', 'StatusChanged']) {
        count = 9;
        if (signal === 'RequestsChanged')
            replyRequestsRevision = requestsRevision;
        else
            replyStatusRevision = statusRevision;
        changed(service, signal);
        waitFor(() => monitor.state.phase === 'unavailable', 'snapshot below announced revision rejected');
        assert(monitor.state.pendingCount === null && monitor.state.badge === '', 'regressing count not rendered');
        replyRequestsRevision = null;
        replyStatusRevision = null;
        monitor.refresh();
        waitFor(() => monitor.state.badge === '9', 'equal watermark snapshot accepted');
    }
    // A newer snapshot advances the watermark even before its signal arrives.
    requestsRevision++;
    statusRevision++;
    count = 4;
    monitor.refresh();
    waitFor(() => monitor.state.badge === '4', 'newer snapshot accepted');
    replyRequestsRevision = requestsRevision - 1n;
    monitor.refresh();
    waitFor(() => monitor.state.phase === 'unavailable', 'previous accepted snapshot watermark retained');
    replyRequestsRevision = null;
    monitor.refresh();
    waitFor(() => monitor.state.badge === '4', 'current snapshot recovers');
    client.signal_unsubscribe(signalId);

    hold = true;
    monitor.refresh();
    waitFor(() => held !== null, 'old-owner snapshot held');
    Gio.bus_unown_name(serviceName);
    nameIds.splice(nameIds.indexOf(serviceName), 1);
    waitFor(() => monitor.state.phase === 'stopped', 'daemon vanishes');
    assert(monitor.state.badge === '' && !monitor.state.canPause, 'owner loss clears badge/action');
    objects.push(exportControl(replacement));
    count = 2;
    requestsRevision = 1n;
    statusRevision = 1n;
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
    assert(!GLib.file_test(`${GLib.getenv('NSE_TEST_TMP')}/gui-activated`, GLib.FileTest.EXISTS),
        'GUI quit activation stayed non-starting');
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
    printerr(`${failure.message}\n${failure.stack ?? ''}`);
    System.exit(1);
}
