/* webln.js — window.webln provider: STUB, not loaded by any manifest.
 * (nostrc-jjyp, TODO nostrc-yka8)
 *
 * WebLN (enable / getInfo / sendPayment / makeInvoice) will be served by
 * the same native host once nostr-wallet-agent exposes org.nostr.Wallet1
 * (gnome/nostr-wallet-agent/, bead nostrc-yka8). The host already routes
 * "webln.*" methods to apps/gnostr-signer/native-host/nm_provider_webln.c,
 * which answers "unsupported" today.
 *
 * To enable it:
 *   1. implement nm_provider_webln.c against org.nostr.Wallet1 (mapping is
 *      in that file's header comment);
 *   2. add 'webln.enable': 'webln', 'webln.getInfo': 'webln',
 *      'webln.sendPayment': null (the wallet agent owns spend approval),
 *      'webln.makeInvoice': 'webln' to METHODS in src/lib/policy.js plus
 *      param validation in validateRequest;
 *   3. merge the object below into page.js (defining window.webln) — it
 *      reuses page.js's call(); it is deliberately NOT injected now, because
 *      sites prefer WebLN when window.webln exists and a non-working one
 *      would break their fallback to other payment flows.
 */
/* eslint-disable no-unused-vars */
function makeWebLN(call) {
  let enabled = false;
  return Object.freeze({
    async enable() { await call('webln.enable', {}); enabled = true; },
    get enabled() { return enabled; },
    getInfo: () => call('webln.getInfo', {}),
    sendPayment: (paymentRequest) => call('webln.sendPayment', { paymentRequest }),
    makeInvoice: (args) => call('webln.makeInvoice', typeof args === 'object' ? args : { amount: args }),
  });
}
