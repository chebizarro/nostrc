# porthome — two-machine §9.4 demo (jmx0) — 2026-09-25

**Bead:** `nostrc-jmx0` (P4) — porthome: two-machine live §9.4 demo
(push on A, pull on B).
**Design reference:** `docs/designs/home-from-relay.md` §9.4 acceptance.
**Prior evidence:**
- `docs/reviews/porthome-89rj-live-e2e-2026-09-25.md` Part 2 — single-VM
  push+pull round-trip green against `blossom.sharegap.net`; documented
  `/etc/hosts` workaround for the LAN-private edge address.
- `docs/reviews/porthome-blossom-content-type-2026-09-25.md` — bpum's
  Content-Type resolver landed but empirical evidence pinned the
  band/primal 415 as body-sniffer-driven, not header-driven.
- `docs/reviews/porthome-blossom-shim-2026-09-25.md` (this session) —
  PNG-shim primitives + pusher/fetcher wiring landed; live-probe
  validation deferred.

## 1. What the bead asked for

A byte- and name-identical portable-home round-trip between two
separate machines:

- Machine A publishes a home (kind-30078 pointer + encrypted
  chunks) against a Blossom server set with `min_replication>=2`.
- Machine B (separate host, distinct filesystem, distinct network
  identity) pulls the same account, materialises the tree, and
  `diff -r` on the two staging dirs is silent.

## 2. Why it is still blocked

### 2.1 Server-set is single-replica-viable only

The design's default `min_replication=2` posture requires ≥ 2 Blossom
servers that accept encrypted-random-bytes uploads. Today:

| Server                       | Status (2026-09-25)                                     |
| ---------------------------- | ------------------------------------------------------- |
| `blossom.sharegap.net`       | ✅ Accepts random bytes; 1 MiB nginx cap (nostrc-e4v6). |
| `blossom.band`               | ❌ 415 — nostr.build content policy on random bytes.    |
| `blossom.primal.net`         | ❌ 415 — internal body-sniffer classifies as octet-stream. |
| `cdn.satellite.earth`        | ⚠ Operationally degraded on 2026-09-24 (yo44 §2, timeouts / 5xx). |

Options to unlock ≥ 2 permissive servers:

- (Ax) Locate a community-run Blossom server that accepts
  `application/octet-stream` bodies. Requires a probe pass from the
  lab host with the yo44 tooling.
- (Bx) Enable the PNG shim (see `porthome-blossom-shim-2026-09-25.md`)
  and empirically confirm blossom.band / blossom.primal.net accept
  shimmed blobs. Then the default set is 3 servers (sharegap
  raw, band shim, primal shim) with `min_replication=2` achievable.
  Code shipped this session; live probe deferred.
- (Cx) Operator-side allowlist on blossom.band / blossom.primal.net
  for a specific Content-Type we advertise. Requires an operator
  conversation; not on us.

### 2.2 Sharegap LAN address + SSRF pre-check

`blossom.sharegap.net` resolves to `192.168.40.104` on the sharegap
LAN (split-horizon DNS). `nostr-home-fetch`'s SSRF pre-check
(nostrc-9k4g / ww50) refuses any Blossom host whose resolved address
is not public-unicast. For 89rj Part 2 we worked around this by
pointing `/etc/hosts` at the public Cloudflare edge
(`172.67.144.42`) for the pull half of the run. That is safe as an
ad-hoc lab affordance but is not a production posture: a real
two-machine demo needs either

- a Blossom server with a genuinely public IP (not split-horizon), OR
- a documented lab-side DNS convention that keeps the SSRF pre-check
  honest (e.g., a dedicated public-DNS override file signed by the
  operator), OR
- an explicit `--allow-lan-blossom` opt-in on the fetch helper.

None of these three are on the roadmap right now, and adding one is
out of scope for this bead — it belongs to the packaging / lab-CI
story.

## 3. What we tried this session

- Landed bpum plumbing (Content-Type resolver, per-server override
  + env var + setter) — did NOT unlock band/primal, per yo44 evidence.
- Landed **PNG-shim primitives + pusher/fetcher wiring** under
  `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1` — unit tests pass; live-probe
  validation deferred.
- Did NOT run live probes against blossom.band / blossom.primal.net
  from this worktree: this session runs on macOS/darwin without
  Python+`secp256k1` set up and without direct reachability to the
  lab host `bizarro@192.168.64.3` where yo44's probe scripts live.

## 4. What would close jmx0

The load-bearing step is a live-probe pass from the lab host:

1. SSH to `bizarro@192.168.64.3`.
2. Copy `/tmp/yo44_probe/probe.py` (or a fresh clone of the same
   pattern) into `/tmp/yo44_probe_shim/probe4.py`.
3. Add a helper that wraps a 1 KiB random payload in the PNG shim
   (byte layout in `libhanami/include/hanami/hanami-blossom-shim.h`):
   - PNG signature + IHDR chunk (30 bytes fixed) + IDAT header
     (8 bytes, length field = 1024) → 41-byte prefix.
   - Followed by 1024 bytes `os.urandom`.
   - `sha256(shim||random)` becomes the blob id in the auth event.
4. PUT against `blossom.band` (omitting `server` tag per yo44 Round 3)
   and `blossom.primal.net`. Record HTTP status.

**Expected outcomes:**

- `2xx` on both → jmx0 UNBLOCKED. Set
  `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1` in the pusher's environment and
  run the two-machine demo against `min_replication=2` with either
  {sharegap raw, band shim} or {sharegap raw, primal shim}.
- `415` on both with a fresh body message → the sniffer looks deeper
  than the shim's first-chunk-header framing. Two fallbacks:
  - Extend the shim to a fuller PNG (fake deflate stream + IEND) —
    another shim-side pass on this bead.
  - Fall back to Option Ax (find a permissive third-party server).
- `4xx` other than 415 → auth or size regression; investigate.

## 5. Recommended path forward

- Track shim live-probe as a spinoff bead (filed this session).
- Track "find a permissive third-party Blossom server" as a
  separate low-priority bead — Option A in parallel with Option Bx.
- Keep jmx0 open until at least one of Ax / Bx flips the block.

Once either unblocks, the two-machine demo transcript belongs in
this file (§6, TBD) — placeholder headings below.

## 6. Two-machine transcript (placeholder — pending unblock)

- 6.1 Enroll on machine A.
- 6.2 Populate the home tree + baseline sha.
- 6.3 Push from A with shim (or the newfound permissive server).
- 6.4 scp the account file to machine B.
- 6.5 Pull from B into `/tmp/nhp-b-pulled/`.
- 6.6 `diff -r` on A's tree vs B's pulled tree → silent.
- 6.7 sha256 of the sorted content-hash sets on both sides → equal.

## 7. Close criteria

jmx0 closes when §6's transcript is filled with a green run
against `min_replication>=2`. Until then, single-server sharegap
demos (89rj Part 2) are the current best-effort; treat them as
Phase-2 acceptance, not §9.4 acceptance.

---

## Part 2 — resolved by hy3e (push side) — 2026-09-25 evening

hy3e ran the live probe with the PNG shim (see
`docs/reviews/porthome-hy3e-shim-live-probe-2026-09-25.md`).
Primal accepts shimmed random-bytes uploads (415 → 200) with a
byte-identical round-trip; sharegap continues to accept both raw and
shim. That gives us the two-permissive-server posture the §9.4 demo
needs for `min_replication=2`. band remains inaccessible (full-PNG
decode on ingest) and is out of the community set for v1.

We built `nostr-homed-provision` + `nostr-home-fetch` on
`bizarro@192.168.64.3` from a fresh clone at commit `9d24938` (the
bpum shim landing). A single surgical fix to
`gnome/nostr-homed/src/porthome/nostr-homed-provision.c` was needed
to unblock the gcc-13 `-Werror=unused-result` on `chown` / `fchown`
/ `system` (three call sites, replaced `(void)f(...)` with
`if (f(...)) { /* ignored */ }`). The edit is behaviour-preserving
and lives in the hy3e worktree diff.

### 6.1–6.3 Enroll + populate + push (green)

```
$PROV enroll --relay wss://relay.sharegap.net --relay wss://relay.damus.io \
             --blossom https://blossom.sharegap.net \
             --blossom https://blossom.primal.net --out-dir $NHDIR

# populate 3 files + 1 MiB blob; sha256 of sorted content set = 268d…fd12
NOSTR_HOMED_BLOSSOM_PNG_SHIM=1 $PROV push \
  --account-file $NHDIR/*.account.json --home $HOMEA \
  --chunk-size 262144 --min-replication 2 --json
```

Push output:

```
push: snapshot /tmp/hy3e_demo/homeA-… → 9 entries (0 skipped)
push: encrypted 7 chunks → 7 Blossom blobs
push: server[0] https://blossom.sharegap.net    uploaded=7 bytes=1049122 failed=0 fell_back=0
push: server[1] https://blossom.primal.net      uploaded=7 bytes=1049122 failed=0 fell_back=0
push: min_replication=2 required, full-replica servers=2
push: relay wss://relay.sharegap.net OK
push: relay wss://relay.damus.io OK
{"gen":0,"chunks":7,"bytes":1048632,"relays_ok":2,"full_replicas":2,
 "event_id":"f8b078e103a422b19655ce30e6af47311fc30d5a95172393272779c327670f16"}
```

Both community servers accepted all 7 shimmed chunks (bytes on the
wire per server: 1 049 122 = 7 × { 80, 90, 96, 262 214 × 4 } — the
262 214 is 262 144 payload + 12 nonce + 16 tag + 1 wire-version + 41
shim). Independent `GET /list/<pubkey>` on sharegap and `HEAD` on
primal confirm all 7 blob hashes, byte-identical between the two
servers.

**§9.4 push-side acceptance is achieved: `min_replication=2` with two
distinct community servers, byte-verified.**

### 6.4–6.5 Pull-side blocked by manifest wiring bug

`scp` of the account file to "B" (simulated as a distinct dest dir
on the same host — see §7 caveat); pull run with the same
`NOSTR_HOMED_BLOSSOM_PNG_SHIM=1` flag against the same account file
with `blossom.sharegap.net` stripped (its LAN IP fails the
`nostr-home-fetch` SSRF pre-check unconditionally — `--allow-insecure`
does NOT bypass the resolved-address check, it only permits `http://`
schemes; §7).

```
NOSTR_HOMED_BLOSSOM_PNG_SHIM=1 $PROV pull \
  --account-file .../machineB.account.json --dest $HOMEB \
  --helper $FETCH --relay-timeout-ms 30000
```

Progress:
```
{"bytes":0,"files":0,"phase":"manifest"}
[nostr_subscription_fire] REQ ... kinds=[30078] authors=[<pubkey>] #d=[nostr-homed.home.v1:personal]
[RELAY_POOL] Subscription 1 received 1 events before EOSE
{"bytes":1670,"files":0,"phase":"decode"}
# then exits 71 (NETWORK_FAIL / LIMITED)
```

Debug prints inserted temporarily in `nh_porthome_blossom_fetch`
(reverted before commit) revealed the failure mode: **the fetcher
was asking for chunk hashes that do not exist on either Blossom
server**. Example: `11150cd7545f…` requested repeatedly, never on
sharegap's `/list/<pubkey>` output nor on primal `HEAD`.

Cross-check: the 7 chunks actually stored (all present on both
servers) hash to `02493981`, `418ff44f`, `89be7a03`, `8c68668f`,
`b0adc550`, `be3ad2c1`, `e7b73c11` — none of which is `11150cd7…`.
The manifest is asking for the pre-shim ciphertext hash. The Blossom
URL uses the post-shim `sha256(shim||ct)` hash. Mismatch by design.

**Root cause** (nostr-homed-provision.c cmd_push, around line 1305):

```c
uint8_t *ct = NULL; size_t ctl = 0; uint8_t addr[32];
int erc = nh_porthome_encrypt_chunk(home_key, pbuf, want, &ct, &ctl, addr);
// ^ addr is set to sha256(ct) here — PRE-shim.
...
batch[n_blobs].bytes = ct;
batch[n_blobs].len   = ctl;
batch[n_blobs].expected_sha256_hex = NULL;
...
memcpy(chs[ci].sha256, addr, 32);   // manifest chunk row: sha256(ct)
```

Later, `nh_porthome_blossom_upload_batch` with the shim path (bpum)
correctly computes `sha256(shim||ct)` and stores it in
`blobs[i].sha256_hex`. But by then the manifest is already built
around `addr` (the pre-shim hash) and nothing propagates the new
hash back. The manifest is sealed, the pointer is published — pull
looks up the pre-shim hash on Blossom → 404 → exit 71.

Filed as `nostrc-<new3>` (P2) — "porthome: fix shim manifest
chunk_hash to be sha256(shim||ct), not sha256(ct)". Blocks jmx0 and
keeps bpum open pending the fix.

### 6.6–6.7 diff + sha256 — DEFERRED

Cannot be run until the pull side actually materialises files. Once
the manifest wiring is fixed, this section becomes trivial to close.

## 7. Caveats & operator notes carried forward

1. **A/B on one host, not two.** The demo ran with two distinct
   `$HOME`-style directories on `bizarro@192.168.64.3`
   (`/tmp/hy3e_demo/homeA-…` and `/tmp/hy3e_demo/homeB-…-pulled`).
   The account file was copied via `cp` rather than `scp`, but the
   receiving side reads it with no host affinity — the compromise is
   documented per the task's guidance and does not affect the
   push/pull semantics under test.

2. **SSRF pre-check + `--allow-insecure`.** In the fetch helper
   `nostr-home-fetch` the `--allow-insecure` flag only affects
   scheme validation (allowing `http://`). The
   `ssrf_precheck_servers` gate that walks each Blossom host's
   resolved addresses and refuses private/link-local/loopback IPs
   runs UNCONDITIONALLY and is what blocks `blossom.sharegap.net`
   (LAN IP `192.168.40.104` from this VM). For the pull half we
   dropped sharegap from B's blossom list and relied on primal
   alone; the push had already written every chunk to both servers.
   A production two-machine demo needs either (a) sharegap reachable
   over a genuinely public IP or (b) an `--allow-lan-blossom`
   opt-in with a documented risk statement — same story as 89rj Part
   2, no change this session.

3. **Damus + sharegap relay for pointer publication.** We published
   the pointer to `wss://relay.sharegap.net` (LAN — writes accepted)
   AND `wss://relay.damus.io`. Damus.io retained the pointer through
   the demo window; we cross-verified with a direct WebSocket REQ
   from a second Python client. Nos.lol was 502 during the run,
   relay.nostr.band was reachable but not needed.

## 8. Close criteria — REFINED

jmx0 closes when:

1. `nostrc-<new3>` (shim manifest hash fix) is closed, AND
2. A rerun of §6.1–6.7 fills in §6.6 with `diff -r` empty and §6.7
   with equal sha256sums.

Both blockers are now concrete and small. Until then, single-server
sharegap demos (89rj Part 2) remain the current best-effort.
