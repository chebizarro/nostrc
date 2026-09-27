// SPDX-License-Identifier: MIT
//
// Unit tests for lib/state.js. Runs under Node (`node tests/run-tests.js`)
// and gjs (`gjs -m tests/run-tests.js`); no dependencies.

import * as S from '../lib/state.js';

let failures = 0;
let count = 0;

function fmt(v) {
    return typeof v === 'bigint' ? `${v}n` : JSON.stringify(v);
}

function eq(actual, expected, what) {
    count++;
    const a = fmt(actual), e = fmt(expected);
    if (a !== e) {
        failures++;
        print_(`not ok ${count} - ${what}\n#   expected ${e}\n#   actual   ${a}`);
    } else {
        print_(`ok ${count} - ${what}`);
    }
}

function print_(s) {
    if (typeof globalThis.print === 'function' && typeof globalThis.process === 'undefined')
        globalThis.print(s);
    else
        console.log(s);
}

const unit = (loadState, activeState, subState, unitFileState) =>
    ({loadState, activeState, subState, unitFileState});

// ── status model: mirrors nss_service_status (test_systemd.c table) ──────
{
    const off = unit('loaded', 'inactive', 'dead', 'disabled');
    const enabledIdle = unit('loaded', 'inactive', 'dead', 'enabled');
    const listening = unit('loaded', 'active', 'listening', 'enabled');
    const svcDead = unit('loaded', 'inactive', 'dead', 'static');
    const svcRun = unit('loaded', 'active', 'running', 'static');
    const svcStart = unit('loaded', 'activating', 'start', 'static');
    const svcFail = unit('loaded', 'failed', 'failed', 'static');
    const svcLoop = unit('loaded', 'activating', 'auto-restart', 'static');

    eq(S.serviceStatus(null, null), S.Status.NOT_INSTALLED, 'no units → not installed');
    eq(S.serviceStatus(unit('not-found', 'inactive', 'dead', ''), null),
        S.Status.NOT_INSTALLED, 'socket not-found → not installed');
    eq(S.serviceStatus(unit('masked', 'inactive', 'dead', 'masked'), svcDead),
        S.Status.MASKED, 'masked socket');
    eq(S.serviceStatus(off, unit('loaded', 'inactive', 'dead', 'masked-runtime')),
        S.Status.MASKED, 'masked-runtime service');
    eq(S.serviceStatus(off, svcDead), S.Status.OFF, 'disabled + inactive → off');
    eq(S.serviceStatus(enabledIdle, svcDead), S.Status.STOPPED, 'enabled, socket inactive → stopped');
    eq(S.serviceStatus(listening, svcDead), S.Status.LISTENING, 'socket listening, service idle');
    eq(S.serviceStatus(listening, svcStart), S.Status.STARTING, 'service activating');
    eq(S.serviceStatus(listening, svcRun), S.Status.RUNNING, 'service active');
    eq(S.serviceStatus(listening, svcFail), S.Status.FAILED, 'service failed');
    eq(S.serviceStatus(listening, svcLoop), S.Status.FAILED, 'auto-restart crash loop → failed (q9ba)');
    eq(S.serviceStatus(unit('loaded', 'failed', 'failed', 'enabled'), svcDead),
        S.Status.FAILED, 'socket failed');
    eq(S.serviceStatus(off, svcRun), S.Status.RUNNING,
        'started by hand while disabled → running');

    for (const [ufs, want] of [['enabled', true], ['enabled-runtime', true], ['linked', true],
        ['linked-runtime', true], ['disabled', false], ['static', false], ['indirect', false],
        ['masked', false], [null, false]])
        eq(S.unitFileEnabled({unitFileState: ufs}), want, `unitFileEnabled(${ufs})`);
}

// ── plans: identical to nss_relay_plan ────────────────────────────────────
eq(S.relayPlan(true).map(o => `${o.method}(${o.unit ?? ''})`),
    ['EnableUnitFiles(nostr-session-relay.socket)', 'Reload()',
        'StartUnit(nostr-session-relay.socket)'], 'on plan: enable → reload → start socket');
eq(S.relayPlan(false).map(o => `${o.method}(${o.unit ?? ''})`),
    ['DisableUnitFiles(nostr-session-relay.socket)', 'Reload()',
        'StopUnit(nostr-session-relay.service)', 'StopUnit(nostr-session-relay.socket)'],
    'off plan: disable first, then stop service, then socket');

// ── refresh clamp ─────────────────────────────────────────────────────────
eq(S.clampRefresh(5), 30, 'refresh never below 30 s');
eq(S.clampRefresh(60), 60, 'refresh 60 s kept');
eq(S.clampRefresh(99999), 3600, 'refresh capped at 1 h');
eq(S.clampRefresh(NaN), 30, 'refresh NaN → 30 s');

// ── formatting ────────────────────────────────────────────────────────────
eq(S.formatBytes(0), '0 bytes', 'bytes 0');
eq(S.formatBytes(1), '1 byte', 'bytes 1');
eq(S.formatBytes(999), '999 bytes', 'bytes 999');
eq(S.formatBytes(1000), '1.0 kB', 'bytes 1000');
eq(S.formatBytes(12345678), '12.3 MB', 'bytes 12.3 MB');
eq(S.formatBytes(999950), '1.0 MB', 'bytes 999.95 kB rounds to 1.0 MB');
eq(S.formatBytes(3n * 10n ** 9n), '3.0 GB', 'bytes BigInt');
eq(S.formatBytes(-1), 'unknown size', 'bytes negative');
eq(S.formatDuration(45), '45 s', 'duration s');
eq(S.formatDuration(125), '2 min', 'duration min');
eq(S.formatDuration(3 * 3600 + 12 * 60 + 5), '3 h 12 min', 'duration h min');
eq(S.formatDuration(2 * 86400 + 3600), '2 d 1 h', 'duration d h');
eq(S.formatSats(0, 'en-US'), '0 sats', 'sats 0');
eq(S.formatSats(999, 'en-US'), '0 sats', 'sub-sat dropped, never rounded up');
eq(S.formatSats(1000, 'en-US'), '1 sat', 'sats singular');
eq(S.formatSats(12345678, 'en-US'), '12,345 sats', 'sats grouping');
eq(S.formatSats(21000000n * 100000000n * 1000n, 'en-US'), '2,100,000,000,000,000 sats',
    '21M BTC in msat (BigInt, exact)');
eq(S.formatSats(18446744073709551615n, 'en-US'), '18446744073709551 sats',
    'uint64 max msat: exact digits beyond 2^53');
eq(S.formatSats('garbage'), '—', 'sats bad input');
eq(S.formatSats(-5), '—', 'sats negative');

// ── relay view: the toggle state table ────────────────────────────────────
{
    const sock = (a, s, u) => unit('loaded', a, s, u);
    const svc = (a, s) => unit('loaded', a, s, 'static');
    const stats = S.normalizeStats({
        event_count: 1234, storage_bytes: 12345678, connected_clients: 3,
        uptime: 7300, storage_backend: 'nostrdb',
    });
    const rows = [
        ['systemd unreachable', {available: false}, [false, false, 'Status unavailable']],
        ['not installed', {available: true, socket: unit('not-found', 'inactive', 'dead', ''),
            service: unit('not-found', 'inactive', 'dead', '')}, [false, false, 'Not installed']],
        ['masked', {available: true, socket: unit('masked', 'inactive', 'dead', 'masked'),
            service: svc('inactive', 'dead')}, [false, false, 'Disabled by administrator']],
        ['off', {available: true, socket: sock('inactive', 'dead', 'disabled'),
            service: svc('inactive', 'dead')}, [false, true, 'Off']],
        ['enabled, not listening', {available: true, socket: sock('inactive', 'dead', 'enabled'),
            service: svc('inactive', 'dead')}, [true, true, 'Enabled, not listening']],
        ['listening', {available: true, socket: sock('active', 'listening', 'enabled'),
            service: svc('inactive', 'dead')}, [true, true, 'Listening']],
        ['starting', {available: true, socket: sock('active', 'running', 'enabled'),
            service: svc('activating', 'start')}, [true, true, 'Starting…']],
        ['running, no stats yet', {available: true, socket: sock('active', 'running', 'enabled'),
            service: svc('active', 'running')}, [true, true, 'Running']],
        ['running + stats', {available: true, socket: sock('active', 'running', 'enabled'),
            service: svc('active', 'running'), stats}, [true, true, '1,234 events · 12.3 MB']],
        ['running, cache-less', {available: true, socket: sock('active', 'running', 'enabled'),
            service: svc('active', 'running'), stats: {...stats, storageBackend: 'none'}},
        [true, true, 'No storage']],
        ['running, count unknown', {available: true, socket: sock('active', 'running', 'enabled'),
            service: svc('active', 'running'), stats: {...stats, eventCount: -1}},
        [true, true, '12.3 MB on disk']],
        ['running, started by hand (disabled)', {available: true,
            socket: sock('active', 'running', 'disabled'), service: svc('active', 'running')},
        [false, true, 'Running']],
        ['failed', {available: true, socket: sock('active', 'listening', 'enabled'),
            service: svc('failed', 'failed')}, [true, true, 'Failed']],
        ['crash loop', {available: true, socket: sock('active', 'listening', 'enabled'),
            service: svc('activating', 'auto-restart')}, [true, true, 'Failed']],
        ['turning off', {available: true, socket: sock('active', 'listening', 'enabled'),
            service: svc('inactive', 'dead'), busy: 'off'}, [true, false, 'Turning off…']],
        ['turning on', {available: true, socket: sock('inactive', 'dead', 'disabled'),
            service: svc('inactive', 'dead'), busy: 'on'}, [false, false, 'Turning on…']],
        ['plan failed', {available: true, socket: sock('inactive', 'dead', 'disabled'),
            service: svc('inactive', 'dead'), error: 'Access denied'}, [false, true, 'Error']],
    ];
    print_('# toggle state table: case | checked | reactive | subtitle | menu status line | panel icon');
    for (const [name, input, [checked, reactive, subtitle]] of rows) {
        const v = S.relayView({socket: null, service: null, stats: null, busy: null,
            error: null, ...input}, 'en-US');
        print_(`# | ${name} | ${v.checked} | ${v.reactive} | ${v.subtitle} | ${v.statusLine}` +
            `${v.detailLine ? ` / ${v.detailLine}` : ''} | ${v.showIndicator ? 'shown' : 'hidden'}`);
        eq([v.checked, v.reactive, v.subtitle], [checked, reactive, subtitle], `relayView: ${name}`);
    }
    const run = S.relayView({available: true, socket: sock('active', 'running', 'enabled'),
        service: svc('active', 'running'), stats, busy: null, error: null}, 'en-US');
    eq(run.detailLine, '3 clients · up 2 h 1 min', 'running detail line');
    eq(run.showIndicator, true, 'panel icon while running');
    eq(S.relayView({available: true, socket: sock('inactive', 'dead', 'disabled'),
        service: svc('inactive', 'dead'), error: 'Access denied'}).statusLine,
    'Could not switch the relay: Access denied', 'error surfaced in menu header');
    eq(S.relayView({available: true, socket: sock('active', 'listening', 'enabled'),
        service: svc('inactive', 'dead'), stats}).subtitle, 'Listening',
    'stats ignored unless the service runs');
}

eq(S.normalizeStats({event_count: 'x', storage_bytes: 5n}),
    {eventCount: null, storageBytes: 5, connectedClients: null, uptime: null, storageBackend: null},
    'normalizeStats tolerates mistyped / missing keys');
eq(S.normalizeStats(null), null, 'normalizeStats(null)');

// ── DM row: honest degrade (no unread count exists) ───────────────────────
eq(S.dmView({available: false, notify: null}), null, 'dm hidden without systemd');
eq(S.dmView({available: true, notify: unit('not-found', 'inactive', 'dead', '')}), null,
    'dm hidden when nostr-notify is not installed');
eq(S.dmView({available: true, notify: unit('loaded', 'active', 'running', 'enabled')}),
    'Message notifications: on', 'dm on');
eq(S.dmView({available: true, notify: unit('loaded', 'inactive', 'dead', 'disabled')}),
    'Message notifications: off', 'dm off');
eq(S.dmView({available: true, notify: unit('loaded', 'failed', 'failed', 'enabled')}),
    'Message notifications: failed', 'dm failed');

// ── unread direct messages (org.nostr.NotifyDaemon1) ─────────────────────
{
    const on = {available: true, notify: unit('loaded', 'active', 'running', 'enabled')};
    eq(S.dmView(on, 3), '3 unread direct messages', 'dm count');
    eq(S.dmView(on, 1), '1 unread direct message', 'dm count singular');
    eq(S.dmView(on, 0), 'No unread direct messages', 'dm none unread');
    eq(S.dmView(on, null), 'Message notifications: on', 'daemon not on the bus → service state');
    eq(S.dmView({available: true, notify: unit('loaded', 'failed', 'failed', 'enabled')}, 5),
        'Message notifications: failed', 'failed service wins over a stale count');
}

// ── wallet row: degrade paths, never a prompt without a click ─────────────
{
    const base = {showBalance: true, agentRunning: true, paired: true, readAllowed: true,
        balanceMsat: null, error: null, granting: false};
    const w = o => S.walletView({...base, ...o}, 'en-US');
    eq(w({showBalance: false}), {state: 'hidden', label: null, canGrant: false}, 'wallet hidden by pref');
    eq(w({agentRunning: false}).label, 'Wallet: agent not running', 'agent not running (not auto-started)');
    eq(w({paired: false}).label, 'Wallet: not paired', 'unpaired');
    eq(w({paired: null}).state, 'unpaired', 'Paired unknown treated as unpaired');
    eq(w({readAllowed: false}), {state: 'needs-grant', label: 'Wallet: —', canGrant: true},
        'no grant → "—" plus explicit grant action');
    eq(w({readAllowed: false, granting: true}),
        {state: 'needs-grant', label: 'Wallet: waiting for approval…', canGrant: false},
        'grant pending');
    eq(w({readAllowed: false, balanceMsat: 5000}).label, 'Wallet: 5 sats',
        'one-time approved balance shown');
    eq(w({}).state, 'loading', 'granted, fetching');
    eq(w({balanceMsat: 21000000}).label, 'Wallet: 21,000 sats', 'balance');
    eq(w({error: 'Timeout'}).label, 'Wallet: —', 'fetch error → "—"');
}
eq(S.classifyWalletError('org.nostr.Wallet1.Error.Denied'), 'denied', 'Denied');
eq(S.classifyWalletError('org.nostr.Wallet1.Error.NotPaired'), 'unpaired', 'NotPaired');
eq(S.classifyWalletError('org.freedesktop.DBus.Error.ServiceUnknown'), 'not-running', 'ServiceUnknown');
eq(S.classifyWalletError('org.nostr.Wallet1.Error.Timeout'), 'error', 'Timeout');
eq(S.classifyWalletError('org.nostr.Wallet1.Error.InteractionRequired'), 'needs-grant', 'InteractionRequired');
eq(S.classifyWalletError('org.freedesktop.DBus.Error.UnknownMethod'), 'needs-grant', 'old agent never prompts');

// ── translator hook ───────────────────────────────────────────────────────
S.setTranslator(s => `«${s}»`);
eq(S.relayView({available: false}).subtitle, '«Status unavailable»', 'strings go through gettext');
S.setTranslator(null);
eq(S.relayView({available: false}).subtitle, 'Status unavailable', 'translator reset');

print_(`1..${count}`);
if (failures) {
    print_(`# ${failures} of ${count} failed`);
    if (typeof globalThis.process !== 'undefined')
        globalThis.process.exit(1);
    else
        (await import('system')).default.exit(1);
}
