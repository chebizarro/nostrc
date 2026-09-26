/* policy.test.js — extension-side policy + page.js API shape (nostrc-jjyp).
 * Run: node tests/policy.test.js  (also registered with CTest when node
 * is available). No browser needed. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const P = require('../src/lib/policy.js');

const tests = [];
function test(name, fn) { tests.push([name, fn]); }

const PK = 'f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9';

test('secureOrigin accepts https and loopback http', () => {
  assert.equal(P.secureOrigin('https://snort.social'), 'https://snort.social');
  assert.equal(P.secureOrigin('https://Snort.Social:443'), 'https://snort.social');
  assert.equal(P.secureOrigin('https://a.example:8443'), 'https://a.example:8443');
  assert.equal(P.secureOrigin('http://localhost:5173'), 'http://localhost:5173');
  assert.equal(P.secureOrigin('http://app.localhost'), 'http://app.localhost');
  assert.equal(P.secureOrigin('http://127.0.0.1:8080'), 'http://127.0.0.1:8080');
  assert.equal(P.secureOrigin('http://[::1]:3000'), 'http://[::1]:3000');
});

test('secureOrigin refuses insecure / opaque / non-origin values', () => {
  for (const o of ['http://snort.social', 'null', '', null, undefined, 42, 'file:///x', 'data:text/html,x',
    'moz-extension://abc', 'chrome-extension://abc', 'about:blank', 'https://u:p@a.example',
    'https://a.example/path', 'https://a.example/?q', 'https://a.example/#f', 'ws://localhost',
    'http://localhost.evil.com', 'javascript:alert(1)'])
    assert.equal(P.secureOrigin(o), null, String(o));
});

test('originFromSender uses the browser-populated sender only', () => {
  assert.equal(P.originFromSender({ origin: 'https://snort.social', url: 'https://evil.example/' }), 'https://snort.social');
  assert.equal(P.originFromSender({ url: 'https://iris.to/notes?x=1#y' }), 'https://iris.to');
  assert.equal(P.originFromSender({ origin: 'null', url: 'about:blank' }), null);
  assert.equal(P.originFromSender({ url: 'http://example.com/' }), null);
  assert.equal(P.originFromSender({}), null);
  assert.equal(P.originFromSender(null), null);
});

test('validateRequest: signEvent canonicalizes and rejects malformed events', () => {
  const ok = P.validateRequest('signEvent', { event: { kind: 1, created_at: 5, tags: [['e', 'x']], content: 'hi', id: 'z', sig: 'z', extra: 1 } });
  assert.deepEqual(ok.params, { event: { kind: 1, created_at: 5, tags: [['e', 'x']], content: 'hi' } });
  const now = P.validateRequest('signEvent', { event: { kind: 0, tags: [], content: '' } });
  assert.ok(Number.isInteger(now.params.event.created_at));
  const bad = [
    {}, { event: null }, { event: [] },
    { event: { kind: '1', tags: [], content: '' } },
    { event: { kind: 1.5, tags: [], content: '' } },
    { event: { kind: 70000, tags: [], content: '' } },
    { event: { kind: 1, tags: {}, content: '' } },
    { event: { kind: 1, tags: ['e'], content: '' } },
    { event: { kind: 1, tags: [['e', 1]], content: '' } },
    { event: { kind: 1, tags: [], content: 5 } },
    { event: { kind: 1, tags: [], content: 'a\u0000b' } },
    { event: { kind: 1, tags: [], content: '', created_at: -1 } },
    { event: { kind: 1, tags: [], content: '', created_at: '1' } },
    { event: { kind: 1, tags: [], content: '', pubkey: 'abc' } },
  ];
  for (const p of bad) {
    const v = P.validateRequest('signEvent', p);
    assert.equal(v.code, 'invalid_request', JSON.stringify(p));
  }
});

test('validateRequest: encrypt/decrypt params', () => {
  assert.deepEqual(P.validateRequest('nip44.encrypt', { pubkey: PK.toUpperCase(), plaintext: 'x' }).params,
    { pubkey: PK, plaintext: 'x' });
  assert.deepEqual(P.validateRequest('nip04.decrypt', { pubkey: PK, ciphertext: 'c' }).params,
    { pubkey: PK, ciphertext: 'c' });
  assert.equal(P.validateRequest('nip44.decrypt', { pubkey: PK }).code, 'invalid_request');
  assert.equal(P.validateRequest('nip04.encrypt', { pubkey: 'npub1x', plaintext: 'x' }).code, 'invalid_request');
  assert.equal(P.validateRequest('nip44.encrypt', { pubkey: PK, plaintext: 'a\u0000' }).code, 'invalid_request');
});

test('validateRequest: unknown methods and host-internal names are refused', () => {
  for (const m of ['host.hello', 'webln.sendPayment', 'StoreKey', '__proto__', 'constructor', 'toString', 42])
    assert.equal(P.validateRequest(m, {}).code, 'unknown_method', String(m));
});

test('grants expire and decrypt is top-frame only', () => {
  const now = 1000;
  const grants = { 'https://a.example': { read: now + 1, decrypt: now - 1 } };
  assert.equal(P.grantValid(grants, 'https://a.example', 'read', now), true);
  assert.equal(P.grantValid(grants, 'https://a.example', 'decrypt', now), false, 'expired');
  assert.equal(P.grantValid(grants, 'https://a.example', 'encrypt', now), false);
  assert.equal(P.grantValid(grants, 'https://b.example', 'read', now), false);
  assert.equal(P.grantValid({ 'https://a.example': { read: true } }, 'https://a.example', 'read', now), false,
    'non-numeric (legacy/forged) grants are ignored');
  assert.equal(P.grantValid(grants, '__proto__', 'read', now), false);
  assert.ok(P.GRANT_TTL_MS.decrypt < P.GRANT_TTL_MS.read);
  assert.equal(P.allowedInSubframe('decrypt'), false);
  assert.equal(P.allowedInSubframe('read'), true);
  assert.equal(P.allowedInSubframe(null), true); /* signEvent: signer dialog names the frame origin */
});

test('permission gates and timeouts', () => {
  assert.equal(P.gateFor('getPublicKey'), 'read');
  assert.equal(P.gateFor('getRelays'), 'read');
  assert.equal(P.gateFor('nip04.encrypt'), 'encrypt');
  assert.equal(P.gateFor('nip44.decrypt'), 'decrypt');
  assert.equal(P.gateFor('signEvent'), null); /* signer's own approval dialog */
  assert.equal(P.timeoutFor('signEvent'), P.TIMEOUT_INTERACTIVE_MS);
  assert.equal(P.timeoutFor('getPublicKey'), P.TIMEOUT_DEFAULT_MS);
  assert.ok(P.TIMEOUT_INTERACTIVE_MS > 120000, 'extension timeout must exceed the host approval timeout');
});

test('page.js defines the NIP-07 surface and posts nonce-tagged requests', async () => {
  const posted = [];
  const listeners = [];
  const win = {
    crypto: { randomUUID: () => 'nonce-' + posted.length },
    addEventListener: (t, fn) => { if (t === 'message') listeners.push(fn); },
    postMessage: (data) => posted.push(data),
  };
  const ctx = vm.createContext({ window: win, setTimeout, clearTimeout, Promise, Object, Error, Array, Uint8Array, String });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../src/page.js'), 'utf8'), ctx);
  const nostr = win.nostr;
  assert.equal(typeof nostr.getPublicKey, 'function');
  assert.equal(typeof nostr.signEvent, 'function');
  assert.equal(typeof nostr.getRelays, 'function');
  assert.equal(typeof nostr.nip04.encrypt, 'function');
  assert.equal(typeof nostr.nip44.decrypt, 'function');
  assert.ok(Object.isFrozen(nostr));
  assert.equal(Object.getOwnPropertyDescriptor(win, 'nostr').configurable, false);

  const p = nostr.signEvent({ kind: 1, tags: [], content: 'x' });
  assert.equal(posted.length, 1);
  const req = posted[0];
  assert.equal(req.dir, 'to-ext');
  assert.equal(req.method, 'signEvent');
  assert.equal(req.origin, undefined, 'page must not send an origin');

  /* a reply from another window, or with the wrong id, is ignored */
  listeners[0]({ source: {}, data: { channel: req.channel, dir: 'to-page', id: req.id, result: 'spoof' } });
  listeners[0]({ source: win, data: { channel: req.channel, dir: 'to-page', id: 'other', result: 'spoof' } });
  listeners[0]({ source: win, data: { channel: req.channel, dir: 'to-page', id: req.id, error: { code: 'rejected', message: 'no' } } });
  await assert.rejects(p, (e) => e.code === 'rejected');
});

(async () => {
  let failures = 0;
  for (const [name, fn] of tests) {
    try { await fn(); console.log('ok -', name); }
    catch (e) { failures++; console.log('not ok -', name, '\n', e); }
  }
  if (failures) { console.log(`${failures} test(s) failed`); process.exit(1); }
  console.log(`all ${tests.length} extension policy tests passed`);
})();
