/* webln.test.js — WebLN in the extension (nostrc-jjyp): sats conversion and
 * range checks, request validation and gating, the window.webln spec shape,
 * error mapping to Error.code, and conditional injection (not injected
 * unless the host reports a paired wallet).
 * Run: node tests/webln.test.js (also registered with CTest). No browser. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const P = require('../src/lib/policy.js');
const W = require('../src/providers/webln.js');

const tests = [];
function test(name, fn) { tests.push([name, fn]); }

const CHANNEL = 'nostr-signer-bridge/v1';
const COFFEE = 'lnbc2500u1pvjluezsp5zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygspp5qqqsyqcyq5rqwzqf' +
  'qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqdq5xysxxatsyp3k7enxv4jsxqzpu9qrsgquk0rl77nj30yxdy8j9vdx' +
  '85fkpmdla2087ne0xh8nhedh8w27kyke0lp53ut353s06fv3qfegext0eh0ymjpf39tuven09sam30g4vgpfna3rh';

test('sats: integers and digit strings, range-checked against the uint32-msat limit', () => {
  assert.deepEqual(P.parseSats(21), { sats: 21 });
  assert.deepEqual(P.parseSats('21'), { sats: 21 });
  assert.deepEqual(P.parseSats('000042'), { sats: 42 });
  assert.deepEqual(P.parseSats(4294967), { sats: 4294967 });
  assert.equal(P.parseSats(4294968).code, 'too_large');
  assert.equal(P.parseSats('99999999999999999999999').code, 'too_large');
  assert.equal(P.parseSats(1e21).code, 'too_large');
  for (const bad of [-1, 1.5, NaN, Infinity, '1.5', '-3', '+3', '1e3', '', ' 21', '0x10', true, [21], {}])
    assert.equal(P.parseSats(bad).code, 'invalid_request', String(bad));
  assert.deepEqual(P.parseSats(undefined), { absent: true });
  assert.deepEqual(P.parseSats(null), { absent: true });
  assert.equal(P.satsToMsat(21), 21000);
  assert.equal(P.satsToMsat(P.MAX_SATS), 4294967000);
  assert.ok(P.satsToMsat(P.MAX_SATS) <= 0xffffffff);
  assert.equal(P.satsToMsat(P.MAX_SATS + 1), null);
  assert.equal(P.satsToMsat(1.5), null);
});

test('makeInvoice args resolve like the host (amount / defaultAmount / minimumAmount, min..max)', () => {
  assert.deepEqual(P.resolveInvoice(21), { params: { amount: 21 } });
  assert.deepEqual(P.resolveInvoice('21'), { params: { amount: 21 } });
  assert.deepEqual(P.resolveInvoice({ amount: 21, defaultMemo: 'coffee' }), { params: { amount: 21, defaultMemo: 'coffee' } });
  assert.deepEqual(P.resolveInvoice({ defaultAmount: 50, minimumAmount: 10, maximumAmount: 100 }), { params: { amount: 50 } });
  assert.deepEqual(P.resolveInvoice({ minimumAmount: 10, maximumAmount: 100 }), { params: { amount: 10 } });
  assert.deepEqual(P.resolveInvoice({ amount: 1, defaultMemo: null }), { params: { amount: 1 } });
  for (const bad of [{}, { amount: 0 }, { maximumAmount: 100 }, { amount: 5, minimumAmount: 10 },
    { amount: 500, maximumAmount: 100 }, { minimumAmount: 100, maximumAmount: 10 }, { amount: 5, defaultMemo: 7 },
    { amount: 5, defaultMemo: 'a\u0000b' }, { amount: 'ten' }, null, [1]])
    assert.equal(P.resolveInvoice(bad).code, 'invalid_request', JSON.stringify(bad));
  assert.equal(P.resolveInvoice({ amount: 4294968 }).code, 'too_large');
  assert.equal(P.resolveInvoice({ defaultAmount: '100000000000' }).code, 'too_large');
  assert.ok(P.resolveInvoice({ amount: 1, defaultMemo: 'm'.repeat(639) }).params);
  assert.equal(P.resolveInvoice({ amount: 1, defaultMemo: 'm'.repeat(640) }).code, 'invalid_request');
  assert.equal(P.resolveInvoice({ amount: 1, defaultMemo: 'é'.repeat(320) }).code, 'invalid_request', 'memo limit is bytes');
});

test('webln.* request validation, gates and timeouts', () => {
  assert.deepEqual(P.validateRequest('webln.enable', { junk: 1 }), { params: {} });
  assert.deepEqual(P.validateRequest('webln.getInfo', {}), { params: {} });
  assert.deepEqual(P.validateRequest('webln.getBalance'), { params: {} });
  assert.deepEqual(P.validateRequest('webln.sendPayment', { paymentRequest: COFFEE, extra: 1 }),
    { params: { paymentRequest: COFFEE } });
  assert.equal(P.validateRequest('webln.sendPayment', {}).code, 'invalid_request');
  assert.equal(P.validateRequest('webln.sendPayment', { paymentRequest: 'lnurl1dp68gurn8ghj7um9wfmxjcm99e3k7mf0' }).code, 'invalid_request');
  assert.deepEqual(P.validateRequest('webln.makeInvoice', { amount: '21', defaultMemo: 'x', junk: 1 }),
    { params: { amount: 21, defaultMemo: 'x' } });
  for (const m of ['webln.keysend', 'webln.signMessage', 'webln.verifyMessage', 'webln.lnurl', 'webln.status'])
    assert.equal(P.validateRequest(m, {}).code, 'unknown_method', m + ' must never be forwarded from a page');
  for (const m of ['webln.enable', 'webln.getInfo', 'webln.getBalance', 'webln.makeInvoice', 'webln.sendPayment']) {
    assert.equal(P.gateFor(m), 'webln');
    assert.ok(P.isWebLN(m));
  }
  assert.ok(!P.isWebLN('getPublicKey'));
  assert.ok(P.allowedInSubframe('webln'));
  assert.equal(P.timeoutFor('webln.sendPayment'), P.TIMEOUT_WALLET_MS);
  assert.equal(P.timeoutFor('webln.enable'), P.TIMEOUT_DEFAULT_MS);
  assert.ok(P.TIMEOUT_WALLET_MS > 190000, 'extension timeout must exceed the host wallet timeout');
  assert.ok(P.SESSION_TTL_MS.webln < P.GRANT_TTL_MS.webln);
  assert.ok(P.grantValid({ 'https://a.example': { webln: 2000 } }, 'https://a.example', 'webln', 1000));
  assert.ok(!P.grantValid({ 'https://a.example': { read: 2000 } }, 'https://a.example', 'webln', 1000));
});

test('BOLT-11 shape check', () => {
  assert.ok(P.isBolt11(COFFEE));
  assert.ok(P.isBolt11(COFFEE.toUpperCase()));
  const mixed = COFFEE.slice(0, 10) + COFFEE[10].toUpperCase() + COFFEE.slice(11);
  for (const bad of [undefined, 42, '', 'lnbc', 'lightning:' + COFFEE, 'lnurl1dp68gurn8ghj7um9', mixed,
    'lnbc2500u1pvjlue zsp5zyg3zyg3zyg3zyg3zyg3', 'user@example.com', 'lnbc1' + 'q'.repeat(7100)])
    assert.ok(!P.isBolt11(bad), String(bad).slice(0, 40));
});

test('inject decision: only an available agent with a paired wallet', () => {
  assert.ok(P.shouldInjectWebLN({ available: true, paired: true }));
  for (const s of [null, undefined, {}, { available: true, paired: false }, { available: false, paired: true },
    { available: 'yes', paired: true }, { available: false, paired: false, reason: 'wallet_unavailable' }])
    assert.ok(!P.shouldInjectWebLN(s), JSON.stringify(s));
});

test('window.webln object: spec shape, sats args, unsupported methods, error codes', async () => {
  const calls = [];
  let reply = () => Promise.resolve({});
  let enabledEvents = 0;
  const webln = W.makeWebLN((method, params) => { calls.push([method, params]); return reply(method, params); },
    () => enabledEvents++);
  for (const m of ['enable', 'isEnabled', 'getInfo', 'getBalance', 'makeInvoice', 'sendPayment',
    'keysend', 'signMessage', 'verifyMessage', 'lnurl'])
    assert.equal(typeof webln[m], 'function', m);
  assert.ok(Object.isFrozen(webln));
  assert.equal(webln.enabled, false);
  assert.equal(await webln.isEnabled(), false);

  assert.equal(await webln.enable(), undefined);
  assert.deepEqual(calls.pop(), ['webln.enable', {}]);
  assert.equal(webln.enabled, true);
  assert.equal(enabledEvents, 1);

  reply = () => Promise.resolve({ paymentRequest: 'lnbc1...', rHash: 'ab' });
  assert.deepEqual(await webln.makeInvoice(21), { paymentRequest: 'lnbc1...', rHash: 'ab' });
  assert.deepEqual(calls.pop(), ['webln.makeInvoice', { amount: 21 }]);
  await webln.makeInvoice('1000');
  assert.deepEqual(calls.pop(), ['webln.makeInvoice', { amount: '1000' }]);
  await webln.makeInvoice({ defaultAmount: 5, minimumAmount: 1, maximumAmount: 9, defaultMemo: 'm', evil: () => 1 });
  assert.deepEqual(calls.pop(), ['webln.makeInvoice', { defaultAmount: 5, minimumAmount: 1, maximumAmount: 9, defaultMemo: 'm' }]);

  reply = () => Promise.resolve({ preimage: '00'.repeat(32) });
  assert.deepEqual(await webln.sendPayment(COFFEE), { preimage: '00'.repeat(32) });
  assert.deepEqual(calls.pop(), ['webln.sendPayment', { paymentRequest: COFFEE }]);

  const before = calls.length;
  for (const m of ['keysend', 'signMessage', 'verifyMessage', 'lnurl'])
    await assert.rejects(webln[m]('x'), (e) => e instanceof Error && e.code === 'unsupported');
  assert.equal(calls.length, before, 'unsupported methods never reach the extension');

  /* host/agent errors keep their code */
  for (const code of ['not_paired', 'budget_exceeded', 'rejected', 'wallet_error', 'too_large', 'not_enabled']) {
    reply = () => Promise.reject(Object.assign(new Error('x'), { code }));
    await assert.rejects(webln.getBalance(), (e) => e.code === code);
  }
});

/* A fake MAIN world for webln.js. */
function mainWorld(extra) {
  const posted = [];
  const listeners = [];
  const events = [];
  let n = 0;
  const win = Object.assign({
    crypto: { randomUUID: () => 'w-' + (n++) },
    addEventListener: (t, fn) => { if (t === 'message') listeners.push(fn); },
    postMessage: (data) => posted.push(data),
    dispatchEvent: (ev) => events.push(ev.type),
  }, extra || {});
  class Event { constructor(type) { this.type = type; } }
  const ctx = vm.createContext({ window: win, Event, setTimeout, clearTimeout, Uint8Array });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../src/providers/webln.js'), 'utf8'), ctx);
  const deliver = (data, source) => listeners.forEach((fn) => fn({ source: source || win, data }));
  return { win, posted, events, deliver };
}

test('window.webln is NOT injected until the extension reports a paired wallet', async () => {
  const w = mainWorld();
  assert.equal(w.win.webln, undefined, 'no availability message: sites keep their no-WebLN fallback');
  /* spoofed / malformed availability messages are ignored */
  w.deliver({ channel: CHANNEL, dir: 'to-page', type: 'webln-available' }, {});
  w.deliver({ channel: 'other', dir: 'to-page', type: 'webln-available' });
  w.deliver({ channel: CHANNEL, dir: 'to-ext', type: 'webln-available' });
  assert.equal(w.win.webln, undefined);
  assert.deepEqual(w.events, []);

  w.deliver({ channel: CHANNEL, dir: 'to-page', type: 'webln-available' });
  assert.equal(typeof w.win.webln.sendPayment, 'function');
  assert.equal(Object.getOwnPropertyDescriptor(w.win, 'webln').configurable, false);
  assert.deepEqual(w.events, ['webln:ready']);
  w.deliver({ channel: CHANNEL, dir: 'to-page', type: 'webln-available' });
  assert.deepEqual(w.events, ['webln:ready'], 'installed once');

  /* requests: nonce id, no origin; replies correlate; other windows ignored */
  const p = w.win.webln.getInfo();
  const req = w.posted[0];
  assert.equal(req.channel, CHANNEL);
  assert.equal(req.dir, 'to-ext');
  assert.equal(req.method, 'webln.getInfo');
  assert.equal(req.origin, undefined, 'page must not send an origin');
  w.deliver({ channel: CHANNEL, dir: 'to-page', id: req.id, result: 'spoof' }, {});
  w.deliver({ channel: CHANNEL, dir: 'to-page', id: 'other', result: 'spoof' });
  w.deliver({ channel: CHANNEL, dir: 'to-page', id: req.id, result: { node: { alias: 'n' } } });
  assert.deepEqual(await p, { node: { alias: 'n' } });

  const q = w.win.webln.sendPayment(COFFEE);
  const req2 = w.posted[1];
  w.deliver({ channel: CHANNEL, dir: 'to-page', id: req2.id, error: { code: 'budget_exceeded', message: 'over' } });
  await assert.rejects(q, (e) => e.code === 'budget_exceeded' && e.message === 'over');
});

test('an existing window.webln (another provider) is left alone', () => {
  const other = { mine: true };
  const w = mainWorld({ webln: other });
  w.deliver({ channel: CHANNEL, dir: 'to-page', type: 'webln-available' });
  assert.equal(w.win.webln, other);
  assert.deepEqual(w.events, []);
});

/* content.js with a fake extension runtime. */
async function runContent(statusReply) {
  const posted = [];
  const sent = [];
  const listeners = [];
  const win = {
    addEventListener: (t, fn) => { if (t === 'message') listeners.push(fn); },
    postMessage: (data) => posted.push(data),
  };
  const browser = { runtime: { sendMessage: (msg) => {
    sent.push(msg);
    if (msg.type === 'webln:status') return Promise.resolve(statusReply);
    return Promise.resolve({ result: 'ok' });
  } } };
  const ctx = vm.createContext({ window: win, browser, Promise, JSON });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../src/content.js'), 'utf8'), ctx);
  await new Promise((r) => setImmediate(r));
  return { posted, sent, listeners, win };
}

test('content.js asks for WebLN status and signals webln.js only when told to inject', async () => {
  let c = await runContent({ result: { inject: false } });
  assert.deepEqual(Array.from(c.sent, (m) => m.type), ['webln:status']);
  assert.equal(c.posted.length, 0, 'unpaired: nothing posted, window.webln stays undefined');
  c = await runContent({ error: { code: 'signer_unavailable', message: 'no host' } });
  assert.equal(c.posted.length, 0);
  c = await runContent(undefined);
  assert.equal(c.posted.length, 0);
  c = await runContent({ result: { inject: true } });
  assert.deepEqual(JSON.parse(JSON.stringify(c.posted)), [{ channel: CHANNEL, dir: 'to-page', type: 'webln-available' }]);

  /* page requests (NIP-07 or WebLN) are forwarded as type "page" */
  c.listeners[0]({ source: c.win, data: { channel: CHANNEL, dir: 'to-ext', id: 'x1', method: 'webln.getInfo', params: {} } });
  await new Promise((r) => setImmediate(r));
  assert.deepEqual(JSON.parse(JSON.stringify(c.sent[1])), { type: 'page', id: 'x1', method: 'webln.getInfo', params: {} });
});

(async () => {
  let failures = 0;
  for (const [name, fn] of tests) {
    try { await fn(); console.log('ok -', name); }
    catch (e) { failures++; console.log('not ok -', name, '\n', e); }
  }
  if (failures) { console.log(`${failures} test(s) failed`); process.exit(1); }
  console.log(`all ${tests.length} WebLN extension tests passed`);
  process.exit(0); /* unanswered page-side calls keep 200 s backstop timers */
})();
