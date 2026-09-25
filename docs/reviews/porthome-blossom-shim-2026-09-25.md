# porthome — Blossom PNG-shim wrapper (bpum follow-up) — 2026-09-25

**Beads:** `nostrc-bpum` (this pass — Option B path) — porthome:
broaden Blossom content-type acceptance for real chunks.
**Related:** `nostrc-yo44` (D8 BUD-02 batch-auth probe), `nostrc-jmx0`
(two-machine §9.4 demo — still blocked, see §5), `nostrc-89rj` Part 2
(single-VM push+pull with sharegap-only replication).
**Design reference:** `docs/designs/home-from-relay.md` §5.4 (D4 wire
format), `docs/reviews/porthome-blossom-content-type-2026-09-25.md`
§2 (empirical body-sniffer evidence).

## 1. What the follow-up asked for

The previous bpum pass shipped a per-server Content-Type resolver
(`hanami_blossom_client_set_upload_content_type` + env override +
compile-time default) but empirical evidence from `nostrc-yo44` Round 2
told us the two problem servers (`blossom.band`, `blossom.primal.net`)
are body-sniffer-driven, not header-driven — swapping the outgoing
Content-Type header alone cannot unlock either. Their 415 body always
reports `application/octet-stream` regardless of what the request
advertised.

The follow-up asked us to break the impasse. Two paths were on the
table:

- **Option A** — expand the default server set: probe candidate
  Blossom servers and pin one or two that accept random-byte
  encrypted chunks.
- **Option B** — wrap each chunk in a fake-PNG shim that satisfies the
  body sniffer while preserving D4 convergence at the manifest level.

This pass lands the primitives + wiring for **Option B**. Option A
requires live network probes from the lab host with the yo44 signing
tooling; that half is filed as a follow-up bead — see §6. Option B is
shipped as an opt-in via an environment variable so the lab can flip
it on for a two-machine demo without waiting on operator-side allowlist
changes at blossom.band / primal.net.

## 2. The shim — wire format

`libhanami/include/hanami/hanami-blossom-shim.h` defines a
41-byte deterministic PNG prefix:

```
[0..8)   PNG signature   89 50 4E 47 0D 0A 1A 0A
[8..12)  IHDR length     00 00 00 0D                       (13, big-endian)
[12..16) IHDR type       "IHDR"
[16..20) width           00 00 00 01                       (1 pixel)
[20..24) height          00 00 00 01                       (1 pixel)
[24]     bit depth       08
[25]     colour type     00                                (grayscale)
[26]     compression     00
[27]     filter          00
[28]     interlace       00
[29..33) IHDR CRC32      3A 7E 9B 55                       (standard PNG CRC over "IHDR"+body)
[33..37) IDAT length     BE(ciphertext_len)                (varies)
[37..41) IDAT type       "IDAT"
[41..]   ciphertext bytes (raw — sniffers do NOT decompress)
```

Total overhead: **41 bytes per chunk**. No IDAT CRC and no IEND
chunk — a strict PNG parser would reject the file as truncated, but
content-sniffers (libmagic, nostr.build's policy filter, primal's
detector) only walk the signature + first chunk header and pass.
This matches the "PNG header + fabricated IHDR + IDAT chunk with the
raw ciphertext" shape called out in yo44's D8 probe design.

Determinism: the CRC is computed over a fixed body so the whole
shim prefix is a pure function of `ciphertext_len` and the ciphertext
bytes. Identical ciphertext → identical shimmed blob → identical
sha256. **D4 convergence is preserved through the shim** — this is
the load-bearing property.

## 3. What landed in this pass

### 3.1 New module — `libhanami-blossom-shim`

- `libhanami/include/hanami/hanami-blossom-shim.h`
- `libhanami/src/hanami-blossom-shim.c`

Public API:

```c
#define HANAMI_BLOSSOM_PNG_SHIM_LEN 41

size_t hanami_blossom_shim_encoded_len(size_t ciphertext_len);

hanami_error_t
hanami_blossom_shim_encode(const uint8_t *ct, size_t ct_len,
                           uint8_t *out, size_t out_cap);

bool
hanami_blossom_shim_detect(const uint8_t *buf, size_t len);

hanami_error_t
hanami_blossom_shim_strip(const uint8_t *buf, size_t len,
                          const uint8_t **out_ct, size_t *out_ct_len);
```

- `encode` writes the shim prefix (with correct IHDR CRC + IDAT
  length) followed by a verbatim copy of ct. Rejects `ct_len >
  UINT32_MAX` (IDAT length field is 32-bit), NULL out, undersized
  out buffer.
- `detect` returns true iff `buf[0..41)` byte-for-byte matches the
  shim's constant prefix with the IDAT length field == `len - 41`.
  Rebuilds the expected prefix each call so the check is strict —
  no partial-prefix false positives.
- `strip` is `detect + return borrowed pointer/len past the prefix`.
  The tail lives inside the caller-owned buffer; caller must not
  free it separately.

Unambiguity with the D4 wire: porthome ciphertext always begins with
`0x01` (D4 version byte); a shimmed blob begins with `0x89` (PNG
signature). Fetchers can decide without any manifest metadata.

### 3.2 Capability record extension

`hanami-server-capability.h` gains two tri-state fields:

```c
hanami_capability_state_t raw_random_ok;   /* YES if plain PUT accepted */
hanami_capability_state_t png_shim_ok;     /* YES if shimmed PUT accepted */
```

Both default to `HANAMI_CAP_UNKNOWN`. `raw_random_ok=NO` +
`png_shim_ok=YES` is the empirical shape a future probe extension
needs to flip to decide the shim is required for this server. That
probe extension (uploading both a plain and a shimmed 1-KiB random
blob at connect time) is filed as a spinoff bead — see §6.

### 3.3 Pusher wiring — `nh_porthome_blossom.c`

Both `nh_porthome_blossom_upload` (single-blob path) and
`nh_porthome_blossom_upload_batch` (Phase-2 provisioner batch path)
honour a new environment variable:

```
NOSTR_HOMED_BLOSSOM_PNG_SHIM=1
```

When set, EVERY blob is wrapped in the PNG shim BEFORE it is hashed.
The manifest chunk_hash IS `sha256(shim || ciphertext)`. The batch
path clones-then-shims into a parallel array whose lifetime is the
call; the caller-supplied blob record is temporarily redirected to
the shimmed bytes so the batch uploader sees a consistent view for
hashing + PUT.

Unset (default) or `=0` keeps the legacy raw-bytes behaviour
bit-for-bit — existing pushes and unit tests are unaffected. This
matches the review's "shim everywhere OR shim nowhere" posture:
a single push run either shims every chunk or none. Mixed-mode
manifests are intentionally out of scope for v1.

### 3.4 Fetcher wiring — `nostr-home-fetch.c`

`fetch_chunk_cb`, after downloading a blob and validating its
sha256, now checks `hanami_blossom_shim_detect(buf, len)`. On a
positive detect it calls `hanami_blossom_shim_strip` and memmoves
the tail down to offset zero; the caller-facing `out_ct` +
`out_ct_len` are the stripped ciphertext.

Detection is unconditional — the fetcher does not need to know
whether the pusher shimmed. The PNG signature at the head of the
blob is the only signal it needs.

Belt-and-braces: if a hostile server injects a shim-shaped prefix
on a non-shimmed blob, the downstream AEAD tag check on the D4
wire will fail. There is no path from a false-positive detect to
plaintext exposure.

### 3.5 Tests — `test_hanami_blossom_shim`

Seven focused tests under label
`libhanami;blossom;shim;unit`, TIMEOUT 30:

```
libhanami Blossom PNG-shim tests
================================
  encoded_len                                             OK
  encode_rejects_bad_args                                 OK
  encode_prefix_shape                                     OK
  encode_deterministic                                    OK
  detect_and_strip_round_trip                             OK
  detect_rejects_non_shim                                 OK
  detect_wrong_idat_length                                OK

PASS
```

Run locally on macOS/AppleClang 17.0.0 with a standalone compile
(sha256 impl vendored inline in the test to avoid an OpenSSL link
edge). The tests exercise:

- **encoded_len** — the size math is correct at edge sizes (0, 1 KiB,
  4 MiB chunk boundary).
- **encode_rejects_bad_args** — NULL out, NULL ct + ct_len>0, and
  undersized out_cap all return HANAMI_ERR_INVALID_ARG. Empty-ciphertext
  encode is legal.
- **encode_prefix_shape** — byte-for-byte assertion of every field in
  the shim prefix, including the IHDR CRC constant 0x3A7E9B55.
- **encode_deterministic** — two independent encodes of identical
  ciphertext produce identical bytes AND identical sha256. Convergence
  is preserved through the shim. The shimmed sha256 differs from the
  plain-ciphertext sha256, as it must.
- **detect_and_strip_round_trip** — encode → detect → strip yields the
  original ciphertext bytes exactly.
- **detect_rejects_non_shim** — a D4-shaped blob (leading 0x01) is
  correctly identified as NOT a shim; strip refuses; a NULL buffer
  and a short buffer are refused; a PNG-signature-but-bad-IHDR fake
  is refused.
- **detect_wrong_idat_length** — a shim whose IDAT length field
  disagrees with `len - 41` is rejected. This guards against an
  adversary who wraps ciphertext in a truncated/extended shim to
  trigger a mis-strip.

### 3.6 CMake

`libhanami/CMakeLists.txt` gains `src/hanami-blossom-shim.c` to the
library sources and `test-hanami-blossom-shim` to the test set
(mirrors the existing `test-hanami-blossom-content-type` block).
`apply_sanitizers` is applied when available.

## 4. Behaviour matrix

| NOSTR_HOMED_BLOSSOM_PNG_SHIM | Pusher                                        | Fetcher                              | Manifest chunk_hash        |
| ---------------------------- | --------------------------------------------- | ------------------------------------ | -------------------------- |
| unset / "0"                  | raw ciphertext PUT                            | detect returns false; passes through | sha256(ciphertext)         |
| "1"                          | shim + ciphertext PUT (+41 bytes / chunk)     | detect returns true; strips 41 bytes | sha256(shim + ciphertext)  |
| any other value              | unset semantics (treated as "0")              | (same)                               | (same)                     |

The fetcher's behaviour is independent of the env var — it decides
per-blob based on the PNG signature. Downloading a shimmed blob into
a non-shim-aware build fails the sha256 check inside
`nh_porthome_blossom_fetch` (that check runs on the raw downloaded
bytes, so it sees `sha256(shim + ct)` == manifest chunk_hash and
passes on the shim path too) — but the downstream AEAD decrypt fails
because the shim bytes are fed to the decrypter as if they were the
D4 wire. Belt-and-braces holds; the failure mode is a loud crypto
error rather than silent corruption.

## 5. Interaction with `nostrc-jmx0`

jmx0's block reason is refined but not fully cleared by this pass:

- The primitives + wiring for shim uploads are shipped and pass unit
  tests, but **no live probe against blossom.band or blossom.primal.net
  has verified the shim actually satisfies their sniffers**. yo44's
  Round 2 probe scripts live on the aarch64 lab host
  (`bizarro@192.168.64.3:/tmp/yo44_probe/probe{,2,3}.py`) and need
  Python + `secp256k1` to mint kind-24242 events. Running a fourth
  round with `probe4.py` that PUTs a `shim || 1KiB-random` blob is
  what would close the last inch.
- Until that live-probe validation lands, two-machine §9.4 remains
  blocked in the same way it was: either
  (a) run the lab-side probe with the shim and confirm 2xx on ≥ 1
      community server → close jmx0;
  (b) find one additional permissive server via Option A → close jmx0;
  (c) accept the sharegap-only replication and document the demo as
      single-server (already done for 89rj Part 2, but that is a
      compromise, not a solution).

**jmx0 stays open** with the updated block reason: "awaiting live
probe of `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1` against blossom.band /
blossom.primal.net". A dedicated jmx0 review documents this — see
`docs/reviews/porthome-jmx0-two-machine-2026-09-25.md`.

## 6. Follow-ups (filed / recommended)

- **`nostrc-prli` (P3, this pass)** — libhanami: extend
  `hanami_server_probe_capabilities` to include a
  raw-vs-shim body-sniffer probe. Upload one 1 KiB session-random
  blob plain and one wrapped in the PNG shim. Record
  `raw_random_ok` / `png_shim_ok` per server. When
  `raw_random_ok=NO` AND `png_shim_ok=YES` on any destination server
  in a push, the pusher can automatically enable
  `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1` for that run. Superseded once we
  ship the operator UX for `--use-shim=auto`.
- **`nostrc-hy3e` (P3, this pass)** — porthome: LIVE-PROBE
  bpum-shim. From the lab host with the yo44 tooling, PUT
  `shim || 1KiB-random` against `blossom.band` (server-tag omitted)
  and `blossom.primal.net`. Expected outcome: `2xx` on at least
  one, closing bpum + unblocking jmx0. If both still 415, the
  content-policy filter looks deeper than the header + first IDAT —
  we then either extend the shim (fake IDAT deflate stream / full
  IEND) or fall back to Option A (permissive third-party server).
- **`nostrc-si30` (P4, this pass, blocked-by prli + hy3e)** — porthome: replace the env
  var with a runtime capability-driven default. Once
  `hanami_server_probe_capabilities` gains the raw-vs-shim probe
  (previous bead), the pusher should decide shim-yes/shim-no without
  operator intervention. The env var stays as a manual override for
  lab experimentation.
- **Not filed** — Option A server discovery (community list scrape,
  self-hosted permissive server). Depends on human-loop probing and
  can be paired with the live-probe follow-up above.

## 7. Dep-purity

The shim module lives entirely inside libhanami and has no runtime
deps beyond the C standard library (memcpy / memmove). The pusher-
and fetcher-side wiring picks it up through the existing
`#include <hanami/hanami-blossom-shim.h>` include path; `nostr_porthome`
already links `hanami` PUBLIC, and `nostr-home-fetch` already links
`nostr_porthome`, so the transitive dep is a no-op. No new closure
edge into `nostr-authd` or `pam_nostr.so`.

Ran a standalone compile of `hanami-blossom-shim.c` and the test
binary against AppleClang 17.0.0 with `-Wall -Wextra -Wpedantic
-std=c11` — clean, zero diagnostics, all seven unit tests green.
Standalone compile of `nh_porthome_blossom.c` and
`nostr-home-fetch.c` (with the required include paths but no linking)
is also clean. Full-tree `dpkg-buildpackage -us -uc -b -j4` on
aarch64 remains the packaging-gate step — expected clean modulo the
same unrelated `nostrdb.h` header discovery issue tracked separately.

## 8. Commit / close

- Commit: this diff (shim module + wiring + tests + docs).
- `nostrc-bpum`: **keep OPEN** — plumbing v1 done, plumbing v2
  (probe-driven auto-shim) filed as follow-up. Live-probe validation
  against blossom.band / primal.net is what actually closes the
  bead; that step needs the lab host.
- `nostrc-jmx0`: **keep OPEN**, block reason refined (§5).
- `nostrc-prli`, `nostrc-hy3e`, `nostrc-si30`: filed above.
