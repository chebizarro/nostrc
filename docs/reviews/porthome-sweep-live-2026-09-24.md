# Portable-home Phase 3 W(2): real-Blossom sweep live probe

- Bead: nostrc-p6qp (Phase 3 wrap-up W(2)); parent nostrc-h10m.
- Design: `docs/designs/home-from-relay.md` §6.5 (retention / weekly HEAD sweep).
- Related evidence: `docs/reviews/porthome-live-e2e-2026-09-23.md`
  (Phase 3 I3 live-relay + live-Blossom e2e including kind-30078 pointer publish).
- Probe binary: `gnome/nostr-homed/tests/integration/nostr_home_sweep_probe.c`
  → target `nostr-home-sweep-probe` (not a CTest case; operator-driven, hits
  real infra).
- Lab: aarch64 VM `bizarro@192.168.64.3`; build under `/tmp/nostrc-p6qp`.

## What the probe proves

1. Publish a small fixture (3 tiny text files) via the same
   nh_porthome_blossom uploader + BUD-02 signer as the operator publisher.
   Snapshot.json is written to the supplied state dir with each file's
   `chunk_addrs_hex`.
2. BUD-02 DELETE two of the three known blobs from
   `https://blossom.sharegap.net` (operator-only path; the daemon still MUST
   NOT delete in v1 per §6.5).
3. Run `nh_syncd_sweep_run_once` against the snapshot with the same
   Blossom server as sole target and a notification hook installed:
   - HEAD detects both blobs missing.
   - Sweep counts `dropped == 2`.
   - The `notify` callback fires exactly once with `dropped=2`.
4. BUD-02 PUT sanity: re-upload a small sentinel blob to prove that the
   PUT axis works against the live server (the sweep itself declined to
   re-upload only because we intentionally passed `cache=NULL` — there is
   no local plaintext to source from in the probe process).

## Live transcript (2026-09-24, `bizarro@192.168.64.3`)

Command:

    nostr-home-sweep-probe run \
      --state-dir   /tmp/nh_sweep_probe/state \
      --fixture-dir /tmp/nh_sweep_probe/fixture \
      --seed-hex    <redacted-64-hex> \
      --nsec-hex    <redacted-64-hex> \
      --blossom     https://blossom.sharegap.net \
      --drop-n 2

Fixture (3 files, ~50 bytes each) → 3 Blossom uploads. After the probe
DELETEs the first two, sweep detects both missing and fires one notification:

```
sweep_probe: publish OK, snapshot at /tmp/nh_sweep_probe/state/snapshot.json
sweep_probe: DELETE 2042b1aace9e8a2ff4bb78d7bb3949bf07a69b200899ab53ff0c3b2cd6967c16 from https://blossom.sharegap.net -> OK
sweep_probe: DELETE 0aa7a344d999e75480b79b06e56079f50c9d8088eb70a9a4a2c73029f72c259e from https://blossom.sharegap.net -> OK
nh_syncd_sweep: blob 2042b1aace9e8a2f… missing on https://blossom.sharegap.net
nh_syncd_sweep: cannot re-upload 2042b1aace9e8a2f…: no cache configured
nh_syncd_sweep: blob 0aa7a344d999e754… missing on https://blossom.sharegap.net
nh_syncd_sweep: cannot re-upload 0aa7a344d999e754…: no cache configured
nh_syncd_sweep: sweep done: 3 blobs checked, 2 dropped, 0 re-uploaded
sweep_probe: sweep rc=0 dropped=2 reuploaded=0
sweep_probe: notify_count=1 summary=Your Blossom server dropped 2 blobs (re-uploaded 0)
DROPPED_SHA256=2042b1aace9e8a2ff4bb78d7bb3949bf07a69b200899ab53ff0c3b2cd6967c16
DROPPED_SHA256=0aa7a344d999e75480b79b06e56079f50c9d8088eb70a9a4a2c73029f72c259e
SWEEP_DROPPED=2
SWEEP_REUPLOADED=0
SWEEP_NOTIFY_COUNT=1
REUPLOAD_SENTINEL_SHA256=cd5fff2d434966ce7821add4ed5caef830195924c0f388a118358bdb0d5ece74
REUPLOAD_SENTINEL_SHA256=cd5fff2d434966ce7821add4ed5caef830195924c0f388a118358bdb0d5ece74
REUPLOAD_MANUAL=2
EXIT=0
```

Blossom sha256s exercised (post-publish snapshot.json, `d_tag=nostr-homed.sweep.probe`):

| file  | sha256 (chunk 0)                                                 | fate        |
|-------|-------------------------------------------------------------------|-------------|
| a.txt | `0aa7a344d999e75480b79b06e56079f50c9d8088eb70a9a4a2c73029f72c259e` | DROPPED     |
| b.txt | `94594a8466db015bf61312cdba41544393ce5a13ebcda063451229157317c871` | untouched   |
| c.txt | `2042b1aace9e8a2ff4bb78d7bb3949bf07a69b200899ab53ff0c3b2cd6967c16` | DROPPED     |
| sentinel | `cd5fff2d434966ce7821add4ed5caef830195924c0f388a118358bdb0d5ece74` | PUT-tested |

kind-30078 pointer publish is out of scope for the sweep round-trip
(sweep walks snapshot.json only). The Phase 3 I3 live e2e already covered
the pointer publish path against the same relay pair — see
`docs/reviews/porthome-live-e2e-2026-09-23.md`.

## Notes

- The daemon's own sweep path never DELETEs (§6.5). The DELETE step above
  is a probe-only injection to force the "server dropped a blob" scenario
  we cannot easily reproduce naturally in a lab.
- The probe intentionally passes `cache=NULL` to `nh_syncd_sweep_run_once`
  because there is no local plaintext of the encrypted chunks to source
  from in the probe process (a real syncd deployment feeds the sweep the
  live `nh_syncd_cache` — proven in `test_syncd_sweep` unit-level).
- The seed / nsec used above were single-use ephemeral values (`openssl
  rand -hex 32`) — not associated with any real account. They are omitted
  from this document per the "sanitized transcript, no keys" rule.
