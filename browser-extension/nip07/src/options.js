/* options.js — list / revoke remembered per-origin grants (nostrc-jjyp). */
'use strict';
const api = globalThis.browser || globalThis.chrome;

async function render() {
  const resp = await api.runtime.sendMessage({ type: 'grants:list' });
  const grants = (resp && resp.result) || {};
  const tbody = document.getElementById('grants');
  tbody.textContent = '';
  const origins = Object.keys(grants).sort();
  if (origins.length === 0) {
    const tr = tbody.insertRow();
    const td = tr.insertCell();
    td.colSpan = 3;
    td.textContent = 'None';
  }
  for (const origin of origins) {
    const tr = tbody.insertRow();
    tr.insertCell().textContent = origin;
    tr.insertCell().textContent = Object.entries(grants[origin])
      .map(([gate, until]) => `${gate} (until ${new Date(until).toLocaleString()})`).join(', ');
    const btn = document.createElement('button');
    btn.textContent = 'Revoke';
    btn.addEventListener('click', async () => {
      await api.runtime.sendMessage({ type: 'grants:revoke', origin });
      render();
    });
    tr.insertCell().appendChild(btn);
  }
}

(async () => {
  render();
  const hello = await api.runtime.sendMessage({ type: 'host:hello' });
  document.getElementById('host').textContent = (hello && hello.result)
    ? `Native host ${hello.result.host} ${hello.result.version} (protocol ${hello.result.protocol}) is installed.`
    : `Native host unavailable: ${(hello && hello.error && hello.error.message) || 'unknown error'}`;
})();
