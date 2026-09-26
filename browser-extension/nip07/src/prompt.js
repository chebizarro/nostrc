/* prompt.js — per-origin permission prompt (nostrc-jjyp). The origin shown
 * comes from the background (browser-derived), never from the page. */
'use strict';
const api = globalThis.browser || globalThis.chrome;
const id = new URLSearchParams(location.search).get('id');
const WHAT = {
  read: 'read your public key and relay list',
  encrypt: 'encrypt messages with your key (NIP-04 / NIP-44)',
  decrypt: 'decrypt messages sent to you (NIP-04 / NIP-44)',
};

async function answer(allow) {
  const remember = document.getElementById('remember').checked;
  await api.runtime.sendMessage({ type: 'prompt:answer', id, allow, remember });
  window.close();
}

(async () => {
  const resp = await api.runtime.sendMessage({ type: 'prompt:get', id });
  if (!resp || !resp.result) { window.close(); return; }
  document.getElementById('origin').textContent = resp.result.origin;
  document.getElementById('what').textContent = WHAT[resp.result.gate] || resp.result.method;
  const hours = resp.result.ttlHours;
  document.getElementById('ttl').textContent = hours >= 48 ? `${Math.round(hours / 24)} days` : `${hours} hours`;
  if (resp.result.embedder) {
    document.getElementById('embedder').textContent = resp.result.embedder;
    document.getElementById('embedded').hidden = false;
  }
  if (resp.result.gate === 'decrypt') document.getElementById('remember').parentElement.title =
    'Remembering lets this site decrypt any message addressed to you without asking again until it expires.';
  document.getElementById('allow').addEventListener('click', () => answer(true));
  document.getElementById('deny').addEventListener('click', () => answer(false));
})();
