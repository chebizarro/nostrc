# porthome-syncd — multi-server accounting + rescan live smoke

**Bead:** nostrc-xnxd (Phase 5 syncd follow-ups)
**Host:** `bizarro@192.168.64.3` — Ubuntu 24.04 aarch64, kernel 7.0.0-31-generic (arm64 QEMU VM)
**Date:** 2026-09-24
**Binary:** `/tmp/nostrc-syncd-xnxd/build/gnome/nostr-homed/nostr-home-syncd` (1,067,864 bytes; 44 shared-lib deps unchanged from prior build)
**Working tree:** `feat/porthome-syncd-followups`, worktree `rp-agent-c5faf43a-feat-porthome-syncd-followups-14268c54`.

Everything ran under `/tmp` — no writes to `/usr`, `/etc`, or a live user session.
Ctest label `syncd`: **18/18 pass** (see § 4 below).

## 1. Scope covered by this smoke

| Part | What the smoke exercises |
| --- | --- |
| 1 — per-server upload accounting | `test_syncd_pusher_multi_server` (4 sub-cases). Real min_replication enforcement + generation-freeze on quorum miss. Status-file fields (`last_upload_servers_ok`, `_total`, `_error_class`) surfaced in the daemon status write in step 6. |
| 2 — full-tree rescan on missed inotify | `test_syncd_rescan_overflow` (quiescent-zero + add-remove) drives `nh_syncd_rescan_diff` — the same primitive the watcher's `IN_Q_OVERFLOW` handler and the periodic rescan tick invoke. |
| 3 — broker credential handoff audit | Steps 2 + 3 below; the daemon reads the drop file, unlinks it, and refuses the env-fallback path unless `NOSTR_HOMED_SYNCD_TEST_MODE=1`. |

## 2. Broker seed drop consumed, env fallback gated (part 3)

Step 6 of the smoke shows the daemon reading `/tmp/.../drop2/home_seed` and unlinking it:

```
syncd: read wrap seed from broker drop at /tmp/xnxd_smoke_662379/drop2/home_seed (unlinked)
```

Step 2 shows a `--check` run with the drop file *absent* and `NOSTR_HOMED_SYNCD_TEST_MODE` unset: the daemon accepts (exit 0) but reports `state=unknown` — `c->seed_hex` is `NULL` and the batch loop would defer any push until a real seed arrives.

Step 3 shows the env fallback is only honoured when `NOSTR_HOMED_SYNCD_TEST_MODE=1` — matches the `load_cfg` audit note in `nostr-home-syncd.c` and the packaging-level comment in `systemd/user/nostr-home-sync.service.in`.

## 3. Multi-server accounting + status-file wiring (part 1)

Two Blossom endpoints supplied — one real (`https://blossom.sharegap.net`), one RFC 5737 TEST-NET-1 unreachable (`https://192.0.2.99`). `min_replication=2`. The daemon booted, consumed the broker seed, discovered no snapshot base → set the partial-state marker → the next batch's interlock refused the push:

```
syncd: batch 1 FAILED rc=-208
```

(`-208` == `NH_SYNCD_ERR_PARTIAL_STATE`, pre-xnxd interlock. The pusher never reached the Blossom PUT loop, so `last_upload_servers_ok/total` stayed at 0 — expected.)

The status file was emitted with the new fields:

```json
{
  "schema": 1,
  "syncd": {
    "state": "error",
    "last_push_gen": 0,
    "last_pull_gen": 0,
    "last_error_class": "push-failed",
    ...
    "last_upload_servers_ok": 0,
    "last_upload_servers_total": 0,
    "last_upload_error_class": "",
    "recent": []
  }
}
```

The real min_replication enforcement + `NH_SYNCD_ERR_INSUFFICIENT_REPLICATION` classification is proven unit-test-side (§ 4): three-server stub, min_replication=2 succeeds, min_replication=3 refuses with generation frozen, min_replication=1 with a single accept passes, all-refuse returns `NH_SYNCD_ERR_UPLOAD`.

## 4. Full ctest run (label `syncd`)

```
$ ctest -L syncd --output-on-failure
...
16/18 Test #159: homed_syncd_pusher_multi_server ........   Passed    0.02 sec
17/18 Test #160: homed_syncd_rescan_overflow ............   Passed    0.01 sec
18/18 Test #161: homed_syncd_pull_e2e ...................   Passed    0.02 sec

100% tests passed, 0 tests failed out of 18
Label Time Summary:
nostr-homed      =   0.27 sec*proc (18 tests)
porthome         =   0.27 sec*proc (18 tests)
syncd            =   0.27 sec*proc (18 tests)
```

## 5. Dep-purity gate

`ldd nostr-home-syncd` unchanged from previous build — 44 shared-lib entries, all
pre-existing (`libcurl`, `libwebsockets`, `libjansson`, `libsecp256k1`,
`libhanami`'s transitive closure via `libgit2`/`libssh`/`libssl`,
`libnsync`, `libsqlite3`, `libc`/`libm`). No new `.so` introduced by the
xnxd changes — all new code uses `clock_gettime` (libc), the existing
`nh_porthome_blossom_*` API, and the existing jansson/pthread machinery
already linked into `nostr_syncd_core`.

## 6. Full smoke transcript (raw)

```
=== 1. binary + libs ===
-rwxrwxr-x 1 bizarro bizarro 1067864 Sep 24 22:00 /tmp/nostrc-syncd-xnxd/build/gnome/nostr-homed/nostr-home-syncd
44
=== 2. --check without seed (broker drop absent, TEST_MODE off) ===
syncd: snapshot base unknown; running additive rescan + setting partial state
syncd: --check OK (lock=held, interlocks=pass, state=unknown)
  exit=0  (0=OK; seed unset ok in --check)
=== 3. --check with TEST_MODE=1 + env-seed fallback (audit) ===
syncd: --check OK (lock=held, interlocks=pass, state=loaded)
  exit=0  (0=OK; env fallback allowed under TEST_MODE)
=== 4. multi-server unit test (real min_replication enforcement) ===
t_two_of_three_quorum_ok OK
t_three_needed_but_only_two_ok OK
t_min_repl_one_single_ok OK
t_all_refuse_returns_upload_error OK
test_syncd_pusher_multi_server: ALL OK
=== 5. IN_Q_OVERFLOW rescan unit test ===
t_quiescent_zero_events OK
t_added_and_removed OK
test_syncd_rescan_overflow: ALL OK
=== 6. status file emission smoke ===
  daemon pid=662421
  --- daemon log tail ---
syncd: read wrap seed from broker drop at /tmp/xnxd_smoke_662379/drop2/home_seed (unlinked)
syncd: snapshot base unknown; running additive rescan + setting partial state
syncd: batch 1 FAILED rc=-208
  --- syncd status file: /tmp/xnxd_smoke_662379/state2/xdg/nostr-homed/porthome-status.json ---
{
    "schema": 1,
    "syncd": {
        "state": "error",
        "last_push_gen": 0,
        "last_pull_gen": 0,
        "last_error_class": "push-failed",
        "pinned_gen_count": 0,
        "cache_bytes": 0,
        "cache_quota_bytes": 0,
        "cache_quota_source": "default",
        "evict_rate_1h": 0,
        "last_upload_servers_ok": 0,
        "last_upload_servers_total": 0,
        "last_upload_error_class": "",
        "recent": []
    }
}
=== 7. cleanup ===
smoke done.
```

## 7. Notes / follow-ups

* Live smoke with a signed pointer + real Blossom PUT is deferred; the
  daemon needs a valid nsec + a snapshot base (i.e. a real pull path first)
  before an inotify-driven batch can advance the generation. The
  min_replication enforcement is proven synthetically via
  `test_syncd_pusher_multi_server` + the sealed unit tests already in the
  suite; a live-Blossom pump-and-observe belongs behind the next Phase-6
  end-to-end skit.
* The periodic rescan tick (`NOSTR_HOMED_SYNCD_RESCAN_INTERVAL_SEC`) is
  wired but defaults OFF — leaving the poll timeout untouched in prod
  installs that don't set it. When set, the poll loop takes the min of
  its normal timeout and "seconds until next tick".
