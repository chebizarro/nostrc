// SPDX-License-Identifier: MIT
//
// Integration test for lib/relay.js and lib/wallet.js — the real monitor code
// the extension runs — on a private bus against fakes of
// org.freedesktop.systemd1, org.nostr.SessionRelay1 and org.nostr.Wallet1.
//
//   tests/run-monitors.sh          (private bus via dbus-run-session; Linux)
//
// Proves: exact on/off plan order (same as nostr-settings), stats only while
// the relay owns its name, the relay and the wallet agent are never
// D-Bus-activated (their fakes are activatable and would leave a marker),
// GetBalance is never called without an allow_read grant for this process's
// own caller id, is called once the grant appears, is refreshed on
// PaymentReceived, and that destroy() unsubscribes and stops all activity.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';

import {RelayMonitor} from '../lib/relay.js';
import {DmMonitor} from '../lib/dm.js';
import {WalletMonitor} from '../lib/wallet.js';
import {dmView, relayView, walletView} from '../lib/state.js';

let count = 0, failures = 0;

// Hard watchdog: a hung wait must never leave this process behind. Fires
// from the main context that waitFor()/spin() iterate; ctest/`timeout -s
// KILL` wrappers cover anything blocking outside it.
const WATCHDOG_SECONDS = 90;
GLib.timeout_add_seconds(GLib.PRIORITY_HIGH, WATCHDOG_SECONDS, () => {
    print(`Bail out! watchdog: no progress after ${WATCHDOG_SECONDS} s`);
    System.exit(2);
    return GLib.SOURCE_REMOVE;
});
function ok(cond, what, extra = '') {
    count++;
    if (cond) {
        print(`ok ${count} - ${what}`);
    } else {
        failures++;
        print(`not ok ${count} - ${what}${extra ? `\n#   ${extra}` : ''}`);
    }
}
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

function waitFor(pred, what, ms = 5000) {
    const ctx = GLib.MainContext.default();
    const deadline = GLib.get_monotonic_time() + ms * 1000;
    let tick = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 20, () => GLib.SOURCE_CONTINUE);
    while (!pred() && GLib.get_monotonic_time() < deadline)
        ctx.iteration(true);
    GLib.Source.remove(tick);
    const r = pred();
    if (!r)
        print(`# timed out waiting for: ${what}`);
    return r;
}
function spin(ms) {
    let done = false;
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
        done = true;
        return GLib.SOURCE_REMOVE;
    });
    waitFor(() => done, 'spin', ms + 1000);
}

// ── private bus (tests/run-monitors.sh) with activatable fakes ───────────
const tmp = GLib.getenv('NSE_TEST_TMP');
const addr = GLib.getenv('DBUS_SESSION_BUS_ADDRESS');
if (!tmp || !addr) {
    print('Bail out! run through tests/run-monitors.sh (needs NSE_TEST_TMP and a private bus)');
    System.exit(2);
}
const relayMarker = GLib.build_filenamev([tmp, 'org.nostr.SessionRelay1.activated']);
const walletMarker = GLib.build_filenamev([tmp, 'org.nostr.Wallet1.activated']);
const notifyMarker = GLib.build_filenamev([tmp, 'org.nostr.NotifyDaemon.activated']);

const connect = () => Gio.DBusConnection.new_for_address_sync(addr,
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
    null, null);
const client = connect();
const service = connect();

// ── fake systemd --user ──────────────────────────────────────────────────
const MGR_XML = `<node><interface name="org.freedesktop.systemd1.Manager">
  <method name="LoadUnit"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
  <method name="Subscribe"/><method name="Unsubscribe"/>
  <method name="EnableUnitFiles"><arg type="as" direction="in"/><arg type="b" direction="in"/>
    <arg type="b" direction="in"/><arg type="b" direction="out"/><arg type="a(sss)" direction="out"/></method>
  <method name="DisableUnitFiles"><arg type="as" direction="in"/><arg type="b" direction="in"/>
    <arg type="a(sss)" direction="out"/></method>
  <method name="Reload"/>
  <method name="StartUnit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
  <method name="StopUnit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
  <signal name="UnitFilesChanged"/><signal name="Reloading"><arg type="b"/></signal>
</interface></node>`;
const UNIT_XML = `<node><interface name="org.freedesktop.systemd1.Unit">
  <property name="LoadState" type="s" access="read"/><property name="ActiveState" type="s" access="read"/>
  <property name="SubState" type="s" access="read"/><property name="UnitFileState" type="s" access="read"/>
  <property name="Job" type="(uo)" access="read"/>
</interface></node>`;

const esc = n => n.replace(/[^A-Za-z0-9]/g, c => `_${c.charCodeAt(0).toString(16).padStart(2, '0')}`);
const unitPath = n => `/org/freedesktop/systemd1/unit/${esc(n)}`;
const calls = [];
const units = {
    'nostr-session-relay.socket': {LoadState: 'loaded', ActiveState: 'inactive', SubState: 'dead', UnitFileState: 'disabled', Job: [0, '/']},
    'nostr-session-relay.service': {LoadState: 'loaded', ActiveState: 'inactive', SubState: 'dead', UnitFileState: 'static', Job: [0, '/']},
    'nostr-notify.service': {LoadState: 'loaded', ActiveState: 'active', SubState: 'running', UnitFileState: 'enabled', Job: [0, '/']},
};
const unitObjs = {};
let failEnable = false;
function setUnit(name, props) {
    Object.assign(units[name], props);
    for (const [k, v] of Object.entries(props))
        unitObjs[name].emit_property_changed(k, new GLib.Variant('s', v));
}
// Like systemd: Start/StopUnit return once the job is queued; it finishes
// later. No PropertiesChanged for the change (nobody called Subscribe), so
// the monitor must notice the pending Job and settle by itself.
function queueJob(name, props) {
    units[name].Job = [7, '/org/freedesktop/systemd1/job/7'];
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 300, () => {
        Object.assign(units[name], props, {Job: [0, '/']});
        return GLib.SOURCE_REMOVE;
    });
}
for (const name of Object.keys(units)) {
    const impl = {};
    for (const p of ['LoadState', 'ActiveState', 'SubState', 'UnitFileState', 'Job'])
        Object.defineProperty(impl, p, {get: () => units[name][p]});
    unitObjs[name] = Gio.DBusExportedObject.wrapJSObject(UNIT_XML, impl);
    unitObjs[name].export(service, unitPath(name));
}
let mgr;
const mgrImpl = {
    LoadUnit(n) {
        calls.push(`LoadUnit(${n})`);
        return unitPath(n);
    },
    Subscribe() {
        calls.push('Subscribe');
    },
    Unsubscribe() {
        calls.push('Unsubscribe');
    },
    EnableUnitFiles(names) {
        calls.push(`EnableUnitFiles(${names})`);
        if (failEnable)
            throw new Error('Access denied');
        for (const n of names)
            units[n].UnitFileState = 'enabled';
        mgr.emit_signal('UnitFilesChanged', null);
        return [false, []];
    },
    DisableUnitFiles(names) {
        calls.push(`DisableUnitFiles(${names})`);
        for (const n of names)
            units[n].UnitFileState = 'disabled';
        mgr.emit_signal('UnitFilesChanged', null);
        return [];
    },
    Reload() {
        calls.push('Reload');
    },
    StartUnit(n) {
        calls.push(`StartUnit(${n})`);
        if (n.endsWith('.socket'))
            queueJob(n, {ActiveState: 'active', SubState: 'listening'});
        return '/org/freedesktop/systemd1/job/1';
    },
    StopUnit(n) {
        calls.push(`StopUnit(${n})`);
        queueJob(n, {ActiveState: 'inactive', SubState: 'dead'});
        return '/org/freedesktop/systemd1/job/2';
    },
};
mgr = Gio.DBusExportedObject.wrapJSObject(MGR_XML, mgrImpl);
mgr.export(service, '/org/freedesktop/systemd1');
let sdOwned = false;
Gio.bus_own_name_on_connection(service, 'org.freedesktop.systemd1', Gio.BusNameOwnerFlags.NONE,
    () => {
        sdOwned = true;
    }, null);
waitFor(() => sdOwned, 'systemd name');

// ── relay monitor ─────────────────────────────────────────────────────────
let relayState = null, relayEvents = 0;
const relay = new RelayMonitor(s => {
    relayState = s;
    relayEvents++;
}, client);
relay.start(30);
ok(waitFor(() => relayState?.available && relayState.socket, 'initial resync'), 'relay: initial state read');
ok(relayView(relayState).status === 'off' && !relayView(relayState).checked, 'relay: starts off/unchecked');
ok(relayState.notify?.activeState === 'active', 'relay: nostr-notify state read');
ok(!calls.includes('Subscribe'), 'relay: never calls Manager.Subscribe (shared shell connection)');

calls.length = 0;
relay.setEnabled(true);
ok(relayView(relayState).subtitle === 'Turning on…', 'relay: busy while plan runs');
ok(waitFor(() => !relayState.busy && relayView(relayState).status === 'listening', 'on'),
    'relay: on → listening (queued job settles without signals)');
const planOn = calls.filter(c => !c.startsWith('LoadUnit'));
ok(same(planOn, ['EnableUnitFiles(nostr-session-relay.socket)', 'Reload',
    'StartUnit(nostr-session-relay.socket)']), 'relay: on plan order matches nostr-settings',
JSON.stringify(planOn));
ok(relayView(relayState).checked, 'relay: checked after on');

// Relay starts (socket activation by some app) → owns its name → stats.
setUnit('nostr-session-relay.service', {ActiveState: 'active', SubState: 'running'});
const RELAY_XML = `<node><interface name="org.nostr.SessionRelay1">
  <method name="GetStats"><arg name="stats" type="a{sv}" direction="out"/></method></interface></node>`;
let statsCalls = 0;
const relayObj = Gio.DBusExportedObject.wrapJSObject(RELAY_XML, {
    GetStats() {
        statsCalls++;
        return {
            event_count: new GLib.Variant('x', 1234), storage_bytes: new GLib.Variant('t', 12345678),
            connected_clients: new GLib.Variant('u', 2), uptime: new GLib.Variant('t', 90),
            storage_backend: new GLib.Variant('s', 'nostrdb'),
        };
    },
});
ok(statsCalls === 0 && !relayState.stats, 'relay: no stats call while the name has no owner');
const relayConn = connect();
relayObj.export(relayConn, '/org/nostr/SessionRelay1');
const relayOwnId = Gio.bus_own_name_on_connection(relayConn, 'org.nostr.SessionRelay1',
    Gio.BusNameOwnerFlags.NONE, null, null);
ok(waitFor(() => relayState.stats?.eventCount === 1234, 'stats'), 'relay: stats fetched once the relay owns its name');
waitFor(() => relayView(relayState).status === 'running', 'service running');
ok(relayView(relayState, 'en-US').subtitle === '1,234 events · 12.3 MB', 'relay: subtitle shows stats',
    relayView(relayState, 'en-US').subtitle);

// Off: disable first, then stop service and socket; relay leaves the bus.
calls.length = 0;
relay.setEnabled(false);
ok(waitFor(() => !relayState.busy && relayView(relayState).status === 'off', 'off'),
    'relay: off → off (queued stop jobs settle without signals)');
spin(1200);
const settledEvents = relayEvents;
spin(2000);
ok(relayEvents === settledEvents, 'relay: settle stops once no job is pending (no steady polling)',
    `${relayEvents - settledEvents} re-reads in 2 s`);
const planOff = calls.filter(c => !c.startsWith('LoadUnit'));
ok(same(planOff, ['DisableUnitFiles(nostr-session-relay.socket)', 'Reload',
    'StopUnit(nostr-session-relay.service)', 'StopUnit(nostr-session-relay.socket)']),
'relay: off plan order matches nostr-settings', JSON.stringify(planOff));
Gio.bus_unown_name(relayOwnId);
relayObj.unexport();
ok(waitFor(() => relayState.stats === null, 'stats cleared'), 'relay: stats dropped when the name vanishes');

// A failing plan step surfaces an error and stops the plan.
calls.length = 0;
failEnable = true;
relay.setEnabled(true);
ok(waitFor(() => !relayState.busy && relayState.error, 'error'), 'relay: failing step reported');
ok(same(calls.filter(c => !c.startsWith('LoadUnit')), ['EnableUnitFiles(nostr-session-relay.socket)']),
    'relay: plan stops at the failing step', JSON.stringify(calls));
ok(relayView(relayState).subtitle === 'Error' && /Access denied/.test(relayView(relayState).statusLine),
    'relay: error shown', relayView(relayState).statusLine);
failEnable = false;

// ── wallet monitor ────────────────────────────────────────────────────────
// A fake agent with the non-interactive read (nostrc-prqu.19): it answers
// only once "granted", else InteractionRequired — like the real agent, it
// never prompts from GetBalanceNonInteractive. The plain GetBalance is the
// explicit "Allow balance access…" path.
const W_XML = `<node><interface name="org.nostr.Wallet1">
  <method name="GetBalance"><arg name="balance_msat" type="t" direction="out"/></method>
  <method name="GetBalanceNonInteractive"><arg name="balance_msat" type="t" direction="out"/></method>
  <signal name="PaymentReceived"><arg type="a{sv}"/></signal>
  <signal name="PaymentSent"><arg type="a{sv}"/></signal>
  <signal name="AppsChanged"/>
  <property name="Paired" type="b" access="read"/></interface></node>`;
let plainCalls = 0;
let quietCalls = 0;
let granted = false;
let balance = 21000000;
const walletObj = Gio.DBusExportedObject.wrapJSObject(W_XML, {
    get Paired() {
        return true;
    },
    GetBalance() {
        plainCalls++;
        return balance;
    },
    GetBalanceNonInteractive() {
        quietCalls++;
        if (!granted) {
            const e = new Error('reading the wallet needs the user\'s approval');
            e.name = 'org.nostr.Wallet1.Error.InteractionRequired';
            throw e;
        }
        return balance;
    },
});

let walletState = null;
const wallet = new WalletMonitor(s => {
    walletState = s;
}, {bus: client});
wallet.start({showBalance: true});
spin(300);
ok(walletView(walletState ?? {showBalance: true}).label === 'Wallet: agent not running',
    'wallet: agent absent → not running, not started');
ok(!GLib.file_test(walletMarker, GLib.FileTest.EXISTS), 'wallet: agent never D-Bus-activated');

const walletConn = connect();
walletObj.export(walletConn, '/org/nostr/Wallet1');
Gio.bus_own_name_on_connection(walletConn, 'org.nostr.Wallet1', Gio.BusNameOwnerFlags.NONE, null, null);
ok(waitFor(() => walletState?.agentRunning && walletState.paired === true, 'paired'), 'wallet: agent appears, Paired read');
ok(waitFor(() => quietCalls >= 1, 'non-interactive read'), 'wallet: asks with GetBalanceNonInteractive');
spin(200);
ok(plainCalls === 0, 'wallet: never the prompting GetBalance on its own');
ok(walletView(walletState).label === 'Wallet: —' && walletView(walletState).canGrant,
    'wallet: InteractionRequired → "—" with an explicit grant action');
ok(walletState.error === null, 'wallet: InteractionRequired is not an error');

// Granted elsewhere (Nostr Settings' GNOME Shell switch): the agent emits
// AppsChanged and the monitor asks again.
granted = true;
walletObj.emit_signal('AppsChanged', null);
ok(waitFor(() => walletState.balanceMsat !== null, 'balance', 5000), 'wallet: AppsChanged → balance fetched');
ok(walletState.readAllowed === true, 'wallet: read allowed');
ok(walletView(walletState, 'en-US').label === 'Wallet: 21,000 sats', 'wallet: label',
    walletView(walletState, 'en-US').label);

balance = 25000000;
walletObj.emit_signal('PaymentReceived', new GLib.Variant('(a{sv})', [{amount: new GLib.Variant('x', 4000000)}]));
ok(waitFor(() => walletState.balanceMsat === 25000000, 'refresh on payment'), 'wallet: refreshed on PaymentReceived');

const quietBefore = quietCalls;
wallet.refresh(3600);
spin(300);
ok(quietCalls === quietBefore, 'wallet: refresh() within max age uses the cache', String(quietCalls));

// Revoked: the next read says InteractionRequired; a payment drops the
// balance it can no longer refresh.
granted = false;
walletObj.emit_signal('AppsChanged', null);
ok(waitFor(() => walletState.readAllowed === false, 'revoked'), 'wallet: revoke noticed');
walletObj.emit_signal('PaymentSent', new GLib.Variant('(a{sv})', [{}]));
ok(waitFor(() => walletState.balanceMsat === null, 'dropped'), 'wallet: ungranted balance never shown stale');

// The explicit click: the plain (prompting) call, exactly once.
wallet.requestAccess();
ok(waitFor(() => plainCalls === 1 && !walletState.granting, 'grant click'), 'wallet: requestAccess → one GetBalance');
ok(walletState.balanceMsat === 25000000, 'wallet: approved balance shown');

wallet.setShowBalance(false);
ok(walletView(walletState).state === 'hidden', 'wallet: hidden by preference');

// ── unread direct messages (org.nostr.NotifyDaemon1, prqu.18) ─────────────
const N_XML = `<node><interface name="org.nostr.NotifyDaemon1">
  <property name="UnreadDirectMessages" type="u" access="read"/>
  <property name="LastDirectMessage" type="t" access="read"/>
  <method name="MarkRead"/></interface></node>`;
let unread = 2;
const notifyObj = Gio.DBusExportedObject.wrapJSObject(N_XML, {
    get UnreadDirectMessages() {
        return unread;
    },
    get LastDirectMessage() {
        return 1700000000;
    },
    MarkRead() {
        unread = 0;
    },
});
let dmState = null;
const dm = new DmMonitor(s => {
    dmState = s;
}, {bus: client});
dm.start();
spin(300);
ok((dmState?.unread ?? null) === null, 'dm: daemon absent → no count');
ok(!GLib.file_test(notifyMarker, GLib.FileTest.EXISTS), 'dm: daemon never D-Bus-activated');
const notifyConn = connect();
notifyObj.export(notifyConn, '/org/nostr/NotifyDaemon1');
Gio.bus_own_name_on_connection(notifyConn, 'org.nostr.NotifyDaemon', Gio.BusNameOwnerFlags.NONE, null, null);
ok(waitFor(() => dmState?.unread === 2, 'count'), 'dm: count read when the daemon appears');
const notifyOn = {available: true, notify: {loadState: 'loaded', activeState: 'active', subState: 'running',
    unitFileState: 'enabled'}};
ok(dmView(notifyOn, dmState.unread) === '2 unread direct messages', 'dm: row shows the count',
    dmView(notifyOn, dmState.unread));
unread = 3;
notifyObj.emit_property_changed('UnreadDirectMessages', new GLib.Variant('u', 3));
ok(waitFor(() => dmState.unread === 3, 'changed'), 'dm: PropertiesChanged updates the count');
notifyConn.close_sync(null);
ok(waitFor(() => dmState.unread === null, 'gone'), 'dm: daemon gone → count dropped');

// ── teardown ──────────────────────────────────────────────────────────────
calls.length = 0;
relay.destroy();
wallet.destroy();
spin(300);
ok(!calls.includes('Unsubscribe'), 'relay: destroy() never calls Manager.Unsubscribe');
dm.destroy();
const before = [relayEvents, plainCalls, quietCalls];
walletObj.emit_signal('PaymentSent', new GLib.Variant('(a{sv})', [{}]));
setUnit('nostr-session-relay.socket', {ActiveState: 'active', SubState: 'listening'});
spin(600);
ok(same([relayEvents, plainCalls, quietCalls], before), 'destroy(): no further callbacks or calls');
ok(!GLib.file_test(notifyMarker, GLib.FileTest.EXISTS), 'dm: never D-Bus-activated');
ok(!GLib.file_test(relayMarker, GLib.FileTest.EXISTS), 'relay: never D-Bus-activated');
ok(!GLib.file_test(walletMarker, GLib.FileTest.EXISTS), 'wallet: never D-Bus-activated');

for (const c of [client, service, relayConn, walletConn]) {
    try {
        c.close_sync(null);
    } catch {}
}
print(`1..${count}`);
if (failures) {
    print(`# ${failures} of ${count} failed`);
    System.exit(1);
}
// Success: let the module finish so gjs shuts down normally.
