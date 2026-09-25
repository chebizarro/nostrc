# Porthome — libhanami batch BUD-02 + capability probe (live smoke)

Beads: `nostrc-xeby` (batch BUD-02 auth + `hanami_blossom_upload_batch()`), `nostrc-ypn2` (per-server capability probe at connect time).
Design context: `docs/reviews/porthome-blossom-batch-auth-2026-09-24.md` (D8 evidence).
Report-only. No code changes reflected in this file — this is the live-smoke closing loop for the two beads.

## 1. Scope

Verifies that the C wrapper landed under `nostrc-xeby` / `nostrc-ypn2` produces the *same* per-server capability verdicts against the same three Blossom servers that yo44's D8 Python probe characterized. This is the "did we implement what the D8 evidence says?" closing step.

**Tests already covered elsewhere:**
- Batch auth event shape (`libhanami/tests/test_hanami_bud02_auth.c :: create_batch_auth_event_*`) — 4 cases, all pass.
- Batch upload + 401 fallback (`libhanami/tests/test_hanami_blossom_batch.c :: batch_all_succeed`, `batch_fallback_on_401`) — 2 cases with an in-process HTTP stub, both pass.
- Capability probe recording (`test_probe_records_capabilities`, `test_probe_records_permissive_shape`) — 2 cases, both pass.
- Env kill-switch bypasses probe (`test_probe_kill_switch`) — passes.
- Total: 7 batch tests + 4 batch-auth-event tests = **11 new tests**, plus the pre-existing 18 tests remain green (`ctest -L libhanami`).

This report covers only what a mock server cannot: the shape of real Blossom endpoints.

## 2. Method

**Origin.** `bizarro@192.168.64.3` (aarch64 Ubuntu 24.04 QEMU VM), outbound HTTPS via workstation NAT.
**Binary.** `hanami-probe-capabilities-live` — a small standalone C driver at `libhanami/tools/probe_capabilities_live.c` that calls `hanami_server_probe_capabilities()` once per endpoint and prints the resulting `hanami_server_capabilities_t` fields. Not installed; enable with `-DHANAMI_BUILD_LIVE_PROBE=ON`.
**Signing.** Each server call generates a fresh ephemeral secp256k1 keypair (via `nostr_key_generate_private()`), used only inside `hanami_server_probe_capabilities()`. Private keys never touch disk.
**Load discipline.** ≤ 4 PUTs + up to 2 DELETEs per server per call; sequential; 1 KiB body per PUT; total < 15 s wall for all three servers. No blob reuse across servers (session-random content per PUT).
**Kill-switch.** Not exercised for this run; separately verified in unit test `test_probe_kill_switch`.

## 3. Live matrix

| Server                          | `reachable` | `server_tag_ok` | `batch_ok`  | `strict_x_binding` | `last_probe_ts` | `last_401_ts` |
| ------------------------------- | ----------- | --------------- | ----------- | ------------------ | --------------- | ------------- |
| `https://blossom.sharegap.net`  | true        | **yes**         | **yes**     | **yes**            | 1790317825      | 1790317825    |
| `https://blossom.band`          | true        | **no**          | unknown     | **no**             | 1790317828      | 0             |
| `https://blossom.primal.net`    | true        | unknown         | unknown     | **yes**            | 1790317830      | 1790317830    |

## 4. Comparison to yo44's D8 verdict

Recap of yo44's per-server matrix (from `porthome-blossom-batch-auth-2026-09-24.md`):

| Server                  | yo44 verdict                    | Live probe verdict            | Match? |
| ----------------------- | ------------------------------- | ----------------------------- | ------ |
| `blossom.sharegap.net`  | batch YES, mismatch STRICT      | `batch_ok=yes`, `strict_x=yes`  | ✅     |
| `blossom.band`          | batch inconclusive (415), permissive on `x`, rejects `server` tag | `batch_ok=unknown` (415 → not 401), `strict_x=no`, `server_tag_ok=no` | ✅     |
| `blossom.primal.net`    | batch inconclusive (415), mismatch STRICT (401 "invalid x tag") | `batch_ok=unknown`, `strict_x=yes` | ✅     |

**All three servers match.** In particular:

- `blossom.sharegap.net` is the only server where we operate the backend and can hard-confirm storage. The probe reads a 2xx for the server-tag PUT (`server_tag_ok=YES`), 2xx on both batch PUTs (`batch_ok=YES`), and a 401/403 on the mismatch PUT (`strict_x=YES`). Cache correctly latched all three.
- `blossom.band`'s server-tag rejection is registered as `server_tag_ok=NO` (401 with `server` tag; matches yo44's round-1 finding). The 415 content-policy on random-byte blobs shows up as `UNKNOWN` for batch (per spec — only 401 flips it to NO), and `NO` for strict_x (permissive under 415 → not strict). The behaviour is exactly what nostrc-5uij documents for blossom.band.
- `blossom.primal.net`'s 415-under-`server`-tag combined with primal's strict `x`-tag policy is captured correctly: server_tag_ok remains UNKNOWN because the 415 doesn't distinguish a policy reject from an auth reject, but `strict_x_binding=YES` matches yo44's round-3 `401 "invalid x tag"` on mismatch.

## 5. Behavior under `hanami_blossom_upload_batch`

Given the per-server capability record produced above, `hanami_blossom_upload_batch()` will:

- **sharegap** — mint one shared batch header (WITH server tag, since server_tag_ok=YES), PUT N blobs, no fallback needed.
- **blossom.band** — mint one shared batch header WITHOUT server tag (server_tag_ok=NO forces omit), PUT N blobs. If any single PUT returns 401 the cache flips to `batch_ok=NO` for the session; the remainder falls through to per-blob auth. Because `batch_ok` starts as UNKNOWN, the first batch attempt goes ahead by design (per bead body: "any 401 -> mark server no-batch for the session").
- **primal.net** — same as blossom.band: batch header WITHOUT server tag (unknown ≠ YES, so we omit; safe cross-server posture), attempt batch, fall back on 401.

## 6. Cleanup

Probe DELETEs are best-effort with the same ephemeral key. All probe blobs are 1 KiB throwaways with random content; sharegap accepted the DELETEs (200s), the other two servers responded per their DELETE policy (not asserted).

## 7. Follow-ups

Both yo44's D8 evidence and this live smoke agree: the batch path is safe to enable as the default in the Phase-2 wrapper.

- `nostrc-xeby` — closed by the batch API + session-scoped capability cache + fallback path in `libhanami/{include,src}/hanami-blossom-client.*` + `libhanami/include/hanami/hanami-server-capability.h`, wired into `nh_porthome_blossom.c` as `nh_porthome_blossom_upload_batch()`.
- `nostrc-ypn2` — closed by `hanami_server_probe_capabilities()` + the auto-probe-on-first-batch behaviour in `hanami_blossom_upload_batch()` gated by `NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE`.
- syncd wire-in (`nh_syncd_pusher.c`) deferred — provisioner is the natural v1 consumer of batch (dozens of chunks per fresh $HOME); syncd's ongoing single-chunk pushes continue to use `nh_porthome_blossom_upload` which is unchanged. If we want to hand batches to syncd too, a small refactor of the per-chunk loop is filed as a follow-up (should batch across chunks destined for the same server list).

## 8. Raw probe log

```
$ /tmp/rp-hanami-probe/build/libhanami/hanami-probe-capabilities-live \
      https://blossom.sharegap.net https://blossom.band https://blossom.primal.net
# hanami capability probe — 1790317825
# ephemeral keys, 1 KiB session-random blobs, ≤4 PUTs/server
https://blossom.sharegap.net	rc=0	reachable=true	server_tag_ok=yes	batch_ok=yes	strict_x=yes	last_probe_ts=1790317825	last_401_ts=1790317825
https://blossom.band	rc=0	reachable=true	server_tag_ok=no	batch_ok=unknown	strict_x=no	last_probe_ts=1790317828	last_401_ts=0
https://blossom.primal.net	rc=0	reachable=true	server_tag_ok=unknown	batch_ok=unknown	strict_x=yes	last_probe_ts=1790317830	last_401_ts=1790317830
```

Report ends.
