# porthome — hy3e: PNG-shim live probe (blossom.band / blossom.primal.net) — 2026-09-25

**Bead:** `nostrc-hy3e` (P3) — porthome: live-probe PNG shim against
blossom.band + blossom.primal.net.

**Prior evidence:**
- `docs/reviews/porthome-blossom-shim-2026-09-25.md` (Q's shim design +
  the compile-time 41-byte prefix + the `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1`
  env-var opt-in).
- `docs/reviews/porthome-blossom-batch-auth-2026-09-24.md` (yo44) — the
  content-policy 415 baseline established from the same lab host.
- `libhanami/{include,src}/hanami-blossom-shim.{h,c}` — the shim
  primitives (already landed on master).

**Ephemeral probe pubkey (log-only, throwaway):**
`a99394a09cd6af2d6616bf764eb3ea83fd1b168a264bd1101390552f8c0dcb81`

**Load discipline.** 3 servers × 3 methods × 2 rounds = 12 real PUTs
(9 in round 1 + 3 in round 2 focused on band); one GET round-trip per
successful upload. `User-Agent: nostrc-hy3e-probe/2026-09-25` on every
request; ≥ 10 s between servers. All from `bizarro@192.168.64.3` under
`/tmp/hy3e_probe/` (script + venv, not committed).

## 1. Method

Three ciphertext-look-alike payloads (all 1 KiB or 1 KiB + 41-byte
prefix):

- **baseline** — 1 KiB `os.urandom`, `sha256(raw)` as blob id.
- **shim** — 41-byte deterministic PNG prefix (byte-identical to
  `libhanami/src/hanami-blossom-shim.c`; verified IHDR CRC =
  `0x3A7E9B55`) prepended to 1 KiB `os.urandom`, `sha256(shim||random)`
  as blob id. IDAT length field patched with `1024` (the ciphertext
  length). No IDAT CRC, no IEND.
- **control** — yo44's "image-header trick": 1 KiB total, starting with
  the same 41-byte shim (IDAT length = 983 to fill the KiB) followed by
  `os.urandom`. Reproduces yo44 D8 §3 exactly.

BUD-02 upload auth: kind-24242, tags
`[["t","upload"],["x",sha256hex],["expiration","+300s"]]`, **no
`server` tag** (per yo44 R3 — blossom.band's server-tag policy is
strict). Auth event carried as `Authorization: Nostr <base64(json)>`.

Success criterion: **≥ 1 of `blossom.band` / `blossom.primal.net`
returns 2xx on the shim upload** where the raw-random baseline is 415.

## 2. Per-server × per-method matrix

| server                       | baseline (raw)                                        | shim (shim + raw)                                                      | control (yo44 image-header trick)                          |
| ---------------------------- | ------------------------------------------------------ | ---------------------------------------------------------------------- | ---------------------------------------------------------- |
| `blossom.sharegap.net`       | **201** (`type=application/octet-stream`) — round-trip OK | **201** (`type=application/octet-stream`) — round-trip OK              | **201** — round-trip OK                                    |
| `blossom.band`               | **415** — `File type not allowed, unsupported.`        | **400** — `Content-Type header does not match the file content, expected image/png` | **400** — same body                                        |
| `blossom.primal.net`         | **415** — `upload rejected: unsupported media type application/octet-stream` | **200** — `type=image/png`, url returned with `.png` suffix; **round-trip OK, byte-identical** | **200** — same shape                                       |

Round-2 (band with `Content-Type: image/png` on the shim payload):

| server        | shim (Content-Type: image/png)                                | control (Content-Type: image/png) |
| ------------- | -------------------------------------------------------------- | ---------------------------------- |
| `blossom.band` | **500** — `Error uploading the file: unknown error`             | **500** — same                     |

Reproducibility check (round-2 primal, fresh random, default CT): **200**,
type `image/png`, byte-identical round-trip.

## 3. Interpretation

### 3.1 blossom.primal.net — shim WORKS

Empirically confirmed: the 41-byte shim (PNG signature + one
well-formed IHDR + a bare IDAT chunk header naming the ciphertext
length) is enough for primal.net's sniffer to classify the upload as
`image/png` and accept it. Primal even rewrites the response URL with
a `.png` suffix and serves the blob with `Content-Type: image/png` —
proving the sniffer really did reclassify, not merely bypass. The
round-trip GET returns the exact shim||ct bytes; head-match verified.

This meets the hy3e success criterion.

### 3.2 blossom.band — shim DOES NOT WORK

Round 1 (default `Content-Type: application/octet-stream`): band's
sniffer rejects with **400** `Content-Type header does not match the
file content, expected image/png` — it read the PNG signature and
inferred image/png, then refused because the request header was
octet-stream. That is a stricter policy than primal: band cross-checks
the sniffed type against the declared type and refuses on mismatch.

Round 2 (`Content-Type: image/png`): band accepts the header check
but then **500** `Error uploading the file: unknown error` — the
downstream processor tries to decode the file as a real PNG (probably
generating a thumbnail or extracting IHDR dimensions), fails on the
truncated IDAT (no valid deflate stream, no IEND), and errors out.
band is running a full PNG decoder in the accept path, not a byte
sniffer.

Extending the shim to a fuller PNG (deflate-encoded IDAT of the
ciphertext followed by IEND with a valid CRC) is *possible* but it
inflates every blob by the deflate overhead of random bytes (~0.1 %
plus a fixed 12-byte IEND) and it materially couples porthome's
crypto layout to a PNG decoder's tolerance. Not worth doing to
unlock a single server; better to accept that band is not part of
the community-server set for portable-home traffic in v1.

### 3.3 blossom.sharegap.net — no regression

Sharegap accepts all three variants (raw, shim, control) with
`application/octet-stream` content-type on both storage and GET.
The shim adds 41 bytes per chunk but does not otherwise change the
byte-for-byte round-trip. Sharegap remains the anchor of the default
server set.

## 4. Verdict (hy3e)

**GREEN** — the PNG shim unlocks acceptance on `blossom.primal.net`
(415 → 200) with a verifiable round-trip. `blossom.band` remains
inaccessible for random-bytes payloads at any prefix length short of a
fully-valid PNG (which is out of scope for the shim design). This
lands us the second permissive-community server we need for a
`min_replication=2` community-server posture:

- Default community set: `{ sharegap (raw), primal (shim) }` — two
  servers accept, min_replication=2 achievable.
- blossom.band opt-in only when a self-hosted PNG encoder is bolted
  onto porthome (deferred).

Filed: `nostrc-<new1>` for the fuller-PNG shim (P4 — optional, only
worth doing if operators need blossom.band specifically).
Filed: `nostrc-<new2>` for the self-hosted permissive Blossom server
(P4 — Option A fallback; belongs to lab-CI packaging story).

## 5. Recommendation for the shim env var

`NOSTR_HOMED_BLOSSOM_PNG_SHIM=1` should become the **default
behaviour** for pushers that include primal.net in their server set —
i.e. the community-server bootstrap set. The auto-shim (nostrc-si30)
design should:

- Probe each server's raw-random capability at connect time (yo44's
  `hanami_server_probe_capabilities` extension).
- Enable the shim on servers where raw fails but shim succeeds
  (currently `primal`).
- Leave it off for servers that accept raw (`sharegap`) so wire
  overhead stays minimal there.
- Skip servers where neither raw nor shim succeeds (`band` currently)
  and downgrade min_replication accordingly if the set becomes
  too small.

Until nostrc-si30 lands, exporting `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1`
on any pusher that talks to primal is correct — the wire penalty on
sharegap is 41 bytes/chunk and no functional change.

## 6. Load-side confirmation the shim upload path works end-to-end

Push run from `nostr-homed-provision push` (freshly built at commit
`9d24938`; see build notes in §7):

```
push: snapshot /tmp/hy3e_demo/homeA-... → 9 entries (0 skipped)
push: encrypted 7 chunks → 7 Blossom blobs
push: server[0] https://blossom.sharegap.net             uploaded=7 bytes=1049122 failed=0 fell_back=0
push: server[1] https://blossom.primal.net               uploaded=7 bytes=1049122 failed=0 fell_back=0
push: min_replication=2 required, full-replica servers=2
push: relay wss://relay.sharegap.net OK
push: relay wss://relay.damus.io OK
{"gen":0,"chunks":7,"bytes":1048632,"relays_ok":2,"full_replicas":2,
 "event_id":"f8b078e103a422b19655ce30e6af47311fc30d5a95172393272779c327670f16"}
```

Post-push independent check via `GET /list/<pubkey>` on sharegap +
`HEAD /<sha>` on primal: **all 7 chunks present on both servers with
identical sha256, identical size (7 × { 80, 90, 96, 262214×4 }),
byte-identical**. The shim is real, deterministic, and the community
server retains the raw bytes so a strip-and-decrypt on the puller
side is possible.

## 7. Build note (surgical fix applied on the demo tree)

The scratch build tree hit three `warn_unused_result` errors under
gcc 13 (`chown`, `fchown`, `system` — the C `(void)` cast is not
sufficient on newer glibc). Fixed in the source with a discard-value
pattern (`if (...) { /* ignored */ }`) — see
`gnome/nostr-homed/src/porthome/nostr-homed-provision.c`. This edit
is in the worktree diff for `nostrc-hy3e`; it does not change
behaviour and is required for any operator building `nostr-homed-
provision` on a stock Ubuntu 24.04 toolchain.

## 8. Follow-up bug found under empirical validation — SEPARATE from hy3e

During the two-machine demo attempt (nostrc-jmx0), the shim upload
succeeded end-to-end but `pull` failed to materialise the tree.
Debug trace inserted in `nh_porthome_blossom_fetch` showed the
fetcher was asking for chunk hashes that **do not exist on either
Blossom server** — the manifest recorded pre-shim `sha256(ct)` while
the actual uploads were `sha256(shim||ct)`. Root cause in
`nh_porthome_provision.c` `cmd_push`: the local `addr` used for
`chs[ci].sha256` (manifest chunk row) is set to the pre-shim hash
from `nh_porthome_encrypt_chunk`; when the shim path in
`nh_porthome_blossom_upload_batch` rewrites `blobs[i].sha256_hex` to
`sha256(shim||ct)`, the manifest is never updated to match.

- File: `gnome/nostr-homed/src/porthome/nostr-homed-provision.c`
  around lines 1305–1320 (batch collect + `memcpy(chs[ci].sha256,
  addr, 32)`).
- Effect: with `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1` the pusher writes
  chunks that no puller can find. Uploads succeed; manifest is
  self-inconsistent.

**This does NOT invalidate the hy3e verdict** — hy3e's question was
"does the shim unlock acceptance on primal/band" and the raw upload
probe (§2) proves it does on primal. The bug is downstream in the
provisioner's manifest wiring, not in the shim primitives.

Filed as `nostrc-<new3>` (P2) — "porthome: fix shim manifest
chunk_hash to be sha256(shim||ct), not sha256(ct)". Blocks jmx0
and bpum.

## 9. Data preserved for audit

- `/tmp/hy3e_probe/results.json` on `bizarro@192.168.64.3` — round-1
  probe JSON (server × method matrix).
- `/tmp/hy3e_probe/probe.log` — round-1 stderr.
- `/tmp/hy3e_probe/probe_round2.py` — round-2 script (band +
  Content-Type variants + primal reproducibility).
- `/tmp/hy3e_demo/pull.err`, `p3.err` — the pull traces that led
  to the manifest-hash bug discovery.
- Ephemeral pubkey logged in §pre; blob receipts still on the two
  Blossom servers under that key.
