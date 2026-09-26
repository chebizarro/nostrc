/* content.js — isolated-world relay between page.js and the background.
 * (nostrc-jjyp)
 *
 * Accepts only messages this very window posted to itself on the bridge
 * channel, forwards {id, method, params} (a JSON copy — nothing else from
 * the page) to the background, and posts the background's reply back.
 * The background derives the requesting origin from the MessageSender the
 * browser attaches to this script's messages.
 */
(() => {
  'use strict';
  const api = globalThis.browser || globalThis.chrome;
  const CHANNEL = 'nostr-signer-bridge/v1';

  function reply(id, body) {
    window.postMessage(Object.assign({ channel: CHANNEL, dir: 'to-page', id }, body), '*');
  }

  window.addEventListener('message', (ev) => {
    if (ev.source !== window) return;
    const d = ev.data;
    if (!d || typeof d !== 'object' || d.channel !== CHANNEL || d.dir !== 'to-ext') return;
    if (typeof d.id !== 'string' || d.id.length === 0 || d.id.length > 64) return;
    if (typeof d.method !== 'string' || d.method.length > 64) return;

    let params;
    try {
      params = JSON.parse(JSON.stringify(d.params === undefined ? {} : d.params));
    } catch (_) {
      reply(d.id, { error: { code: 'invalid_request', message: 'Parameters are not JSON-serializable' } });
      return;
    }

    let sent;
    try {
      sent = api.runtime.sendMessage({ type: 'nip07', id: d.id, method: d.method, params });
    } catch (e) {
      reply(d.id, { error: { code: 'signer_unavailable', message: 'Extension context unavailable' } });
      return;
    }
    Promise.resolve(sent).then(
      (resp) => {
        if (!resp || typeof resp !== 'object') {
          reply(d.id, { error: { code: 'internal', message: 'No response from the extension' } });
        } else if (resp.error) {
          reply(d.id, { error: { code: String(resp.error.code), message: String(resp.error.message) } });
        } else {
          reply(d.id, { result: resp.result });
        }
      },
      (e) => reply(d.id, { error: { code: 'signer_unavailable', message: String((e && e.message) || e) } }),
    );
  });
})();
