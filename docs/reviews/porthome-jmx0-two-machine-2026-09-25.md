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
