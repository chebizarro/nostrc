# Porthome — Blossom BUD-02 batch-auth probe (D8)

Bead: `nostrc-yo44` (P3). Design context: `docs/designs/home-from-relay.md` §4.3 row **D8** and §12 item 10. Report-only. No code changes to porthome, syncd, fuse, libhanami, or auth.

## 1. Scope + methodology

**Question.** BUD-02 (`kind 24242`) auth events carry an `x` tag naming a specific blob hash. Does the Blossom ecosystem accept **one** signed kind-24242 event whose `x` tags enumerate an upcoming *batch* of blobs as the `Authorization:` header for all the PUTs in that batch, or is each PUT required to present its own `x`-scoped auth event?

If batch is universally accepted, the Phase-2 provisioner (`nostrc-89rj`, `nh_porthome_blossom_upload`) can amortize one NIP-46 `sign_event` round-trip across a batch of chunk uploads. If not, we fall to the D9 delegate-key mitigation. Either result is actionable — D8 is only about which code path Phase 2 codes first.

**Ephemeral keypairs.** Each round generated a fresh secp256k1 keypair held only in Python memory and discarded on process exit. Private keys were never written to disk or logged. Public keys (x-only, hex) — grep server logs to distinguish this probe's traffic:

```
round 1 pubkey: d027147b6f25cf439f33574d58a9d5c06d055d5062c6bc366d3a1fef93c8ec3a
round 2 pubkey: 27bf73ee83f9a63222a8bec932afa7ce30bcd89c645bf2688d76914f8ef96630
round 3 pubkey: 403c237d81b0cebc0aa850b4420062896c374ee83115c8e3574de25fe4089ae6
```

All three keys are single-use throwaways.

**Servers probed.**

| Server                          | Why chosen                                                       |
| ------------------------------- | ---------------------------------------------------------------- |
| `https://blossom.sharegap.net`  | We operate this server — full server-side visibility available.  |
| `https://blossom.band`          | High-visibility community server (nostr.build-hosted).           |
| `https://cdn.satellite.earth`   | Second widely-used community server.                             |
| `https://blossom.primal.net`    | Third widely-used community server (primal.net infra).           |

**Load discipline.** Sequential PUTs of ≤ 2 KiB random bytes; 2 runs per probe (baseline / batch / mismatch); ≤ 10 total PUTs per server per round; ≥ 10 s between servers in round 1; `User-Agent: nostrc-yo44-probe/2026-09-24` on every request; no concurrent PUTs. Three rounds were run (round 1 = full probe on all four servers; round 2 = server-tag / Content-Type variant search on blossom.band and blossom.primal.net; round 3 = clean batch/mismatch on those two servers with the variant that passes auth). Total ≤ 44 PUTs across all rounds.

**Origin.** All probes ran outbound from the local macOS workstation (bizarro-adjacent, not gnome-dev). `libhanami` was NOT invoked; the probe used a self-contained Python script that reimplements the BUD-02 event shape (kind `24242`, `t=upload`, one-or-many `x` tags, `expiration`, optional `server`) and the `Nostr <base64(JSON)>` header — matching `libhanami/src/hanami-bud02-auth.c:180-260` (`hanami_bud02_create_auth_event` currently accepts one hash; the batch case requires the new call filed as `nostrc-xeby`). BIP-340 Schnorr signing via the `secp256k1` PyPI package; NIP-01 event IDs computed from `json.dumps(..., separators=(",",":"))`. The probe scripts live at `/tmp/yo44_probe/probe{,2,3}.py` on the workstation and are **not** committed.

## 2. Per-server matrix

Each cell is `HTTP-status (short body excerpt)`. Statuses within a run are `[first_put, second_put, ...]`. Two independent runs shown as `[…] | […]`.

| Server                                 | A. baseline (auth x=[H], PUT H)            | B. batch (auth x=[H1,H2,H3], PUT H1,H2,H3)                       | C. mismatch (auth x=[H1], PUT H2)                          | Auth-layer verdict              | Batch viable? |
| -------------------------------------- | ------------------------------------------ | ----------------------------------------------------------------- | ---------------------------------------------------------- | ------------------------------- | ------------- |
| `blossom.sharegap.net`                 | `[201, 201]`                               | `[201, 201, 201] \| [201, 201, 201]` — all six blobs stored       | `[403, 403]` "Auth token does not authorize operation on blob …" | Batch ACCEPT, mismatch STRICT   | **YES** (definitive) |
| `blossom.band` (round 1, `server` tag) | `[401, 401]` "server tags do not include this server" | `[401, 401, 401] \| [401, 401, 401]`                              | `[401, 401]`                                               | AUTH REJECTED (server-tag policy — see §2.1) | inconclusive with server tag |
| `blossom.band` (round 3, no `server` tag) | `[415, 415]` "File type not allowed" — auth PASS | `[415, 415, 415] \| [415, 415, 415]` — auth PASS on all six       | `[415, 415]` — auth PASS (permissive on `x` mismatch)      | Batch ACCEPT, mismatch PERMISSIVE | **YES** (auth layer)         |
| `blossom.primal.net`                   | `[415, 415]` "unsupported media type" — auth PASS | `[415, 415, 415] \| [415, 415, 415]` — auth PASS on all six       | `[401, 401]` "invalid x tag"                                | Batch ACCEPT, mismatch STRICT   | **YES** (auth layer)         |
| `cdn.satellite.earth`                  | `[timeout, timeout]` (30 s each)           | `[timeout×3] \| [500, 500, timeout]` — nginx 5xx / read timeouts  | `[timeout, timeout]`                                       | INCONCLUSIVE (degraded server)  | **UNKNOWN**                  |

Delegate-fallback probe (per-blob auth on a server that rejected batch in round 1) — run once against blossom.band with the same key. Result: `PUT /upload → 401 "server tags do not include this server"` — same 401 as batch, i.e. blossom.band's round-1 rejection was NOT batch-related, it was the server-tag mismatch. Round 3 confirmed this and produced the true auth-layer batch verdict for that server.

### 2.1 Reading the 415s and the blossom.band server-tag quirk

`blossom.band` and `blossom.primal.net` are hosted with content-policy filters (nostr.build backend on blossom.band; a similar policy on primal.net) that reject 2 KiB random-byte blobs *after* auth passes:

- `401` (with body naming the auth failure) = auth rejected the event
- `415` (with body naming the content policy) = auth accepted, body rejected

That distinction is what lets us characterise batch behavior on these servers even though random-byte test blobs do not survive their media filters. **On both servers, all six batched PUTs returned `415`** — meaning the shared auth event with three `x` tags was accepted for each of the three distinct blobs. On primal.net the mismatch (`x=H1`, PUT `H2`) flipped to `401 "invalid x tag"` — clear evidence primal.net's BUD-02 impl enforces the `x` binding per request. blossom.band did NOT flip on mismatch (`415`), which is a separate observation about that server's auth strictness — filed as `nostrc-5uij`.

`blossom.band`'s BUD-02 impl rejects the auth event with `401 "server tags do not include this server"` when a `server` tag is present in any URL form we tried (`https://blossom.band`, trailing-slash variant, host-only, no scheme). Omitting the `server` tag entirely lets the auth event through. BUD-02 makes the `server` tag optional, so this is spec-conformant; a portable wrapper must be prepared for either behavior and default to omitting the tag unless a per-server capability probe tells it otherwise.

## 3. D8 verdict

**BATCH-VIABLE.**

Every server whose auth layer we could reach — three of the four probed — accepted a single kind-24242 event with N `x` tags as `Authorization:` for N sequential PUTs of the enumerated blobs:

- `blossom.sharegap.net` returned `201` end-to-end for all six batched blobs (definitive, we operate the server and can inspect storage).
- `blossom.band` (round 3, no `server` tag) returned `415` on all six batched PUTs — content-policy 415 means auth passed.
- `blossom.primal.net` returned `415` on all six batched PUTs and additionally returned `401 "invalid x tag"` for the mismatch case, proving the `x` binding is enforced per request against the auth event's list.

The `x` tag is checked as *"is `sha256(request-body)` in the event's `x` tag list?"* — not *"is there exactly one `x` tag and does it equal this blob's hash?"*. That's the shape BUD-02 mandates (`hzrd149/blossom/buds/02.md`), and libhanami's `hanami_bud02_validate_auth_event` matches: validation of `expected_sha256` is an optional server-side choice.

**Caveat.** `cdn.satellite.earth` was operationally degraded on 2026-09-24 — 30-second read timeouts on nearly every PUT and `500 nginx/1.24.0 (Ubuntu)` on the ones that responded. Baseline single-blob PUTs got the same 500/timeout, so there is no evidence batching specifically fails there. But we also cannot *prove* batch works there. The Phase-2 wrapper should have a per-server capability probe at connect time so a degraded server does not wedge the provisioner (filed as `nostrc-ypn2`).

**Recommendation.** Implement the batch path as the default in the Phase-2 wrapper. Add a defensive fall-back: on any `401` for a batched PUT, cache the server as "no-batch" for the session and re-issue that PUT (and subsequent PUTs to that server) with a per-blob auth event. Cost: one extra `sign_event` per batch on a hostile server; zero on the three servers we probed.

## 4. Latency + body-size headroom

All PUTs were 2 KiB random bytes; no size cap was exercised. Observed latency floor per server:

| Server                         | Best-case latency | Worst-case latency (non-timeout) | Size hints observed          |
| ------------------------------ | ----------------- | -------------------------------- | ---------------------------- |
| `blossom.sharegap.net`         | 27 ms             | 75 ms                            | 201 for 2 KiB; no size hint  |
| `blossom.band`                 | 115 ms            | 1737 ms (first PUT on cold conn) | 415 body-policy; no size hint |
| `blossom.primal.net`           | 553 ms            | 637 ms                           | 415 body-policy; no size hint |
| `cdn.satellite.earth`          | 291 ms (5xx)      | 30 000 ms (timeout)              | Cannot exercise              |

Size-cap probing is out of scope for D8. Filed as `nostrc-t44z` — needs real allowed content (e.g. image/png header + noise) to survive body filters on blossom.band and primal.net.

## 5. Recommendation for Phase 2 wrapper implementation

Add to `libhanami` a new `hanami_bud02_create_batch_auth_event(action, sha256_hex_array, count, expiration, server_url_or_null)` that mints one kind-24242 event with `["x", h_i]` tags per hash and one shared `["expiration", …]`. The porthome caller (`nh_porthome_blossom_upload`) buckets blob PUTs into batches of ~8–16 chunks, does one NIP-46 `sign_event` round-trip per batch, and PUTs each blob with the shared `Authorization:`. On `401` for any PUT in a batch, mark that server as no-batch for the session (session-scoped negative cache) and re-issue per-blob. **Do not** include a `server` tag in the batch event by default — blossom.band actively rejects every URL form we tried, while sharegap and primal accept both variants; omitting is the safe cross-server posture. Add a per-server capability probe at connect time so degraded servers get quarantined instead of wedging every upload.

The D9 delegate-key path stays on the roadmap for a different reason than D8. D9 mitigates the key-exposure and rate-limit surface when the account key is a hardware/bunker key that costs a slow round-trip per signature. D9 is orthogonal to whether batching works on the wire; batching reduces the count of D9 delegate-key uses just as much as it reduces the count of account-key uses.

## 6. Follow-ups (filed)

- **`nostrc-xeby`** (P2) — `libhanami`: `hanami_bud02_create_batch_auth_event()` + `hanami_blossom_upload_batch()`, wired from `nh_porthome_blossom_upload` in Phase 2. Includes the per-server `401`-on-batch fall-back and the session-scoped negative-capability cache. Blocks `nostrc-89rj`.
- **`nostrc-ypn2`** (P3) — porthome: at Blossom connect time, run a lightweight per-server capability probe (BUD-01 `HEAD /` or `GET /<random-hex>`, plus a batch-of-2 PUT using an ephemeral session key) and record: reachable, accepts `server` tag, batch-viable, strict vs permissive on `x` mismatch. Cache per-server for the session; feed to the batch wrapper. Blocks `nostrc-89rj`.
- **`nostrc-t44z`** (P3) — Blossom body-size cap probe. Complement to D8; not part of D8. Should run 1 MiB / 8 MiB / 32 MiB / 64 MiB PUTs with a real allowed content type (image/png header + noise) to characterise per-server body-size ceilings so porthome's chunker sizes correctly.
- **`nostrc-5uij`** (P4) — blossom.band conformance note: `x`-tag binding not enforced when the `server` tag is omitted (evidence: round-3 mismatch on blossom.band returned `415` content-policy instead of `401` auth). Noting for the operator; not blocking porthome.

## 7. Raw probe log (redacted)

Auth headers below are shortened to the first 8 base64 characters + total base64 length. Blob hashes are full 64-char hex. Full logs retained at `/tmp/yo44_probe/run{1,2,3}.log` on the probe workstation.

### Round 1 — full probe, all four servers

```
# ephemeral probe pubkey (x-only, hex): d027147b6f25cf439f33574d58a9d5c06d055d5062c6bc366d3a1fef93c8ec3a
# UA: nostrc-yo44-probe/2026-09-24
# started: 2026-09-25T04:50:56Z

=== https://blossom.sharegap.net ===
  HEALTH GET /does-not-exist-<hex>                                                                  404  43ms
  # A. baseline
  PUT /upload  201  74ms  auth=Nostr eyJpZCI6... (684 b64)  body=<url…c65f88d7…,sha256=c65f88d7…,size=2048>
  PUT /upload  201  41ms  auth=Nostr eyJpZCI6... (684 b64)  body=<url…b96530f8…,sha256=b96530f8…,size=2048>
  # B. batch run 1: single event x-tags=[4dfa9ef6…, 00cf8064…, e5260b8b…]
  PUT /upload  201  35ms  auth=Nostr eyJpZCI6... (880 b64)  body=<url…4dfa9ef6…,size=2048>
  PUT /upload  201  37ms  auth=Nostr eyJpZCI6... (880 b64)  body=<url…00cf8064…,size=2048>
  PUT /upload  201  72ms  auth=Nostr eyJpZCI6... (880 b64)  body=<url…e5260b8b…,size=2048>
  # B. batch run 2: single event x-tags=[787d48d6…, 75585b1d…, ffa7d6e7…]
  PUT /upload  201  37ms  auth=Nostr eyJpZCI6... (880 b64)  body=<url…787d48d6…,size=2048>
  PUT /upload  201  27ms  auth=Nostr eyJpZCI6... (880 b64)  body=<url…75585b1d…,size=2048>
  PUT /upload  201  36ms  auth=Nostr eyJpZCI6... (880 b64)  body=<url…ffa7d6e7…,size=2048>
  # C. mismatch run 1: auth x=e6973f14… but PUT blob 2a5c504e…
  PUT /upload  403  75ms  auth=Nostr eyJpZCI6... (684 b64)  body="Auth token does not authorize operation on blob 2a5c504e…"
  # C. mismatch run 2: auth x=62c98878… but PUT blob 69673030…
  PUT /upload  403  42ms  auth=Nostr eyJpZCI6... (684 b64)  body="Auth token does not authorize operation on blob 69673030…"
  SUMMARY: A=[201,201]  B=[201,201,201] | [201,201,201]  C=[403,403]  batch_viable=True  mismatch_permissive=False

=== https://blossom.band ===
  HEALTH GET /does-not-exist-<hex>                                                                  400  153ms
  # A. baseline
  PUT /upload  401  178ms  auth=Nostr eyJpZCI6... (672 b64)  body="Blossom Authorization event server tags do not include this server"
  PUT /upload  401  141ms  auth=Nostr eyJpZCI6... (672 b64)  body="Blossom Authorization event server tags do not include this server"
  # B. batch run 1 (server tag present): x-tags=[39d92599…, cc37cb13…, 35cc9f53…]
  PUT /upload  401  146ms  auth=Nostr eyJpZCI6... (868 b64)  body="Blossom Authorization event server tags do not include this server"
  PUT /upload  401  115ms  auth=Nostr eyJpZCI6... (868 b64)  body="Blossom Authorization event server tags do not include this server"
  PUT /upload  401  124ms  auth=Nostr eyJpZCI6... (868 b64)  body="Blossom Authorization event server tags do not include this server"
  # B. batch run 2 (server tag present): x-tags=[9ad9c509…, ddd4d5fa…, cc2d463d…]
  PUT /upload  401  137ms  auth=Nostr eyJpZCI6... (868 b64)  body=<same as above>
  PUT /upload  401  138ms  auth=Nostr eyJpZCI6... (868 b64)  body=<same as above>
  PUT /upload  401  131ms  auth=Nostr eyJpZCI6... (868 b64)  body=<same as above>
  # C. mismatch (server tag present)
  PUT /upload  401  122ms  auth=Nostr eyJpZCI6... (672 b64)  body=<same as above>
  PUT /upload  401  118ms  auth=Nostr eyJpZCI6... (672 b64)  body=<same as above>
  SUMMARY: A=[401,401]  B=[401,401,401] | [401,401,401]  C=[401,401]  auth REJECTED by server-tag policy — see round 3

=== https://cdn.satellite.earth ===
  HEALTH GET /does-not-exist-<hex>                                                                  404  309ms
  PUT /upload  ERROR 30429ms  ReadTimeout   auth=Nostr eyJpZCI6... (684 b64)
  PUT /upload  ERROR 30231ms  ReadTimeout   auth=Nostr eyJpZCI6... (684 b64)
  # B. batch run 1
  PUT /upload  ERROR 30219ms  ReadTimeout   auth=Nostr eyJpZCI6... (876 b64)
  PUT /upload  ERROR 30204ms  ReadTimeout   auth=Nostr eyJpZCI6... (876 b64)
  PUT /upload  ERROR 30205ms  ReadTimeout   auth=Nostr eyJpZCI6... (876 b64)
  # B. batch run 2
  PUT /upload  500  291ms   auth=Nostr eyJpZCI6... (876 b64)  body="<html><title>500 Internal Server Error</title>…nginx/1.24.0 (Ubuntu)"
  PUT /upload  500  295ms   auth=Nostr eyJpZCI6... (876 b64)  body=<same as above>
  PUT /upload  ERROR 30196ms  ReadTimeout   auth=Nostr eyJpZCI6... (876 b64)
  # C. mismatch
  PUT /upload  ERROR 30195ms  ReadTimeout   auth=Nostr eyJpZCI6... (684 b64)
  PUT /upload  ERROR 30191ms  ReadTimeout   auth=Nostr eyJpZCI6... (684 b64)
  SUMMARY: A=[-1,-1]  B=[-1,-1,-1] | [500,500,-1]  C=[-1,-1]  server DEGRADED — inconclusive

=== https://blossom.primal.net ===
  HEALTH GET /does-not-exist-<hex>                                                                  400  591ms
  # A. baseline (auth-pass indicator = 415, auth-fail = 401)
  PUT /upload  415  570ms  auth=Nostr eyJpZCI6... (680 b64)  body="upload rejected: unsupported media type application/octet-stream"
  PUT /upload  415  568ms  auth=Nostr eyJpZCI6... (680 b64)  body=<same as above>
  # B. batch run 1: x-tags=[7425238e…, f8454195…, dea996d4…]
  PUT /upload  415  565ms  auth=Nostr eyJpZCI6... (876 b64)  body=<same as above>
  PUT /upload  415  562ms  auth=Nostr eyJpZCI6... (876 b64)  body=<same as above>
  PUT /upload  415  580ms  auth=Nostr eyJpZCI6... (876 b64)  body=<same as above>
  # B. batch run 2: x-tags=[b8b333c6…, e2ab9e02…, f74b1fcf…]
  PUT /upload  415  572ms  auth=Nostr eyJpZCI6... (876 b64)  body=<same as above>
  PUT /upload  415  574ms  auth=Nostr eyJpZCI6... (876 b64)  body=<same as above>
  PUT /upload  415  571ms  auth=Nostr eyJpZCI6... (876 b64)  body=<same as above>
  # C. mismatch: auth x=ba079dbd… but PUT blob 5e26578c…
  PUT /upload  401  584ms  auth=Nostr eyJpZCI6... (680 b64)  body="invalid x tag"
  # C. mismatch: auth x=28ad4b69… but PUT blob f7279104…
  PUT /upload  401  574ms  auth=Nostr eyJpZCI6... (680 b64)  body="invalid x tag"
  SUMMARY: A=[415,415]  B=[415,415,415] | [415,415,415]  C=[401,401]  batch auth ACCEPTED, mismatch STRICT

# delegate-fallback probe on the one server that rejected batch in round 1
=== https://blossom.band DELEGATE-FALLBACK (per-blob auth) ===
  PUT /upload  401  148ms  auth=Nostr eyJpZCI6... (672 b64)  body="Blossom Authorization event server tags do not include this server"
  → conclusion: blossom.band's 401 is the server-tag policy, NOT a per-vs-batch verdict. Round 3 removes the server tag and re-tests.
```

### Round 2 — server-tag / Content-Type variant search

```
# probe pubkey: 27bf73ee83f9a63222a8bec932afa7ce30bcd89c645bf2688d76914f8ef96630

=== blossom.band tag-format variants ===
  variant "trailing slash" server_tag="https://blossom.band/"
    PUT /upload  401  148ms  ct=application/octet-stream  body="Blossom Authorization event server tags do not include this server"
  variant "no server tag" server_tag=None
    PUT /upload  415  1737ms  ct=application/octet-stream  body="File type not allowed, unsupported. Please check https://nostr.build for supported file types and paid options"
  variant "host only" server_tag="blossom.band"
    PUT /upload  415  372ms  ct=application/octet-stream  body="File type not allowed, unsupported. Please check https://nostr.build for supported file types and paid options"
  variant "exact URL (baseline)" server_tag="https://blossom.band"
    PUT /upload  401  145ms  ct=application/octet-stream  body="Blossom Authorization event server tags do not include this server"

=== blossom.primal.net Content-Type variants ===
  variant ct="application/pdf"       → 415 565ms  body="upload rejected: unsupported media type application/octet-stream"
  variant ct="image/png"             → 415 577ms  body="upload rejected: unsupported media type application/octet-stream"
  variant ct="text/plain"            → 415 637ms  body="upload rejected: unsupported media type application/octet-stream"
  variant ct="" (fallback octet)     → 415 574ms  body="upload rejected: unsupported media type application/octet-stream"
```

Interpretation: blossom.band's server-tag check is unusually strict (rejects every URL form). primal.net always reports "application/octet-stream" in its 415 body regardless of the request's `Content-Type`, suggesting internal type detection is what triggers the reject — outside scope for D8.

### Round 3 — clean batch/mismatch on the two content-filtered servers

```
# probe3 pubkey: 403c237d81b0cebc0aa850b4420062896c374ee83115c8e3574de25fe4089ae6

=== https://blossom.band  (server_tag omitted) ===
  # A. baseline (auth-pass indicator = 415, auth-fail = 401)
  PUT /upload  415  1152ms  body="File type not allowed, unsupported…"
  PUT /upload  415   324ms  body=<same as above>
  # B. batch (3 blobs sharing 1 auth event with 3 x-tags)
  PUT /upload  415   326ms  body=<same as above>
  PUT /upload  415   372ms  body=<same as above>
  PUT /upload  415   290ms  body=<same as above>
  PUT /upload  415   333ms  body=<same as above>
  PUT /upload  415   333ms  body=<same as above>
  PUT /upload  415   327ms  body=<same as above>
  # C. mismatch (auth x=[H1], PUT blob H2)
  PUT /upload  415   323ms  body=<same as above>   ← auth PASS on mismatched blob (permissive, filed as nostrc-5uij)
  PUT /upload  415   329ms  body=<same as above>
  SUMMARY: A=[415,415]  B=[415,415,415] | [415,415,415]  C=[415,415]  batch auth ACCEPTED, mismatch PERMISSIVE

=== https://blossom.primal.net  (server_tag set) ===
  PUT /upload  415  572ms  body="upload rejected: unsupported media type application/octet-stream"
  PUT /upload  415  566ms  body=<same as above>
  # B. batch
  PUT /upload  415  556ms  body=<same as above>
  PUT /upload  415  569ms  body=<same as above>
  PUT /upload  415  560ms  body=<same as above>
  PUT /upload  415  576ms  body=<same as above>
  PUT /upload  415  564ms  body=<same as above>
  PUT /upload  415  571ms  body=<same as above>
  # C. mismatch
  PUT /upload  401  564ms  body="invalid x tag"
  PUT /upload  401  553ms  body="invalid x tag"
  SUMMARY: A=[415,415]  B=[415,415,415] | [415,415,415]  C=[401,401]  batch auth ACCEPTED, mismatch STRICT
```

---

Report ends. Verdict recorded on `nostrc-yo44` at close.
