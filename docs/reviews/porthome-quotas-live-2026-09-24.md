# Portable-home cache quotas + daemon status writers — live verify (2026-09-24)

Bead(s): **nostrc-8hw8** (Phase 5 I3: cache quotas), **nostrc-h10m.1.1**
(daemon-side status writers). Both close after this run.

Host: `bizarro@192.168.64.3` — Ubuntu 24.04 arm64 (Linux 7.0.0-31-generic
aarch64, QEMU VM). Everything landed under `/tmp/nostrc-porthome-quotas`;
no writes to `/etc`, `/var`, or the actual `$HOME`.

Build gates ON: `NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL`,
`NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL`,
`NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL`,
`NOSTR_HOMED_ENABLE_AUTH_RUNTIME`, `NOSTR_HOMED_ENABLE_AUTH_CORE`,
`NOSTR_HOMED_ENABLE_IDENTITY_CORE`. Base off:
`-DNOSTR_HOMED_ENABLE_AUTH_INSTALL=OFF`.

## What shipped

1. **Quota computation** — new `nh_syncd_cache_effective_quota()` layers:
   env `NOSTR_HOMED_PORTHOME_CACHE_QUOTA_BYTES`, then
   `$XDG_CONFIG_HOME/nostr-homed/cache-quota`, then the pre-existing
   `min(10 GiB, 10 % of FS)` default. Both overrides are clamped to
   `[1 GiB, 200 GiB]` — values outside the band are logged and rejected.
   Cache logs `dir=… quota_bytes=… (source=env|config|default)` on open
   and on SIGHUP reload.

2. **Auto-eviction at pull-write time** — `nh_syncd_cache_put()` now
   sweeps LRU-unpinned after each successful landing when
   `set_auto_evict(true)`. Flag is OFF by default (legacy callers keep
   the pre-I3 invariant); syncd + fuse flip it ON immediately after
   opening the cache. If sweep can't reclaim enough or the just-put
   blob was itself evicted (only-unpinned candidate + pins saturate the
   quota), put returns the new `NH_SYNCD_CACHE_ERR_QUOTA_PINNED`.

3. **Thrash detector** — 1-hour rolling window, threshold defaulted to
   100 and overridable via `NOSTR_HOMED_PORTHOME_QUOTA_THRASH_THRESHOLD`.
   First breach fires the registered `nh_syncd_cache_evict_notify_fn`
   exactly once per window; syncd wires it to
   `nh_porthome_notify(LIMITED_MODE, key="cache-thrashing", …)`.

4. **Daemon-side status writers** — new
   `src/porthome-syncd/nh_syncd_status.[c,h]` composes the "syncd" key
   body (state / last_push_gen / last_pull_gen / last_error_class /
   pinned_gen_count / cache_bytes / cache_quota_bytes /
   cache_quota_source / evict_rate_1h + `recent[]` rolling ring capped at
   20 slots, most-recent-first). Emit uses the existing atomic merge
   helper. The fuse side gets `nh_fuse_write_status_unified()` under
   `nh_fuse_status.h` and mirrors into the "fuse" key at every
   `write_status_json()` invocation.

5. **Notifier record hook** — `nh_porthome_notifier_set_record()` wired
   in syncd main to `nh_syncd_status_notify_record`, which appends to
   `recent[]` and re-emits.

6. **`nostr-home-status quota`** — new subcommand: default pretty print,
   `--json`, `--set-override BYTES`, `--clear-override`, `--reload`
   (delegates to `systemctl --user reload nostr-home-sync.service`,
   falls back to `pkill -HUP nostr-home-syncd`).

## Unit + regression tests (host = bizarro)

- `test_syncd_quota` (new): env override rejection at bounds, XDG
  config-file override honoured, auto-eviction after put drops the LRU
  unpinned candidate, `QUOTA_PINNED` returned when the only unpinned
  chunk is the fresh put, thrash notify fires once per window.
- `test_porthome_status_daemon_writers` (new): syncd key replaces
  atomically; fuse key survives peer writes; `recent[]` rolls (slot 4
  gets evicted while slot 5 survives; most-recent renders first).
- Regressions (all green): `test_porthome_status`,
  `test_porthome_notify`, `test_fuse_status`, `test_syncd_cache`,
  `test_syncd_cache_lru`, `test_syncd_reconcile`, `test_syncd_pull_e2e`,
  `test_syncd_push_e2e`, `test_fuse_source_poison`, `test_fuse_source`,
  `test_fuse_table`, `test_syncd_pin_ring`, `test_syncd_sweep`, …

Full ctest run (label `nostr-homed`): 32/35 pass. The 3 failures
(`homed_porthome_smallhome`, `homed_porthome_sandbox_integration`,
`homed_porthome_broker_wait`) are pre-existing missing-driver test
targets — unaffected by this change; the failure text is literally
`driver not executable: … porthome_smallhome_driver` because that
optional executable is not part of the syncd/fuse test bill of
materials.

## Live smoke (real relay + Blossom)

```
NOSTR_HOMED_SYNCD_BLOSSOM=https://blossom.sharegap.net
NOSTR_HOMED_SYNCD_RELAYS=wss://relay.sharegap.net
NOSTR_HOMED_SYNCD_NSEC_HEX=<64-hex>
NOSTR_HOMED_SYNCD_SEED_HEX=<64-hex>
HOME=/tmp/nhq-home XDG_STATE_HOME=/tmp/nhq-state
XDG_CONFIG_HOME=/tmp/nhq-cfg XDG_CACHE_HOME=/tmp/nhq-cache
```

### 1. `--check` bootstraps a status body

```
$ nostr-home-syncd --check
syncd: snapshot base unknown; running additive rescan + setting partial state
nh_syncd_cache: dir=/tmp/nhq-cache/nostr-homed/blobs quota_bytes=6620562630 (source=default) thrash_threshold=100/hr
syncd: --check OK (lock=held, interlocks=pass, state=unknown)

$ cat /tmp/nhq-state/nostr-homed/porthome-status.json
{"schema":1,"syncd":{"state":"idle","last_push_gen":0,"last_pull_gen":0,
 "last_error_class":"","pinned_gen_count":0,"cache_bytes":0,
 "cache_quota_bytes":6620562630,"cache_quota_source":"default",
 "evict_rate_1h":0,"recent":[]}}
```

### 2. CLI `quota` + `--field syncd.*`

```
$ nostr-home-status quota
cache quota:     6620562630 bytes (source=default)
cache used:      0 bytes
pinned gens:     0
evict rate/1h:   0

$ nostr-home-status --field syncd.cache_bytes
0
$ nostr-home-status --field syncd.state
idle
$ nostr-home-status --field syncd.cache_quota_source
default
```

### 3. XDG config-file override + SIGHUP round-trip

```
$ echo 8589934592 > /tmp/nhq-cfg/nostr-homed/cache-quota
$ kill -HUP <syncd_pid>       # daemon log: "reload dir=… quota_bytes=8589934592 (source=config)"
$ nostr-home-status --field syncd.cache_quota_source
config
$ nostr-home-status --field syncd.cache_quota_bytes
8589934592
```

### 4. `nostr-home-status quota --set-override / --clear-override`

```
$ nostr-home-status quota --set-override 2147483648
quota override: 2147483648 bytes -> /tmp/nhq-cfg/nostr-homed/cache-quota
note: syncd will pick up the new value on next SIGHUP
$ cat /tmp/nhq-cfg/nostr-homed/cache-quota
2147483648
$ nostr-home-status quota --clear-override
quota override: cleared
note: syncd will pick up the change on next SIGHUP
$ ls /tmp/nhq-cfg/nostr-homed/cache-quota
ls: cannot access '/tmp/nhq-cfg/nostr-homed/cache-quota': No such file or directory
```

## Dependency-purity gate

`nostr-authd` (the base-line broker built with `AUTH_RUNTIME=ON`,
`PORTHOME_EXPERIMENTAL=ON`, `SYNCD_EXPERIMENTAL=ON`,
`PORTHOME_FUSE_EXPERIMENTAL=ON`):

```
$ ldd .../nostr-authd | grep -Ei 'fuse|hanami|porthome'
(empty)

$ nm .../nostr-authd | grep -c 'nh_syncd\|nh_fuse'
0
```

`nh_porthome_*` symbols ARE present in `nostr-authd` — that is
expected: `NH_AUTH_BROKER_ENABLE_PORTHOME=1` on `nostr_auth_runtime`
statically links the porthome primitives so the broker can
PROVISION_HOME. Zero syncd- and zero fuse-side symbols end up in
authd, and no FUSE3 shared object is dragged in dynamically. Gate is
green.

## Follow-ups

- **fuse process misses its own notifier + thrash wiring.** The fuse
  binary opens its own cache but does not currently instantiate an
  `nh_porthome_notifier`. When the fuse-side cache thrashes, no
  desktop notification fires — syncd's own thrash detector doesn't see
  fuse's evictions because it's a different process. Follow-up bead:
  give fuse a small notifier + wire `set_evict_notify` there too;
  optionally push a "fuse-side cache thrashing" `recent[]` record into
  the `fuse` key rather than `syncd`.
- **Provisioner status key not populated.** Task listed provisioner as
  optional; I punted. Follow-up: extend `auth_porthome.c` to call
  `nh_porthome_status_write_key_default("provisioner", …)` after every
  PROVISION_HOME with `last_provisioned_ts` + `last_state`.
- **Recent-entry deduping.** The `recent[]` ring keeps every notify
  attempt in strict chronological order, so 20 duplicate throttled
  notifications about the same object look identical. Future tuning
  could collapse consecutive same-`(cat, key_hash8)` entries.
- **`--reload` best-effort.** Systemctl --user path is silent on
  failure; the fallback `pkill -HUP` masks the exit code. Acceptable
  today (the CLI prints "syncd will pick up on next SIGHUP" regardless)
  but a next revision should surface a clean error when neither path
  succeeds.

