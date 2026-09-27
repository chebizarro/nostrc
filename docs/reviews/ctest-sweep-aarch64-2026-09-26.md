# Full ctest sweep — aarch64, freestanding -Werror (nostrc-m4y1)

**Date:** 2026-09-26 · **Branch:** `fix/libnostr-dav-packaging-followups` at `a91728c9` (master `f0de4260` merged, incl. relayd `WITH_NOSTRDB=ON`)
**Host:** aarch64 lab (Ubuntu 24.04, GCC 13, libwebsockets 4.3.3, libgit2 1.7.2), isolated checkout `~/nostrc-ci/wD-core/src`
**Result:** configure OK · **1226/1226 targets built, 0 failures** under `-DCMAKE_C_FLAGS=-Werror` · **ctest 328 passed / 4 failed / 1 skipped of 333**

## Configuration

The debian/rules feature set with testing turned on and no relaxations
(no `DEB_CFLAGS_MAINT_APPEND`, no `-Wno-error`):

```
cmake -G Ninja ../src -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Werror \
  -DBUILD_SHARED_LIBS=ON -DNOSTR_WITH_GLIB=OFF -DBUILD_APPS=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DBUILD_LIBHANAMI=ON \
  -DBUILD_RELAYD=ON -DWITH_NOSTRDB=ON -DLIBNOSTR_WITH_NOSTRDB=OFF \
  -DENABLE_NOSTR_{DAV,DISPATCHER,SEAL,SHARE,SEARCH_PROVIDER,SIGNER_WEBEXT_HOST,
                  WALLET_AGENT,NAUTILUS,SETTINGS,SHELL_EXTENSION}=ON \
  -DENABLE_NOSTR_HOMED=ON -DNOSTR_HOMED_ENABLE_*=ON (as debian/rules) \
  -DBUILD_TESTING=ON -DNOSTR_HOMED_BUILD_TESTS=ON
ctest -j6 --timeout 300 --output-on-failure
```

Every test executable now runs with live `assert()` (`-UNDEBUG`, see
nostrc-xw95): before this sweep the Release build compiled bare asserts to
nothing, so a green run did not mean the checks executed.

## Matrix

| Area | Tests | Pass | Fail | Skip | Notes |
| --- | ---: | ---: | ---: | ---: | --- |
| libgo (`Go*`) | 25 | 25 | 0 | 0 | incl. new zero-capacity array cases (nostrc-6tuz) |
| libnostr (`libnostr_*`) | 8 | 8 | 0 | 0 | incl. new `subscription_dispatch` (prqu.4), `filter_limit_zero` (pnc7) |
| `test*` (tests/ tree, nips/*, libhanami) + concurrency/profile/glib | 144 | 143 | 1 | 0 | `test-hanami-blossom-batch` (nostrc-yk3t) |
| relayd | 9 | 9 | 0 | 0 | storage-enabled: `session_relay_storage`, `_ws`, `req_contract` |
| nostr-dav | 6 | 6 | 0 | 0 | 44+18+10+3 sub-cases incl. REQ/upstream/ETag/TZID |
| nostr-homed (`homed_*`) | 86 | 83 | 2 | 1 | see failures; `porthome_broker_wait` skips (needs root) |
| desktop (`nostr-*` share/settings/search/seal/wallet/shell-ext, notify) | 50 | 49 | 1 | 0 | `nostr-shell-extension-monitors` (nostrc-twwl) |
| nip55l | 5 | 5 | 0 | 0 | |
| **Total** | **333** | **328** | **4** | **1** | |

## Failures (all pre-existing / environmental; filed, not fixed)

| Test | Symptom | Cause | Bead |
| --- | --- | --- | --- |
| `homed_identity_store` | NH_CHECK `schema_version == 2` at test_identity_store.c:95 | authority schema is now v3; test not updated. NH_CHECK was always live → predates this work | nostrc-ohrz |
| `test-hanami-blossom-batch` | assert `put_count == 4` at test_hanami_blossom_batch.c:524 | **hidden failure exposed by live asserts** (was a no-op under NDEBUG) | nostrc-yk3t |
| `homed_smb_conf_standalone_testparm` | testparm rc 1: state dir `/var/lib/nostr-auth/samba-state` missing; netbios name > 15 chars | test depends on packaged host dirs | nostrc-bvka |
| `nostr-shell-extension-monitors` | wallet cases 26–30 time out on the budgets.json file monitor | passed in the first sweep, failed 3/3 an hour later with identical sources on the shared lab; environmental | nostrc-twwl |

## Fixed while getting here

First sweep (before the fixes below) was 6 failures of 330:
`test_nip47_spec_source`, `test_nip49_spec_source` (opened `SPEC_SOURCE`
relative to the build cwd; now run from the repo root) and
`gnostr_valgrind_crash_test` ("Not Run": registered with `BUILD_APPS=OFF`;
now gated) — all fixed in nostrc-xw95. The freestanding `-Werror` build
itself went from 45 failing objects to 0 (xw95). New regression tests from
this branch (prqu.4, ir7c, wr3t, 862u, w8y1, iq04, pnc7, 6tuz) are all green.

## Reproduce

```
~/nostrc-ci/wD-core/build-final$ ctest -j6 --timeout 300 --output-on-failure
```
Logs: `~/nostrc-ci/wD-core/{configure-final,build-final,ctest-final}.log` on the lab.
