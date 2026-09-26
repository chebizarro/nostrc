/* page.js — MAIN-world script: defines window.nostr (NIP-07).
 * (nostrc-jjyp)
 *
 * Runs in the page's own JS world, so it holds no privileges: it only
 * posts requests to this window, where content.js (isolated world) picks
 * them up. Each request carries a fresh random id (the nonce) that the
 * reply must echo; replies for unknown ids, from other windows/frames, or
 * without the channel/direction markers are ignored. The page's identity
 * (origin) is NOT part of the message — the background derives it from the
 * browser's MessageSender.
 */
(() => {
  'use strict';
  if (window.nostr) return; /* another NIP-07 provider got here first */

  const CHANNEL = 'nostr-signer-bridge/v1';
  const PAGE_TIMEOUT_MS = 150000; /* backstop; the background times out first */
  const pending = new Map();

  function nonce() {
    if (window.crypto && typeof window.crypto.randomUUID === 'function') return window.crypto.randomUUID();
    const b = new Uint8Array(16);
    window.crypto.getRandomValues(b);
    return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
  }

  window.addEventListener('message', (ev) => {
    if (ev.source !== window) return;
    const d = ev.data;
    if (!d || typeof d !== 'object' || d.channel !== CHANNEL || d.dir !== 'to-page') return;
    const p = pending.get(d.id);
    if (!p) return;
    pending.delete(d.id);
    clearTimeout(p.timer);
    if (d.error) {
      const err = new Error(String(d.error.message || 'Request failed'));
      err.code = String(d.error.code || 'internal');
      p.reject(err);
    } else {
      p.resolve(d.result);
    }
  });

  function call(method, params) {
    return new Promise((resolve, reject) => {
      const id = nonce();
      const timer = setTimeout(() => {
        pending.delete(id);
        const err = new Error('The signer did not answer in time');
        err.code = 'timeout';
        reject(err);
      }, PAGE_TIMEOUT_MS);
      pending.set(id, { resolve, reject, timer });
      window.postMessage({ channel: CHANNEL, dir: 'to-ext', id, method, params }, '*');
    });
  }

  const nostr = Object.freeze({
    getPublicKey: () => call('getPublicKey', {}),
    signEvent: (event) => call('signEvent', { event }),
    getRelays: () => call('getRelays', {}),
    nip04: Object.freeze({
      encrypt: (pubkey, plaintext) => call('nip04.encrypt', { pubkey, plaintext }),
      decrypt: (pubkey, ciphertext) => call('nip04.decrypt', { pubkey, ciphertext }),
    }),
    nip44: Object.freeze({
      encrypt: (pubkey, plaintext) => call('nip44.encrypt', { pubkey, plaintext }),
      decrypt: (pubkey, ciphertext) => call('nip44.decrypt', { pubkey, ciphertext }),
    }),
  });

  Object.defineProperty(window, 'nostr', { value: nostr, enumerable: true, configurable: false, writable: false });
  /* window.webln lives in src/providers/webln.js (injected only when a
   * wallet is paired). */
})();
