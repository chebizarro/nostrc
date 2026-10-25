// Real lab acceptance, not a mocked relay/store. Only disposable test keys.
import { finalizeEvent } from "nostr-tools/pure";
const [blossom, relay] = Deno.args;
if (!blossom || !relay) throw new Error("expected Blossom HTTP URL and relay WS URL");
const member = new Uint8Array(32); member[31] = 1;
const outsider = new Uint8Array(32); outsider[31] = 2;
function check(ok: unknown, message: string): asserts ok {
  if (!ok) throw new Error(message);
}
const now = () => Math.floor(Date.now() / 1000);
function signed(key: Uint8Array, kind: number, tags: string[][], content = "VSP-01 conformance", created_at = now()) {
  return finalizeEvent({ kind, created_at, tags, content }, key);
}
function token(key: Uint8Array, verb: string, hash?: string, tags: string[][] = []) {
  const e = signed(key, 24242, [["t", verb], ["expiration", String(now() + 300)],
    ...(hash ? [["x", hash]] : []), ...tags]);
  return "Nostr " + btoa(JSON.stringify(e)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
async function http(path: string, method = "GET", auth?: string, body?: Uint8Array<ArrayBuffer>) {
  const headers: Record<string, string> = {};
  if (auth) headers.Authorization = auth;
  if (body) {
    headers["Content-Type"] = "application/octet-stream";
    headers["X-SHA-256"] = hash;
  }
  return await fetch(blossom + path, { method, headers, body,
    signal: AbortSignal.timeout(15000) });
}
const blob = crypto.getRandomValues(new Uint8Array(8192));
const hash = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", blob)))
  .map((x) => x.toString(16).padStart(2, "0")).join("");
const missing = "0".repeat(64);
const info = await http("/");
check(info.ok && (await info.json()).private === true, "Blossom private marker");
for (const method of ["GET", "HEAD"]) {
  for (const h of [hash, missing]) {
    for (const auth of [undefined, token(outsider, "get", h)]) {
      const r = await http("/" + h, method, auth);
      check(r.status === 401 && r.headers.get("WWW-Authenticate") === "Nostr",
        `${method} nonmember/missing must be 401 + challenge, got ${r.status}`);
      await r.arrayBuffer();
    }
  }
}
for (const auth of [undefined, token(outsider, "upload", hash)]) {
  const denied = await http("/upload", "PUT", auth, blob);
  check(denied.status === 401 && denied.headers.get("WWW-Authenticate") === "Nostr", "nonmember upload");
  await denied.arrayBuffer();
}
const oversize = await fetch(blossom + "/upload", {
  method: "HEAD",
  headers: { Authorization: token(member, "upload", hash), "X-SHA-256": hash,
    "X-Content-Type": "application/octet-stream", "X-Content-Length": "8388609" },
  signal: AbortSignal.timeout(15000),
});
check(oversize.status === 413, "private mode must retain the 8 MiB cap");
const uploaded = await http("/upload", "PUT", token(member, "upload", hash), blob);
check([200, 201].includes(uploaded.status), `member upload: ${uploaded.status} ${await uploaded.text()}`);
const head = await http("/" + hash, "HEAD", token(member, "get", hash));
check(head.status === 200, "member HEAD");
const got = await http("/" + hash, "GET", token(member, "get", hash));
check(got.status === 200 && got.headers.get("content-type")?.startsWith("application/octet-stream"), "member GET MIME");
check(got.headers.get("cache-control") === "no-store", "private blobs must not carry upstream public caching");
const bytes = new Uint8Array(await got.arrayBuffer());
check(bytes.length === blob.length && bytes.every((b, i) => b === blob[i]), "byte-exact octet-stream round trip");
for (const method of ["GET", "HEAD"]) {
  for (const auth of [undefined, token(outsider, "get", hash)]) {
    const r = await http("/" + hash, method, auth);
    check(r.status === 401 && r.headers.get("WWW-Authenticate") === "Nostr", "existing blob nonmember denied");
    await r.arrayBuffer();
  }
}
for (const auth of [token(outsider, "get", hash), token(member, "get", missing),
                    token(member, "upload", hash), token(member, "get", hash, [["server", "wrong.invalid"]])]) {
  const r = await http("/" + hash, "GET", auth);
  check(r.status === 401, "existing blob must not bypass membership/action/hash/server scope");
  await r.arrayBuffer();
}
const absent = await http("/" + missing, "GET", token(member, "get", missing));
check(absent.status === 404, "authorized missing blob must remain 404");
await absent.arrayBuffer();
console.log("PASS Blossom: private, 401 existence hiding, upload allowlist, scoped auth, octet-stream round trip");

class Wire {
  ws: WebSocket;
  pending: unknown[][] = [];
  wake: (() => void) | undefined;
  opened: Promise<void>;
  constructor() {
    this.ws = new WebSocket(relay);
    this.opened = new Promise((resolve, reject) => {
      this.ws.onopen = () => resolve();
      this.ws.onerror = () => reject(new Error("relay websocket error"));
    });
    this.ws.onmessage = (e) => { this.pending.push(JSON.parse(e.data)); this.wake?.(); };
    this.ws.onclose = () => this.wake?.();
  }
  send(frame: unknown[]) { this.ws.send(JSON.stringify(frame)); }
  async recv(): Promise<unknown[]> {
    if (!this.pending.length) {
      await new Promise<void>((resolve, reject) => {
        const timer = setTimeout(() => { this.wake = undefined; reject(new Error("relay frame deadline")); }, 10000);
        this.wake = () => { clearTimeout(timer); this.wake = undefined; resolve(); };
      });
    }
    const frame = this.pending.shift();
    check(frame, "relay closed before a protocol reply");
    return frame;
  }
  async auth(key: Uint8Array, challenge: string, tags?: string[][], kind = 22242) {
    const event = signed(key, kind, tags ?? [["relay", relay], ["challenge", challenge]]);
    this.send(["AUTH", event]);
    const ok = await this.recv();
    check(ok[0] === "OK" && ok[1] === event.id, "AUTH must return correlated OK");
    return ok;
  }
}
const nip11 = await fetch(relay.replace(/^ws/, "http"), {
  headers: { Accept: "application/nostr+json" }, signal: AbortSignal.timeout(10000),
});
const doc = await nip11.json();
check(doc.vault === true && doc.limitation?.auth_required === true, "NIP-11 vault/auth_required");
const w = new Wire();
try {
  await w.opened;
  const challenge = await w.recv();
  check(challenge[0] === "AUTH" && typeof challenge[1] === "string", "NIP-42 challenge");
  const chal = challenge[1];
  const events = [30078, 445, 1059].map((kind) => signed(outsider, kind,
    kind === 30078 ? [["d", "vault:sv9:" + crypto.randomUUID()]] : [], crypto.randomUUID()));
  async function denied(prefix: string) {
    for (const filter of [{ kinds: [30078] }, { kinds: [445] }, { kinds: [1059] }, {},
                          { ids: [events[0].id] }, { kinds: [1, 1059] }]) {
      w.send(["REQ", "private", filter]);
      const r = await w.recv();
      check(r[0] === "CLOSED" && r[1] === "private" && String(r[2]).startsWith(prefix), `REQ must be ${prefix}: ${r}`);
    }
    w.send(["COUNT", "count", {}]);
    const count = await w.recv();
    check(count[0] === "CLOSED" && String(count[2]).startsWith(prefix), "COUNT must not leak vault inventory");
    for (const event of events) {
      w.send(["EVENT", event]);
      const ok = await w.recv();
      check(ok[0] === "OK" && ok[1] === event.id && ok[2] === false && String(ok[3]).startsWith(prefix), "protected EVENT denied");
    }
  }
  await denied("auth-required:");
  w.send(["AUTH", {}]);
  check((await w.recv())[2] === false, "malformed AUTH refused with OK");
  const stale = signed(member, 22242, [["relay", relay], ["challenge", chal]], "stale auth", now() - 601);
  w.send(["AUTH", stale]);
  check((await w.recv())[2] === false, "stale AUTH refused");
  const forged = signed(member, 22242, [["relay", relay], ["challenge", chal]]);
  forged.content = "tampered";
  w.send(["AUTH", forged]);
  check((await w.recv())[2] === false, "tampered AUTH refused");
  check((await w.auth(member, chal, undefined, 1))[2] === false, "wrong AUTH kind refused");
  check((await w.auth(member, chal, [["relay", "ws://wrong.invalid"], ["challenge", chal]]))[2] === false, "wrong relay refused");
  check((await w.auth(member, chal, [["relay", relay], ["challenge", "wrong"]]))[2] === false, "wrong challenge refused");
  check((await w.auth(outsider, chal))[2] === true, "outsider can authenticate, but not access vault");
  await denied("restricted:");
  check((await w.auth(member, chal))[2] === true, "member AUTH");
  // Gift wraps have random outer authors; authorization belongs to the session.
  for (const event of events) {
    w.send(["EVENT", event]);
    const ok = await w.recv();
    check(ok[0] === "OK" && ok[1] === event.id && ok[2] === true, `member publish ${event.kind}: ${ok}`);
    w.send(["REQ", "roundtrip", { ids: [event.id], kinds: [event.kind], limit: 1 }]);
    const received = await w.recv();
    check(received[0] === "EVENT" && (received[2] as { id: string }).id === event.id, "OK must imply query-visible event");
    const eose = await w.recv();
    check(eose[0] === "EOSE" && eose[1] === "roundtrip", "EOSE is catch-up completion");
  }
  console.log("PASS relayd: NIP-11, NIP-42 validation/OK, auth-required/restricted, broad/COUNT guards, all three kinds round trip");
} finally {
  w.ws.close();
}
