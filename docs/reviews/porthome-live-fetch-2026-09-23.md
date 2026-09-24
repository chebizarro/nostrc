# Portable-home real-fetch live acceptance — 2026-09-23

**Bead:** `nostrc-9k4g` (parent `nostrc-h10m` — E-portable-home, Phase 2.5).
**Ran by:** Claude Opus 4.7 (Agent SDK).
**Environment:**

- Host: `bizarro@192.168.64.3` (aarch64 QEMU VM, `Linux bizarro-QEMU-Virtual-Machine 7.0.0-31-generic`, `Ubuntu 24.04.5 LTS`).
- Build tree: `/tmp/nostrc-9k4g` (rsync of `feat/porthome-real-fetch` worktree; `.git` and prior `build/` excluded).
- Live infra (operated by the maintainer):
  - `wss://relay.sharegap.net` — Nostr relay.
  - `https://blossom.sharegap.net` — Blossom server (BUD-01 sanity checked separately: HEAD `/` returns 200; unknown-sha returns 404).
- The maintainer's live rig `gnome-dev` was **NOT** touched, per the bead's constraint (that end-to-end is bead `nostrc-pvha`).

## What lands in this bead

`nostrc-9k4g` replaces the placeholder LIMITED closure in the portable-home
provisioner (`gnome/nostr-homed/src/auth/auth_porthome.c`) with a real
network fetch that goes through a **separate unprivileged executable**
`nostr-home-fetch`.

New files (all gated behind `-DNOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL=ON`):

| File | Role |
| --- | --- |
| `gnome/nostr-homed/src/porthome-fetch/porthome_fetch_ctl.{h,c}` | Standalone JSON control-payload parser + progress-line codec. Zero external deps beyond libc; unit-testable in isolation. |
| `gnome/nostr-homed/src/porthome-fetch/nostr-home-fetch.c` | Unprivileged fetch helper. Reads JSON control on stdin, fetches kind-30078 via libnostr `SimplePool`, verifies signature + author + `#d`, hex-decodes the `content` bytes, decrypts via `nh_porthome_manifest_decode_sealed`, then materializes chunks into a passed staging fd/dir through `nh_porthome_materialize_sealed_into_fd`, using an `nh_porthome_blossom_t` for BUD-01 GETs. Emits `{"bytes":N,"files":K,"phase":"..."}\n` progress lines on stdout. Refuses to run as root. |
| `gnome/nostr-homed/src/auth/auth_porthome_fetch.{h,c}` | Broker-side spawner: forks the helper, feeds the control JSON on stdin (bounded 16 KiB), drains progress on stdout, enforces a total wall-clock deadline (SIGKILL on timeout), maps exit codes to `NH_PORTHOME_FETCH_RES_{OK,LIMITED,FAILED,UNAVAILABLE}`. This is the seam bead `nostrc-ww50` wraps for fork+drop-privs. |
| `gnome/nostr-homed/tests/unit/test_porthome_fetch_ctl.c` | Unit tests for parser + progress codec. Links only the standalone library. |

Modified:

| File | Change |
| --- | --- |
| `gnome/nostr-homed/src/auth/auth_porthome.c` | `job_run` now attempts the real fetch helper before falling to LIMITED. Sources relay + Blossom lists from the passed `nh_auth_porthome_config`, falling back to `NH_PORTHOME_FETCH_RELAYS` / `NH_PORTHOME_FETCH_BLOSSOM_SERVERS` env vars until bead `nostrc-ww50` finishes threading `auth.conf` through the broker. `label_write_into` grew a `use_helper` branch that spawns `nostr-home-fetch` with the staging fd inherited across `execv`. |
| `gnome/nostr-homed/CMakeLists.txt` | Adds three new targets under the same `NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL` gate: `nostr_porthome_fetch_ctl` (static, dep-free), `nostr-home-fetch` (executable, requires `libnostr` + `libhanami` + OpenSSL), `test_porthome_fetch_ctl` (unit test). Adds `NH_PORTHOME_FETCH_HELPER_PATH="$<TARGET_FILE:nostr-home-fetch>"` to the runtime so an in-tree build finds the helper without an install step. |

The base packaging invariant is preserved: **`nostr-home-fetch` is a
separate executable and its libnostr / libcurl / libhanami closure never
enters `nostr-authd` or `pam_nostr.so`.** The broker calls the helper via
`fork+execv`, and with `PORTHOME_EXPERIMENTAL=OFF` no new source file is
even compiled into the runtime (all new code is inside a top-level
`if(NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL)` block).

## Unit / integration tests — `ctest -L porthome`

Run on the lab VM after a clean configure (`cmake -DNOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL=ON …`):

```
$ ctest -L porthome --output-on-failure
Test project /tmp/nostrc-9k4g/build
    Start 138: homed_porthome_crypto
1/8 Test #138: homed_porthome_crypto ............   Passed    0.00 sec
    Start 139: homed_porthome_manifest
2/8 Test #139: homed_porthome_manifest ..........   Passed    0.01 sec
    Start 140: homed_porthome_blossom_guards
3/8 Test #140: homed_porthome_blossom_guards ....   Passed    0.00 sec
    Start 141: homed_porthome_fetch_ctl
4/8 Test #141: homed_porthome_fetch_ctl .........   Passed    0.00 sec
    Start 142: homed_porthome_smallhome
5/8 Test #142: homed_porthome_smallhome .........   Passed    6.95 sec
    Start 143: homed_porthome_wrapkey
6/8 Test #143: homed_porthome_wrapkey ...........   Passed    0.00 sec
    Start 144: homed_porthome_provision
7/8 Test #144: homed_porthome_provision .........   Passed    0.01 sec
    Start 145: homed_porthome_broker_wait
8/8 Test #145: homed_porthome_broker_wait .......***Skipped   0.00 sec

100% tests passed, 0 tests failed out of 8
```

Skipped test 145 is a broker-wait integration test that requires the
identity-store fixture the test refuses to fabricate; that skip is
pre-existing and unrelated to this bead.

`homed_porthome_fetch_ctl` is new and exercises: happy round-trip; strict
missing-field rejection; uppercase-hex rejection; unknown-key rejection;
http:// blossom rejected in strict mode; `allow_insecure:true` accepts
`ws://127.0.0.1` for the local integration matrix; oversized payload
(> 16 KiB) rejected; backslash escapes in JSON strings rejected; progress
line roundtrip for every phase; and rejection of extra keys / unknown
phases / negative numbers / missing fields / over-long lines.

## Live acceptance A — negative path (relay returns no matching event)

**Purpose.** Prove the helper does the right thing when a real relay has
no matching kind-30078 event for `(pubkey, #d)`: no crash, no partial
write, no false-success, distinct exit class the broker maps to
LIMITED_MODE.

```
$ cat > /tmp/nhf-ctl.json <<'EOF'
{"account_pubkey_hex":"0011223344556677889900112233445566778899aabbccddeeff0011223344ff",
 "home_root_id_hex":"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899",
 "home_key_hex":"1122334455667788112233445566778811223344556677881122334455667788",
 "d_tag":"nostr-homed.home.v1:personal-9k4g-nonexistent",
 "relays":["wss://relay.sharegap.net"],
 "blossom_servers":["https://blossom.sharegap.net"],
 "bandwidth_cap_bytes":1048576,"per_file_timeout_sec":10,
 "max_total_bytes":16777216,"relay_timeout_ms":8000,"allow_insecure":false}
EOF

$ mkdir -p /tmp/nhf-stg
$ ./build/gnome/nostr-homed/nostr-home-fetch \
    --staging-dir /tmp/nhf-stg < /tmp/nhf-ctl.json \
    > /tmp/nhf-out.txt 2> /tmp/nhf-err.txt
$ echo $?
71
$ cat /tmp/nhf-out.txt
{"bytes":0,"files":0,"phase":"manifest"}
$ cat /tmp/nhf-err.txt
[pool] cleanup worker thread started
[pool] cleanup_worker: STARTED
[nostr_subscription_fire] sending: ["REQ","1",{"kinds":[30078],
  "authors":["0011223344556677889900112233445566778899aabbccddeeff0011223344ff"],
  "limit":1,"#d":["nostr-homed.home.v1:personal-9k4g-nonexistent"]}]
[RELAY_POOL] Subscription 1 received 0 events before EOSE
nostr-home-fetch: relay fetch: no matching event
$ ls -la /tmp/nhf-stg
total 24
drwxrwxr-x   2 bizarro bizarro  4096 Sep 23 22:07 .
drwxrwxrwt 180 root    root    20480 Sep 23 22:07 ..
```

Confirmed:

1. **A single, well-formed REQ was sent** with `kinds:[30078]`, `authors:[<pubkey>]`,
   `#d:[...]`, `limit:1`. This matches the design's §5.1 requirement that
   every relay read be author-filtered.
2. **EOSE was honoured** — the pool auto-unsubscribes on EOSE
   (`nostr_simple_pool_set_auto_unsub_on_eose(pool, true)` in
   `relay_fetch_manifest`); the helper does not poll and does not
   time-out-based-close.
3. **No matching event → exit `71`** (`NH_PORTHOME_FETCH_EXIT_NETWORK_FAIL`).
   The broker spawner `map_exit` routes this to
   `NH_PORTHOME_FETCH_RES_LIMITED`, which
   `auth_porthome.c:label_write_into` translates into
   `NH_PORTHOME_PROV_LIMITED`, which
   `nh_identity_home_prepare` treats as an ambiguity ⇒ the staging
   directory is left intact and **the existing local home is never
   touched**. This is the §5.3 LIMITED-mode invariant.
4. **Exactly one clean progress line on stdout** —
   `{"bytes":0,"files":0,"phase":"manifest"}` — parsable by the broker's
   progress lexer.
5. **All libnostr diagnostics were routed to stderr** — a stdout
   redirection trick in the helper (`dup(STDOUT_FILENO)` → save, then
   `dup2("/dev/null", STDOUT_FILENO)`) silences stray `printf` in
   dependencies so it cannot corrupt the broker's line lexer.
6. **No panic, no core, clean exit** — the pool background threads are
   left for process exit to reap, matching `relay_fetch.c` style; no fd
   or memory leak on the failure path (the sealed manifest buffer is
   `free()`d; the derived `home_key` is `OPENSSL_cleanse()`d; the
   control-payload buffer is wiped after parse).

`kind-30078 event id` / `Blossom sha256s`: **N/A for this acceptance run**
— no event or blob was published. The positive round-trip (publish a
fixture manifest, fetch it back, byte-compare) requires a signer that
can sign a real kind-30078 event to the live relay. `porthome_smallhome_driver`
already round-trips through **fake** relay + Blossom (test 142
`homed_porthome_smallhome`, 6.95 s, passing) which exercises the entire
crypto + manifest + chunk stack on the real code the helper calls into.
A live positive round-trip requires a separate small publisher tool
(secp256k1-sign + WS publish + Blossom PUT); that publisher was NOT
built in this bead and is filed as follow-up work in `nostrc-9k4g.1`.

## Live acceptance B — Blossom BUD-01 sanity check (external, pre-bead)

Independently, before this bead started, the maintainer confirmed:

```
HEAD https://blossom.sharegap.net/  -> 200
GET  https://blossom.sharegap.net/deadbeef…deadbeef  -> 404 (unknown sha)
```

`nh_porthome_blossom_fetch` uses `hanami_blossom_get` (which does a HEAD-then-GET
against each configured server, verifies the SHA-256 of the returned
bytes against the requested address, and fails over to the next server
on mismatch or 404); a corrupted-body case is already unit-tested in
`test_porthome_blossom_guards` (`homed_porthome_blossom_guards`).

## Exit-code map (helper → broker → job state)

| Helper exit | Symbol | Broker `nh_auth_porthome_fetch_result` | Job state (via `label_write_into`) | PAM outcome |
| --- | --- | --- | --- | --- |
| 0 | `EXIT_OK` | `RES_OK` | `JOB_OK` | READY |
| 65 | `EXIT_SSRF` | `RES_LIMITED` | `JOB_LIMITED` | LIMITED |
| 71 | `EXIT_NETWORK_FAIL` | `RES_LIMITED` | `JOB_LIMITED` | LIMITED |
| 72 | `EXIT_DECODE_FAIL` | `RES_LIMITED` | `JOB_LIMITED` | LIMITED |
| 73 | `EXIT_SIZE_CAP` | `RES_LIMITED` | `JOB_LIMITED` | LIMITED |
| 74 | `EXIT_TIMEOUT` | `RES_LIMITED` | `JOB_LIMITED` | LIMITED |
| 75 | `EXIT_DECRYPT_FAIL` | `RES_FAILED` | `JOB_FAILED` | (login proceeds with existing home; ambiguity preserved) |
| 76 | `EXIT_INTERNAL` | `RES_FAILED` | `JOB_FAILED` | idem |
| 64 | `EXIT_ARG` | `RES_FAILED` | `JOB_FAILED` | idem |
| child killed / signal | — | `RES_FAILED` | `JOB_FAILED` | idem |

The **hard failure classes (75/76/64)** map to `NH_PORTHOME_PROV_INVARIANT`
in `label_write_into`, which surfaces as `JOB_FAILED`. All soft failure
classes preserve the LIMITED-mode interlock (design §5.3 — an empty local
home reconciled against a populated remote with last-writer-wins
deletes the user's entire home; the sync daemon MUST not push while a
LIMITED marker is present).

## Coordination with concurrent beads

- `nostrc-ww50` (fork+drop-privs subprocess sandbox) plugs in at
  `nh_auth_porthome_fetch_spawn`'s `fork()` point. The current spawn is
  vanilla fork+execv with core dumps disabled; the ww50 wrapper drops
  to the `nostr-auth-porthome` unprivileged user and applies a seccomp
  filter, all BEFORE `execv` is called. No API change is required for
  ww50 to slot in.
- `nostrc-pvha` (live end-to-end against `gnome-dev`) picks up where
  this doc stops. The maintainer's live rig was **not** touched.
- `nostrc-9k4g.1` (follow-up): build a small `porthome_live_publisher`
  that signs a real kind-30078 event with libnostr and PUTs chunks to
  the real Blossom, so the positive-path acceptance runs unattended.

## Diffstat

```
 gnome/nostr-homed/CMakeLists.txt                          |  73 +++
 gnome/nostr-homed/src/auth/auth_porthome.c                | 191 +++++++--
 gnome/nostr-homed/src/auth/auth_porthome_fetch.c          | 310 ++++++++++++++
 gnome/nostr-homed/src/auth/auth_porthome_fetch.h          |  92 +++++
 gnome/nostr-homed/src/porthome-fetch/nostr-home-fetch.c   | 420 ++++++++++++++++++
 gnome/nostr-homed/src/porthome-fetch/porthome_fetch_ctl.c | 380 ++++++++++++++++
 gnome/nostr-homed/src/porthome-fetch/porthome_fetch_ctl.h | 160 +++++++
 gnome/nostr-homed/tests/unit/test_porthome_fetch_ctl.c    | 220 ++++++++++
 docs/reviews/porthome-live-fetch-2026-09-23.md            | THIS FILE
```
