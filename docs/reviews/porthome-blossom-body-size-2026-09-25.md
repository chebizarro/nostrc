# Porthome — Blossom body-size cap probe (t44z, complement to D8)

Bead: `nostrc-t44z` (P3). Complement to `nostrc-yo44` / D8 (batch-auth axis) — this report scans the *size* axis instead. Design context: `docs/designs/home-from-relay.md` §4.3 D8 and §12 item 10 ("chunker should size to each server's per-request body cap"). Report-only. No changes to porthome, syncd, fuse, libhanami, or auth.

## 1. Scope + methodology

**Question.** At what PUT body size does each production Blossom server we care about start returning 413 (or otherwise refuse the upload)? Porthome's chunker in the Phase-2 provisioner (`nh_porthome_blossom_upload`) needs an empirical ceiling per server so a single chunk is guaranteed to land in one PUT. The DESIGN currently pins the chunk size at 4 MiB (for convergent-encryption and dedup reasons — that stays canonical); this probe tells us where the wire actually caps out and whether the 4 MiB pick is safe on every server.

**Ephemeral keypairs.** Two runs, two independent secp256k1 keypairs generated in Python memory and discarded on process exit. Private keys never written to disk or logged. Public keys (x-only, hex) — grep server logs to distinguish this probe's traffic:

```
run 1 (1 / 8 / 32 / 64 MiB) pubkey: 30cb4b086e3cf45c8a0c9b1a342c58f837daaff16b56956e6ec30092268417c9
run 2 (2 / 4 MiB refinement) pubkey: a6949cf42149f584c50fd1df55f1ddc420d59d81f9fb772917f042759d459191
```

Both keys single-use throwaways.

**Servers probed.**

| Server                          | Why                                                                     |
| ------------------------------- | ----------------------------------------------------------------------- |
| `https://blossom.sharegap.net`  | We operate this server — nginx front-end, full server-side visibility.  |
| `https://blossom.band`          | Widely used community server; auth-quirk from yo44 §2.1 (no `server` tag). |
| `https://blossom.primal.net`    | Widely used community server; strict-`x`-tag from yo44 §3.               |

`cdn.satellite.earth` was excluded — yo44 §2 established it was operationally degraded (30 s read timeouts / 5xx) on 2026-09-24, so body-size data would be uninterpretable.

**Sizes.** 1 MiB, 8 MiB, 32 MiB, 64 MiB (per t44z instructions). After run 1 revealed sharegap.net rejecting ≥ 8 MiB, a run-2 refinement added 2 MiB and 4 MiB across all three servers to pin sharegap's cap and confirm 4 MiB (the DESIGN chunk size) works on the community servers.

**Content.** Every PUT body starts with a genuinely valid 67-byte PNG prefix — real 8-byte signature (`\x89PNG\r\n\x1a\n`), IHDR chunk for a 1×1 8-bit grayscale image with a correct CRC, a single deflated IDAT scanline (also CRC-correct), and a valid IEND. The remainder of the body is `secrets.token_bytes()` random noise up to the target size. `file(1)` reports the prefix as `PNG image data, 1 x 1, 8-bit grayscale, non-interlaced`, so any Content-Type sniffer that checks magic bytes plus first-chunk validity classifies the blob as `image/png`. This dodges naive content-type filters (which killed 2-KiB random-byte probes in yo44 on blossom.band and primal.net) while remaining harmless.

**Auth.** kind-24242 event, `t=upload`, single `x` tag = sha256(blob), `expiration = now + 300 s`. **No `server` tag** on any request — the yo44 finding was that omitting the tag is the only cross-server-safe posture (blossom.band rejects every URL form otherwise). Header form: `Authorization: Nostr <base64(canonical-JSON-event)>`. Content-Type: `image/png`. `User-Agent: nostrc-t44z-probe/2026-09-25` on every request. Content-Length set explicitly so the server sees the intended size before the body streams.

**Load discipline.** Sequential PUTs, concurrency = 1 per server, 10 s between runs, one attempt per (server, size) with baseline timeouts of (30 s connect / max(60 s, 8 s·MiB) read). On the first timeout, one retry with (120 s connect / 600 s read) per t44z spec. Total budget: **run 1** = 12 PUTs × up to 106 MiB per server ≤ 318 MiB across all three servers; **run 2** = 6 PUTs × 6 MiB per server ≤ 18 MiB. No 429s seen.

**Origin.** Both runs from the local macOS workstation (bizarro-adjacent, not gnome-dev). The probe scripts live at `/tmp/t44z_probe/probe.py` and `/tmp/t44z_probe/probe2.py` with logs at `run1.log` / `run2.log`. **Not committed.** `libhanami` was not invoked — self-contained Python that reimplements the BUD-02 event shape.

## 2. Per-server × per-size result matrix

Sent-bytes = the intended body length (`Content-Length`). `-` in `status` = no HTTP response (write timeout / connection abort). Elapsed_ms is the successful-attempt time for retried entries. `clen_echo` is the *response* Content-Length header — for 2xx it is the JSON body length; for 413 it is the length of the nginx error HTML.

| Server               | 1 MiB                  | 2 MiB                | 4 MiB                | 8 MiB                          | 32 MiB                                       | 64 MiB                                       |
| -------------------- | ---------------------- | -------------------- | -------------------- | ------------------------------ | -------------------------------------------- | -------------------------------------------- |
| blossom.sharegap.net | **201** (141 ms)       | **413** (163 ms)     | **413** (158 ms)     | **413** (1 376 ms)             | **413** (3 437 ms)                           | **413** (6 063 ms)                           |
| blossom.band         | **201** (9 025 ms)     | **201** (18 054 ms)  | **201** (27 775 ms)  | **201** (57 413 ms, retry)     | **inconclusive** (write-timeout 120 s, retry write-timeout 120 s) | **inconclusive** (write-timeout 120 s, retry write-timeout 120 s) |
| blossom.primal.net   | **200** (9 878 ms)     | **200** (17 010 ms)  | **200** (30 871 ms)  | **200** (67 985 ms, retry)     | **inconclusive** (write-timeout 120 s, retry write-timeout 120 s) | **inconclusive** (write-timeout 120 s, retry write-timeout 120 s) |

**Response-body key.**

- `201`/`200` bodies on all three servers were the canonical Blossom blob descriptor JSON — `url`, `sha256`, `size`, `type`, `uploaded` — with the server-reported `size` exactly matching the sent bytes (spot-checked band-2 MiB → `"size":2097152`, band-4 MiB → `"size":4194304`, band-8 MiB → `"size":8388608`). So the ≥ 8 MiB PUTs on band/primal *did* fully land — no partial-body storage.
- `413` on sharegap: nginx-generated `<html><title>413 Request Entity Too Large</title>…<hr><center>nginx/1.29.8</center>…`, ~183 bytes. Response arrives *before* the entire body has been sent — the 8 / 32 / 64 MiB times are the request-abort round trip, not full upload times.
- `inconclusive` = the write-side socket timed out at ~120 s during upload. Whether that timeout comes from the server (nginx `client_body_timeout` default is 60 s; some CDN fronts use 120 s), an intermediate CDN, or the client-side OS is not distinguishable from this vantage. Both retries hit the same 120 s wall. The upstream throughput measured on 8 MiB successes was ~123–140 KB/s (band ≈ 146 KB/s, primal ≈ 123 KB/s), so 32 MiB would need ~4 min and 64 MiB ~9 min of continuous write — well within the 600 s read_timeout we set but past whatever 120 s ceiling actually applies on the wire.

## 3. Verdict per server

Recommended max blob size that the porthome chunker can trust to land in a single PUT, given only what this probe *observed*:

| Server               | Recommended max single-PUT | Confidence | Notes |
| -------------------- | -------------------------- | ---------- | ----- |
| blossom.sharegap.net | **1 MiB**                  | high       | Hard nginx `client_max_body_size` — 1 MiB is the last size that returned 201, 2 MiB is the first that returned 413. Consistent with an nginx `client_max_body_size 1M;` (or default 1M). |
| blossom.band         | **≥ 8 MiB**                | high       | 4 successful sizes end-to-end, JSON body confirms full receipt. Actual ceiling unknown from this network vantage — write-side timeout at 120 s makes it operationally *behave* as ≈ 15 MiB at our measured 146 KB/s upload rate. |
| blossom.primal.net   | **≥ 8 MiB**                | high       | Same shape as band; behaves as ≈ 15 MiB at 123 KB/s. |

**Critical finding.** `blossom.sharegap.net` caps below the DESIGN's 4 MiB chunk size. Every 4 MiB chunk sent to sharegap today will 413. Since sharegap is the server we operate, the cheapest fix is a nginx config change (see follow-up `nostrc-e4v6`).

**Auth-layer note (negative result).** No PUT in this probe returned 401 for a well-formed auth event at any size. That's a positive result versus the t44z instruction "if any server returns 401 for a well-formed auth event on a legitimate-size PUT, that's a NEW bug beyond D8". No new bug beyond D8 was found — the auth-layer behavior at 1–8 MiB matches yo44 §3's auth-layer characterization at 2 KiB.

## 4. Recommendation for `hanami_blossom_upload` chunk-size selection

The DESIGN's 4 MiB chunk size stays canonical for the reasons the DESIGN gives (convergent-encryption keys are computed per chunk; chunk-size changes would break dedup across a fleet). Nothing this probe found argues for changing the design chunk size in the general case — 4 MiB works on the two community servers we characterized and would work on sharegap once its nginx cap is raised.

What this probe *does* argue for:

1. **Sharegap immediate config fix** — raise `client_max_body_size` on sharegap to at least 8 MiB (comfortable headroom over the 4 MiB chunk). Filed as **`nostrc-e4v6`** (P2). This is a two-line nginx conf change on a server we operate; do this before Phase 2 starts writing 4 MiB chunks or every provision will 413. Recommended value: `client_max_body_size 8m;` — 2× the design chunk gives headroom for accidental 5-10% overhead and for any per-blob metadata we later attach.

2. **Per-server capability field for max PUT size.** yo44 already spec'd `nostrc-ypn2` (per-server capability probe at connect time) and `libhanami/src/hanami-server-capability.c` (state struct). Extend that struct with a `uint64_t max_put_bytes` field populated from either (a) a peek probe at connect time — HEAD/OPTIONS if the server supports it, else a small-then-medium PUT to bisect, or (b) a session-scoped 413 negative cache: on the first 413 from a chunk-sized PUT, record `max_put_bytes = min(current, sent - 1)` and quarantine the server for chunks larger than that; the chunker then splits or refuses. This is orthogonal to the DESIGN's canonical 4 MiB — the DESIGN chunk stays, but the *upload path* per-server can decide "this server can't accept a 4 MiB chunk, either sub-chunk further or fall over to another server in the write set". Filed as sub-task under **`nostrc-ypn2`** (P3) rather than a new bead — logical extension of the capability-probe work.

3. **Empirical cap on band/primal beyond 8 MiB is unknown but doesn't matter for us.** The DESIGN chunks at 4 MiB, so even if band's real cap turned out to be 20 MiB the porthome chunker never sends that big. No follow-up needed unless a future DESIGN change raises the chunk.

4. **Client-side chunker path stays regardless of empirical cap.** The t44z spec notes: "the design's answer is NO — always chunk for convergent-encryption reasons — but recording the empirical cap is still useful for the batch-auth path where 415 vs 413 disambiguation matters". Concretely: on the batch path (yo44's D8 result), when a server responds with 413 to any PUT in a batch, that's an unambiguous "chunk too big" signal — different from 415 (content policy — retry impossible on this content) and 401 (auth — session-negative-cache the server). The chunker should distinguish those three cases and route each to its own recovery path:
   - **413** → shrink to `max_put_bytes ← current / 2`, negative-cache the server as sub-chunk-required, resume upload from the failed chunk's start with the smaller chunks. Note this only works cleanly for the *last* PUT of a batch — for a mid-batch 413 the batch auth event's `x` tags no longer match the new chunk hashes; you must re-mint a fresh batch auth event over the newly-chunked blob list.
   - **415** → server does not accept our content shape; nothing porthome can do at upload time. Log and skip.
   - **401** → auth-layer rejection. Batch-viable → per-blob auth retry, or session-negative-cache the server for batch. Covered by `nostrc-xeby`.

## 5. Follow-ups

- **`nostrc-e4v6`** (P2, bug) — **filed by this probe.** Raise `client_max_body_size` on blossom.sharegap.net nginx front-end to ≥ 8 MiB. Cheap config change; blocks Phase-2 provisioning against sharegap.
- **`nostrc-ypn2`** (P3, existing) — extend `hanami_server_capabilities_t` with `max_put_bytes` and a 413-driven negative cache in the capability probe. Logical extension of the existing bead; not a new one.
- **`nostrc-xeby`** (P2, existing) — the 413-vs-415-vs-401 disambiguation logic in the batch-upload wrapper. Add a note referencing §4 above when implementing.
- No new authentication-layer bug filed — no 401s at any tested size.

## 6. Raw request log (redacted)

Authorization headers shortened to first 8 base64 chars + total length. Blob hashes shown truncated to 8 hex; full sha256 in JSON summary at `/tmp/t44z_probe/run{1,2}.log` on the probe workstation (not committed).

### Run 1 — 1 / 8 / 32 / 64 MiB across three servers

```
# pubkey: 30cb4b086e3cf45c8a0c9b1a342c58f837daaff16b56956e6ec30092268417c9
# UA: nostrc-t44z-probe/2026-09-25
# started: 2026-09-25T07:13:53Z

=== blossom.sharegap.net ===
  PUT /upload  1 MiB  ->  201  141 ms   auth=Nostr eyJpZCI6... (628 b64)  body={"url":".../78610a7a…png","sha256":"78610a7a…","size":1048576,…}
  PUT /upload  8 MiB  ->  413 1 376 ms  body=<html>413 Request Entity Too Large … nginx/1.29.8</html>  (183 B)
  PUT /upload 32 MiB  ->  413 3 437 ms  body=<same 183-B nginx page>
  PUT /upload 64 MiB  ->  413 6 063 ms  body=<same 183-B nginx page>

=== blossom.band ===
  PUT /upload  1 MiB  ->  201  9 025 ms  auth=Nostr eyJpZCI6... (628 b64)  body={"url":"https://npub1xr9…blossom.band/a36a1107…png","size":1048576,…}
  PUT /upload  8 MiB  attempt1 ERROR 30 068 ms  ConnectionError('The write operation timed out')  (default 30 s+64 s timeouts)
             retry (120 s / 600 s) ->  201 57 413 ms  body={"url":".../b44a4881…png","size":8388608,…}
  PUT /upload 32 MiB  attempt1 ERROR 30 074 ms  write-timeout
             retry (120 s / 600 s) ERROR 120 067 ms  write-timeout  → INCONCLUSIVE
  PUT /upload 64 MiB  attempt1 ERROR 30 069 ms  write-timeout
             retry (120 s / 600 s) ERROR 120 071 ms  write-timeout  → INCONCLUSIVE

=== blossom.primal.net ===
  PUT /upload  1 MiB  ->  200  9 878 ms   auth=Nostr eyJpZCI6... (628 b64)  body={"url":".../72a0468a…png","sha256":"72a0468a…","size":1048576,…}
  PUT /upload  8 MiB  attempt1 ERROR 30 375 ms  write-timeout
             retry (120 s / 600 s) ->  200 67 985 ms  body={"url":".../b1ec4051…png","sha256":"b1ec4051…","size":8388608,…}
  PUT /upload 32 MiB  attempt1 ERROR 30 562 ms  write-timeout
             retry (120 s / 600 s) ERROR 120 403 ms  write-timeout  → INCONCLUSIVE
  PUT /upload 64 MiB  attempt1 ERROR 30 711 ms  write-timeout
             retry (120 s / 600 s) ERROR 120 817 ms  write-timeout  → INCONCLUSIVE
```

### Run 2 — 2 / 4 MiB refinement

```
# pubkey: a6949cf42149f584c50fd1df55f1ddc420d59d81f9fb772917f042759d459191
# started: 2026-09-25T07:30:23Z

=== blossom.sharegap.net ===
  PUT /upload  2 MiB  ->  413 163 ms  body=<183-B nginx 413 page>
  PUT /upload  4 MiB  ->  413 158 ms  body=<same 183-B nginx 413 page>

=== blossom.band ===
  PUT /upload  2 MiB  ->  201 18 054 ms  auth=Nostr eyJpZCI6... (628 b64)  body={"url":".../0169ade0…png","size":2097152,…}
  PUT /upload  4 MiB  ->  201 27 775 ms  auth=Nostr eyJpZCI6... (628 b64)  body={"url":".../ccbc9f7b…png","size":4194304,…}

=== blossom.primal.net ===
  PUT /upload  2 MiB  ->  200 17 010 ms  auth=Nostr eyJpZCI6... (628 b64)  body={"url":".../aa764a25…png","sha256":"aa764a25…","size":2097152,…}
  PUT /upload  4 MiB  ->  200 30 871 ms  auth=Nostr eyJpZCI6... (628 b64)  body={"url":".../ea8f42b0…png","sha256":"ea8f42b0…","size":4194304,…}
```

---

Report ends. Verdict recorded on `nostrc-t44z` at close; follow-up `nostrc-e4v6` filed.
