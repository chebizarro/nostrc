/* background.js — NIP-07 bridge background (nostrc-jjyp).
 *
 * Firefox: event page (manifest background.scripts, policy.js loaded
 * first). Chromium: service worker (importScripts below).
 *
 *   page.js ──postMessage──▶ content.js ──runtime.sendMessage──▶ here
 *   here ──runtime.connectNative("org.nostr.signer_bridge")──▶ host
 *   host ──D-Bus──▶ org.nostr.Signer (app_id = origin)
 *
 * Security-relevant duties:
 *   - the origin sent to the host comes from the browser's MessageSender
 *     (policy.originFromSender), never from the page's message;
 *   - requests are validated before a native process is spawned;
 *   - methods the signer does not gate itself (getPublicKey, getRelays,
 *     nip04/nip44 encrypt/decrypt) need a per-origin grant from the user,
 *     asked in an extension-owned prompt window; signEvent is gated by the
 *     signer's own approval dialog keyed on the same origin;
 *   - host request ids are generated here (never the page's id), so pages
 *     in different tabs cannot collide or answer for each other;
 *   - every call has a timeout that maps to a rejected promise.
 */
'use strict';

if (typeof NostrBridgePolicy === 'undefined' && typeof importScripts === 'function') {
  importScripts('lib/policy.js');
}

const api = globalThis.browser || globalThis.chrome;
const P = globalThis.NostrBridgePolicy;
const EXT_BASE = api.runtime.getURL('');
const PROMPT_TIMEOUT_MS = 120000;
const IDLE_DISCONNECT_MS = 30000;

/* ---- native host connection ------------------------------------------- */

let port = null;
let idleTimer = null;
let nextHostId = 1;
const inflight = new Map(); /* host id -> {resolve, timer} */

function err(code, message) { return { error: { code, message } }; }

function settle(hostId, body) {
  const p = inflight.get(hostId);
  if (!p) return;
  inflight.delete(hostId);
  clearTimeout(p.timer);
  p.resolve(body);
  scheduleIdleDisconnect();
}

function scheduleIdleDisconnect() {
  clearTimeout(idleTimer);
  if (inflight.size > 0 || !port) return;
  idleTimer = setTimeout(() => {
    if (inflight.size === 0 && port) { port.disconnect(); port = null; }
  }, IDLE_DISCONNECT_MS);
}

function ensurePort() {
  if (port) return port;
  const p = api.runtime.connectNative(P.NATIVE_HOST);
  p.onMessage.addListener((msg) => {
    if (!msg || typeof msg !== 'object' || typeof msg.id !== 'string') return; /* id:null = frame error */
    if (msg.error) settle(msg.id, err(String(msg.error.code), String(msg.error.message)));
    else settle(msg.id, { result: msg.result });
  });
  p.onDisconnect.addListener((dp) => {
    const why = (dp && dp.error && dp.error.message) ||
      (api.runtime.lastError && api.runtime.lastError.message) || 'native host exited';
    if (port === p) port = null;
    for (const id of Array.from(inflight.keys()))
      settle(id, err('signer_unavailable', 'Signer bridge unavailable: ' + why));
  });
  port = p;
  return p;
}

function hostCall(method, origin, params) {
  return new Promise((resolve) => {
    const id = 'b' + (nextHostId++);
    const timer = setTimeout(() => settle(id, err('timeout', 'The signer did not answer in time')),
      P.timeoutFor(method));
    inflight.set(id, { resolve, timer });
    clearTimeout(idleTimer);
    try {
      ensurePort().postMessage({ id, method, origin, params });
    } catch (e) {
      settle(id, err('signer_unavailable', 'Cannot reach the signer bridge: ' + ((e && e.message) || e)));
    }
  });
}

/* ---- per-origin grants -------------------------------------------------- */

async function loadGrants() {
  const { grants } = await api.storage.local.get('grants');
  return (grants && typeof grants === 'object') ? grants : {};
}

async function saveGrant(origin, gate) {
  const grants = await loadGrants();
  grants[origin] = Object.assign({}, grants[origin], { [gate]: Date.now() + P.GRANT_TTL_MS[gate] });
  await api.storage.local.set({ grants });
}

async function revokeGrant(origin, gate) {
  const grants = await loadGrants();
  if (!grants[origin]) return;
  if (gate) delete grants[origin][gate];
  if (!gate || Object.keys(grants[origin]).length === 0) delete grants[origin];
  await api.storage.local.set({ grants });
}

const prompts = new Map();      /* prompt id -> {origin, embedder, gate, method, resolve, windowId, timer} */
const promptByKey = new Map();  /* origin|gate -> Promise<boolean> */

function finishPrompt(id, allow, remember) {
  const pr = prompts.get(id);
  if (!pr) return;
  prompts.delete(id);
  promptByKey.delete(pr.key);
  clearTimeout(pr.timer);
  if (allow && remember) saveGrant(pr.origin, pr.gate);
  if (pr.windowId !== undefined) api.windows.remove(pr.windowId).catch(() => {});
  pr.resolve(!!allow);
}

function askUser(origin, gate, method, embedder) {
  const key = origin + '|' + gate + '|' + (embedder || '');
  if (promptByKey.has(key)) return promptByKey.get(key);
  const promise = new Promise((resolve) => {
    if (!api.windows || typeof api.windows.create !== 'function') { resolve(false); return; }
    const id = crypto.randomUUID();
    const pr = { origin, embedder, gate, method, key, resolve, windowId: undefined, timer: null };
    pr.timer = setTimeout(() => finishPrompt(id, false, false), PROMPT_TIMEOUT_MS);
    prompts.set(id, pr);
    api.windows.create({
      url: api.runtime.getURL('src/prompt.html') + '?id=' + encodeURIComponent(id),
      type: 'popup', width: 440, height: 340,
    }).then((w) => {
      if (prompts.has(id)) pr.windowId = w.id; else api.windows.remove(w.id).catch(() => {});
    }, () => finishPrompt(id, false, false));
  });
  promptByKey.set(key, promise);
  return promise;
}

if (api.windows && api.windows.onRemoved) {
  api.windows.onRemoved.addListener((windowId) => {
    for (const [id, pr] of prompts) if (pr.windowId === windowId) finishPrompt(id, false, false);
  });
}

async function permitted(origin, gate, method, embedder) {
  if (P.grantValid(await loadGrants(), origin, gate, Date.now())) return true;
  return askUser(origin, gate, method, embedder);
}

/* For a request from an iframe: the top-level page's origin, shown in the
 * prompt so the user sees "snort.social embedded in example.com". Needs no
 * extra permission: sender.tab.url is populated for pages our content
 * scripts match. */
function embedderOf(sender, origin) {
  if (sender.frameId === 0 || !sender.tab || typeof sender.tab.url !== 'string') return null;
  try {
    const top = new URL(sender.tab.url).origin;
    return top !== origin ? top : null;
  } catch (_) {
    return 'an unknown page';
  }
}

/* ---- message handling --------------------------------------------------- */

function fromExtensionPage(sender) {
  return sender && sender.id === api.runtime.id && typeof sender.url === 'string' &&
    sender.url.startsWith(EXT_BASE);
}

async function handleNip07(msg, sender) {
  /* Only content-script messages from a tab frame carry a frameId. */
  if (typeof sender.frameId !== 'number' || !sender.tab)
    return err('origin_denied', 'Requests must come from a web page');
  const origin = P.originFromSender(sender);
  if (!origin) return err('origin_denied', 'Only secure (https or localhost) pages may use the signer');
  const v = P.validateRequest(msg.method, msg.params);
  if (v.error) return err(v.code, v.error);
  const gate = P.gateFor(msg.method);
  if (sender.frameId !== 0 && !P.allowedInSubframe(gate))
    return err('origin_denied', 'Decryption is only available to top-level pages, not embedded frames');
  if (gate && !(await permitted(origin, gate, msg.method, embedderOf(sender, origin))))
    return err('rejected', 'The user rejected the request');
  return hostCall(msg.method, origin, v.params);
}

async function handleUi(msg, sender) {
  if (!fromExtensionPage(sender)) return err('rejected', 'Not allowed');
  switch (msg.type) {
    case 'prompt:get': {
      const pr = prompts.get(msg.id);
      return pr ? { result: { origin: pr.origin, embedder: pr.embedder, gate: pr.gate, method: pr.method,
                               ttlHours: Math.round(P.GRANT_TTL_MS[pr.gate] / 3600000) } }
                : err('not_found', 'Expired');
    }
    case 'prompt:answer':
      finishPrompt(msg.id, !!msg.allow, !!msg.remember);
      return { result: true };
    case 'grants:list':
      return { result: await loadGrants() };
    case 'grants:revoke':
      await revokeGrant(String(msg.origin), msg.gate ? String(msg.gate) : null);
      return { result: true };
    case 'host:hello':
      return hostCall('host.hello', null, {});
  }
  return err('unknown_method', 'Unknown message');
}

api.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg || typeof msg !== 'object' || sender.id !== api.runtime.id) return false;
  let work;
  if (msg.type === 'nip07') {
    if (!P.isValidPageId(msg.id)) return false;
    work = handleNip07(msg, sender);
  } else if (typeof msg.type === 'string') {
    work = handleUi(msg, sender);
  } else {
    return false;
  }
  work.then(sendResponse, (e) => sendResponse(err('internal', String((e && e.message) || e))));
  return true; /* async sendResponse (Chromium + Firefox) */
});
