/* policy.js — extension-side request policy for the NIP-07 bridge
 * (nostrc-jjyp). Loaded by the background (Firefox: background.scripts;
 * Chromium: importScripts) and by tests/policy.test.js under Node.
 *
 * The native host re-validates everything below; this copy exists so bad
 * input is refused before a native process is spawned and so the
 * permission gate can be decided per method.
 */
(function (root) {
  'use strict';

  const NATIVE_HOST = 'org.nostr.signer_bridge';
  const MAX_ID_LEN = 64;

  /* Which extension-side permission a method needs. signEvent is gated by
   * the desktop signer's own approval dialog (keyed on the origin), so the
   * extension does not prompt for it. The signer does not gate
   * GetPublicKey / GetRelays / NIP04* / NIP44* at all today, so the
   * extension does. */
  const METHODS = Object.freeze({
    getPublicKey: 'read',
    getRelays: 'read',
    signEvent: null,
    'nip04.encrypt': 'encrypt',
    'nip44.encrypt': 'encrypt',
    'nip04.decrypt': 'decrypt',
    'nip44.decrypt': 'decrypt',
  });

  /* Calls that can wait on a human (signer approval) get the long timeout;
   * it is a little longer than the host's own approval timeout so the
   * host's "timeout" error normally wins. */
  const TIMEOUT_INTERACTIVE_MS = 125000;
  const TIMEOUT_DEFAULT_MS = 35000;
  const INTERACTIVE = new Set(['signEvent', 'nip04.decrypt', 'nip44.decrypt']);

  /* How long a "remember for this site" grant lasts. Decryption is the
   * most sensitive capability (it reads every message addressed to you), so
   * its grant is short-lived; nothing is remembered forever. */
  const GRANT_TTL_MS = Object.freeze({
    read: 30 * 24 * 3600 * 1000,
    encrypt: 30 * 24 * 3600 * 1000,
    decrypt: 24 * 3600 * 1000,
  });

  /* Gates a cross-origin iframe may use. Decryption is refused outside the
   * top-level frame so an embedded widget cannot ride on the page the user
   * believes they are talking to. */
  const SUBFRAME_OK = Object.freeze({ read: true, encrypt: true, decrypt: false });

  const HEX64 = /^[0-9a-fA-F]{64}$/;

  function isLoopbackHost(hostname) {
    return hostname === 'localhost' || hostname.endsWith('.localhost') ||
      hostname === '127.0.0.1' || hostname === '[::1]';
  }

  /* A browser-reported origin that may use the signer: https, or http on
   * a loopback host. Returns the canonical origin string or null. */
  function secureOrigin(origin) {
    if (typeof origin !== 'string' || origin === 'null' || origin.length > 512) return null;
    let u;
    try { u = new URL(origin); } catch (_) { return null; }
    if (u.origin === 'null' || u.username || u.password) return null;
    if ((u.pathname !== '/' && u.pathname !== '') || u.search || u.hash) return null;
    if (u.protocol === 'https:') return u.origin;
    if (u.protocol === 'http:' && isLoopbackHost(u.hostname)) return u.origin;
    return null;
  }

  /* The requesting frame's origin, taken from the browser-populated
   * runtime.MessageSender — never from anything the page sent. Chromium
   * provides sender.origin; Firefox provides the frame URL. */
  function originFromSender(sender) {
    if (!sender || typeof sender !== 'object') return null;
    let origin = null;
    if (typeof sender.origin === 'string' && sender.origin !== 'null') {
      origin = sender.origin;
    } else if (typeof sender.url === 'string') {
      try { origin = new URL(sender.url).origin; } catch (_) { origin = null; }
    }
    return origin ? secureOrigin(origin) : null;
  }

  function isString(s) { return typeof s === 'string' && !s.includes('\u0000'); }

  function validateEvent(ev) {
    if (!ev || typeof ev !== 'object' || Array.isArray(ev)) return { error: 'event must be an object' };
    if (!Number.isInteger(ev.kind) || ev.kind < 0 || ev.kind > 65535) return { error: 'kind must be an integer in 0..65535' };
    let createdAt = ev.created_at;
    if (createdAt === undefined || createdAt === null) createdAt = 0;
    if (!Number.isSafeInteger(createdAt) || createdAt < 0) return { error: 'created_at must be a non-negative integer' };
    if (createdAt === 0) createdAt = Math.floor(Date.now() / 1000);
    if (!Array.isArray(ev.tags)) return { error: 'tags must be an array' };
    for (const tag of ev.tags) {
      if (!Array.isArray(tag)) return { error: 'each tag must be an array' };
      for (const item of tag) if (!isString(item)) return { error: 'tag items must be strings' };
    }
    if (!isString(ev.content)) return { error: 'content must be a string' };
    if (ev.pubkey !== undefined && ev.pubkey !== null && !(typeof ev.pubkey === 'string' && HEX64.test(ev.pubkey)))
      return { error: 'pubkey must be 64 hex characters' };
    return {
      event: {
        kind: ev.kind,
        created_at: createdAt,
        tags: ev.tags.map((t) => t.slice()),
        content: ev.content,
      },
    };
  }

  /* Validate a request's method + params. Returns {params} with a
   * sanitized copy, or {code, error}. */
  function validateRequest(method, params) {
    if (typeof method !== 'string' || !Object.prototype.hasOwnProperty.call(METHODS, method))
      return { code: 'unknown_method', error: 'Unknown method' };
    const p = (params && typeof params === 'object' && !Array.isArray(params)) ? params : {};
    switch (method) {
      case 'getPublicKey':
      case 'getRelays':
        return { params: {} };
      case 'signEvent': {
        const v = validateEvent(p.event);
        if (v.error) return { code: 'invalid_request', error: 'Invalid event: ' + v.error };
        return { params: { event: v.event } };
      }
      case 'nip04.encrypt':
      case 'nip44.encrypt':
      case 'nip04.decrypt':
      case 'nip44.decrypt': {
        const field = method.endsWith('encrypt') ? 'plaintext' : 'ciphertext';
        if (typeof p.pubkey !== 'string' || !HEX64.test(p.pubkey))
          return { code: 'invalid_request', error: 'pubkey must be 64 hex characters' };
        if (!isString(p[field])) return { code: 'invalid_request', error: field + ' must be a string' };
        return { params: { pubkey: p.pubkey.toLowerCase(), [field]: p[field] } };
      }
    }
    return { code: 'unknown_method', error: 'Unknown method' };
  }

  function gateFor(method) { return METHODS[method] || null; }
  function allowedInSubframe(gate) { return gate === null || SUBFRAME_OK[gate] === true; }
  function grantValid(grants, origin, gate, now) {
    const g = grants && Object.prototype.hasOwnProperty.call(grants, origin) ? grants[origin] : null;
    return !!g && typeof g[gate] === 'number' && g[gate] > now;
  }
  function timeoutFor(method) { return INTERACTIVE.has(method) ? TIMEOUT_INTERACTIVE_MS : TIMEOUT_DEFAULT_MS; }
  function isValidPageId(id) { return typeof id === 'string' && id.length > 0 && id.length <= MAX_ID_LEN; }

  const api = Object.freeze({
    NATIVE_HOST, METHODS, TIMEOUT_INTERACTIVE_MS, TIMEOUT_DEFAULT_MS, GRANT_TTL_MS,
    secureOrigin, originFromSender, validateEvent, validateRequest, gateFor, timeoutFor, isValidPageId,
    allowedInSubframe, grantValid,
  });
  root.NostrBridgePolicy = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
