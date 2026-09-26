/* webln.js — MAIN-world script: window.webln (WebLN) backed by the desktop
 * wallet agent org.nostr.Wallet1. (nostrc-jjyp)
 *
 * Like page.js it holds no privileges: requests go over the same
 * window.postMessage channel to content.js, carry a random id the reply
 * must echo, and never carry an origin (the background takes it from the
 * browser's MessageSender).
 *
 * Conditional injection: window.webln is only defined after content.js
 * reports that a wallet is paired (background -> host "webln.status",
 * cached briefly). With no wallet, or no wallet agent, it stays undefined
 * so a site's own "no WebLN" fallback keeps working. Once defined, a
 * "webln:ready" event is dispatched on window. An existing window.webln
 * (another provider) is never replaced.
 *
 *   enable()                 per-site consent (extension prompt); resolves
 *   getInfo()                {node: {alias, pubkey?, color?}, methods, supports, version}
 *   getBalance()             {balance (sats), currency: "sats"}
 *   makeInvoice(args)        {paymentRequest, rHash}; args: sats | "sats" | RequestInvoiceArgs
 *   sendPayment(bolt11)      {preimage} — always approved by the desktop wallet
 *                            (its dialog, or this site's daily budget)
 *   keysend / signMessage / verifyMessage / lnurl  reject with code "unsupported"
 *
 * Rejections are Error objects with a string .code (README "Error codes").
 */
(function () {
  'use strict';

  const UNSUPPORTED = ['keysend', 'signMessage', 'verifyMessage', 'lnurl'];

  function codedError(code, message) {
    const e = new Error(String(message || 'Request failed'));
    e.code = String(code || 'internal');
    return e;
  }

  /* Normalize makeInvoice's argument to an object of plain JSON values. */
  function invoiceArgs(args) {
    if (typeof args === 'number' || typeof args === 'string') return { amount: args };
    if (!args || typeof args !== 'object') return {};
    const out = {};
    for (const k of ['amount', 'defaultAmount', 'minimumAmount', 'maximumAmount', 'defaultMemo'])
      if (args[k] !== undefined) out[k] = args[k];
    return out;
  }

  /* The WebLN object over a transport call(method, params) -> Promise. */
  function makeWebLN(call, onEnabled) {
    let enabled = false;
    const unsupported = (name) => () => Promise.reject(
      codedError('unsupported', name + ' is not supported by the desktop wallet'));
    const api = {
      enable: () => call('webln.enable', {}).then(() => {
        enabled = true;
        if (onEnabled) onEnabled();
      }),
      isEnabled: () => Promise.resolve(enabled),
      getInfo: () => call('webln.getInfo', {}),
      getBalance: () => call('webln.getBalance', {}),
      makeInvoice: (args) => call('webln.makeInvoice', invoiceArgs(args)),
      sendPayment: (paymentRequest) => call('webln.sendPayment', { paymentRequest }),
    };
    for (const name of UNSUPPORTED) api[name] = unsupported(name);
    Object.defineProperty(api, 'enabled', { get: () => enabled, enumerable: true });
    return Object.freeze(api);
  }

  /* Node (tests): export the pure parts. A page may define its own global
   * "module", so only when there is no window at all. */
  if (typeof window === 'undefined') {
    if (typeof module !== 'undefined' && module.exports) module.exports = { makeWebLN, invoiceArgs, UNSUPPORTED };
    return;
  }

  /* ---- browser (MAIN world) ---------------------------------------------- */
  const CHANNEL = 'nostr-signer-bridge/v1';
  const PAGE_TIMEOUT_MS = 200000; /* backstop; background (195 s) and host (190 s) time out first */
  const pending = new Map();
  let installed = false;

  function nonce() {
    if (window.crypto && typeof window.crypto.randomUUID === 'function') return window.crypto.randomUUID();
    const b = new Uint8Array(16);
    window.crypto.getRandomValues(b);
    return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
  }

  function call(method, params) {
    return new Promise((resolve, reject) => {
      const id = nonce();
      const timer = setTimeout(() => {
        pending.delete(id);
        reject(codedError('timeout', 'The wallet did not answer in time'));
      }, PAGE_TIMEOUT_MS);
      pending.set(id, { resolve, reject, timer });
      window.postMessage({ channel: CHANNEL, dir: 'to-ext', id, method, params }, '*');
    });
  }

  function install() {
    if (installed) return;
    installed = true;
    if (window.webln) return; /* another WebLN provider got here first */
    const webln = makeWebLN(call, () => window.dispatchEvent(new Event('webln:enabled')));
    Object.defineProperty(window, 'webln', { value: webln, enumerable: true, configurable: false, writable: false });
    window.dispatchEvent(new Event('webln:ready'));
  }

  window.addEventListener('message', (ev) => {
    if (ev.source !== window) return;
    const d = ev.data;
    if (!d || typeof d !== 'object' || d.channel !== CHANNEL || d.dir !== 'to-page') return;
    if (d.type === 'webln-available') { install(); return; }
    const p = pending.get(d.id);
    if (!p) return; /* page.js's replies share the channel; ids never collide */
    pending.delete(d.id);
    clearTimeout(p.timer);
    if (d.error) p.reject(codedError(d.error.code, d.error.message));
    else p.resolve(d.result);
  });
})();
