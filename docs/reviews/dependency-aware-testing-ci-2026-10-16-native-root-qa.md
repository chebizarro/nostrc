# Independent native root QA — 84ae0896b

**Source tested:** `84ae0896b0abd835935c264b872e9b184d616364`, fast-forward merged into the assigned `fix/testing-main-thread-latency-20261016` worktree. **Execution:** 2026-10-16 UTC / 2026-10-15 PDT. **Tracker:** `nostrc-0cfsg.5`; acceptance work remains open. This is a new execution after the CMake helper changes, not a restatement of the [previous native-root receipt](dependency-aware-testing-ci-2026-10-15-native-root-qa.md).

**Result:** fresh native root configure **0**, all-default-target build **0**, first unfiltered full CTest **0**: **437 unique registrations; 432 passed, 5 skipped, 0 failures/errors/disabled**. Both read-only keychain guards exited **0**. Source SHA and clean Git status were captured before every phase and after CTest. No configure/build/test retry, exclusion, quarantine, skip waiver, source correction, Oracle repeat, merge back to parent, or push occurred. Only this report is committed; **no component version bump** (documentation only).

## Exact source, toolchain and safety boundary

- `SRC=/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-558dfc12-fix-testing-main-thread-latency-ca3a2a65`.
- Fresh `RUN=/private/tmp/nostrc-0cfsg5-native-20261016.2LRxvZ`; build `$RUN/build`, outside all worktrees. Run/private HOME/runtime directories mode 0700; umask 077. No shared build/cache, sibling worktree, Docker resource or developer settings were cleaned or changed.
- macOS **27.0.1**, build **26A434**, arm64; Apple clang **21.0.0** (`clang-2100.3.34.2`, Xcode toolchain), CMake/CTest **4.3.2**, Ninja **1.13.2**, Python **3.14.8**, PyGObject **3.58.0**, GLib **2.90.0**, libsecret **0.21.8.2**. CMake selected `/usr/bin/cc` and `/usr/bin/c++`.
- Initialized gitlinks: nostrdb `2d459e8fa9ba0af5af091c37920e5d2078d881ca`, nested secp256k1 `1ad5185cd42c0636104129fcc9f6a4bf9c67cc40`, nsync `5b420ef6b16532a4fad4be1fde8e483b6b7bd8f4`. Optional `docs/nips` remained uninitialized; no gitlink changes.
- Wrapper exports `HOME=$RUN/home`, `XDG_CONFIG_HOME=$RUN/xdg-config`, `XDG_DATA_HOME=$RUN/xdg-data`, `XDG_CACHE_HOME=$RUN/xdg-cache`, `XDG_RUNTIME_DIR=$RUN/xdg-runtime`, `GSETTINGS_BACKEND=memory`. Registered tests additionally use their existing private HOME/XDG isolation below the build tree.
- Explicit private D-Bus: `unix:path=$RUN/bus`; actual `DBUS_SESSION_BUS_ADDRESS` including generated GUID is in `receipt.json`. Config uses session type, `EXTERNAL` auth and the repository test-bus allow-own/send/eavesdrop policy, **without standard activation directories**. The developer session bus was not used.
- Throwaway service: `python3 -u "$SRC/gnome/groundhog/tests/app/test_secret_service.py" "$RUN/secrets.json"`, inside the private bus/HOME. `gdbus wait --session --timeout 15 org.freedesktop.secrets` awaited ownership (not a sleep); `GetNameOwner` returned `:1.1`. This plaintext fixture contains test identities only, not real Secret Service/provider acceptance. The wrapper killed/reaped only its service PID; `dbus-run-session` owned bus lifetime.
- `scripts/check-macos-keychain.sh` ran with the original developer HOME before and after the private session, both exit 0. No keychain default/search-list/settings mutation or repair command was run. `nip55l_keychain_migration` passed its existing throwaway-keychain/reference guard.
- Additional network boundary inherited by configure/build/tests: Seatbelt denies outbound IP except localhost, allowing local TCP/WebSocket fixtures and Unix sockets. Preflight proved IPv4 and IPv6 loopback connectivity and `EPERM` for an attempted connection to documentation-only address `203.0.113.1:9`. No public-relay acceptance/contact was requested.
- `DISPLAY=:0` was inherited; `GDK_BACKEND` and `GNOSTR_SKIP_UI_TESTS` were unset in the wrapper. `CC`, `CXX`, `CFLAGS`, `CXXFLAGS`, `CPPFLAGS`, `LDFLAGS`, ASan/UBSan/LSan/TSan options, `CTEST_PARALLEL_LEVEL` and inherited backend overrides were explicitly unset. `PKG_CONFIG_PATH`, `LIBRARY_PATH`, `DEVELOPER_DIR` were already unset; no toolchain workaround was sourced. Existing PATH/tool resolution is preserved in the tools receipt.

## Commands and execution

Using the previous receipt's private-session method, with the additional network profile:

```sh
# With original HOME, before and after the private session:
bash "$SRC/scripts/check-macos-keychain.sh"

# Network profile $RUN/network.sb:
# (version 1)
# (allow default)
# (deny network-outbound (remote ip "*:*"))
# (allow network-outbound (remote ip "localhost:*"))
sandbox-exec -f "$RUN/network.sb" \
  dbus-run-session --config-file="$RUN/bus.conf" -- bash "$RUN/inside-bus.sh"

# Inside the isolated session, after Secret Service ownership was verified:
cmake -S "$SRC" -B "$RUN/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DNIP55L_SECRET_BACKEND=libsecret \
  -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service
ctest --test-dir "$RUN/build" --show-only=json-v1
cmake --build "$RUN/build" --parallel 4
ctest --test-dir "$RUN/build" --parallel 1 --output-on-failure \
  --output-junit "$RUN/ctest.xml"
```

Exact argv, source identities, UTC timestamps and effective session environment are in `receipt.json`; retained wrappers/configs contain the complete startup, environment and cleanup commands. No CTest selection or repeat flag was supplied.

| Phase | UTC start → end, 2026-10-16 | Exit | Subprocess seconds |
|---|---|---|---|
| Configure | 02:37:48.711 → 02:37:56.725 | 0 | 8.014 |
| Fresh enumeration | 02:37:56.725 → 02:37:56.827 | 0 | 0.102 |
| All-default-target build | 02:37:56.828 → 02:38:43.510 | 0 | 46.682 |
| First full serial CTest | 02:38:43.511 → 02:44:09.659 | 0 | 326.148 |

Cache confirms root defaults `BUILD_TESTING=ON`, `BUILD_TESTING_FRAMEWORK=ON`, `BUILD_APPS=ON`, `SIGNET_ENABLE=ON`, **`BUILD_GROUNDHOG=OFF`**. Signet passkeys/test hooks stayed OFF. No sanitizer instrumentation was requested. Groundhog's backend argument was unused, visibly warned by CMake, because Groundhog was OFF; **Groundhog build/tests were NOT RUN**. Configure also reported missing hidapi/WebKitGTK, disabled optional nip29/Communikeys/Concord plugins and unavailable relayd upstream federation; none establishes executed coverage.

JUnit names were checked against fresh enumeration with exact multiset equality: **437 expected = 437 unique observed**, including seven fixture-setup registrations. This verifies execution accounting, **not policy-inventory completeness, selected-check semantics, profile union or deduplicated logical-check coverage**. CTest prints 326.02 seconds; subprocess timing includes overhead. One uncontrolled native sample is not a representative cost/savings baseline.

### First-run non-pass outcomes retained

No failed/error/disabled registration occurred. All five skips are JUnit `status=notrun`, `SKIP_RETURN_CODE=77`; they remain **unexecuted coverage**, not waived passes.

| Registration | Observed reason / boundary |
|---|---|
| `test_nip5f_tcp` | `Skipping TCP test (no tcp endpoint/token configured)`; no live TCP endpoint acceptance. |
| `signer/qr-scanner` | Exit 77; existing Apple registration includes `GNOSTR_SKIP_UI_TESTS=1`; source guard `apps/grotto/tests/test-qr-scanner.c:102`. |
| `signer/onboarding` | Exit 77, empty output; same registered environment; Apple guard `apps/grotto/tests/test-onboarding.c:477–479`. |
| `signer/ui-layout` | `1..0 # SKIP native UI disabled`; same registered environment. |
| `signer/ui-accessibility` | `1..0 # SKIP native UI disabled`; same registered environment. |

The existing Apple environment is set at `apps/grotto/tests/CMakeLists.txt:225–227`, not by this QA wrapper. Other passing native registrations under UI-skip environment do not establish rendered GUI behavior. These skips cannot satisfy a future blocking GUI check.

### Setup failures retained separately from tests

Before configure, nsync initialization with `--reference` failed because the reference repository was shallow; a local clone then checked out the exact gitlink. The first temporary network policy was rejected by Seatbelt's parser (`host must be * or localhost`) before any wrapped command executed; the numeric-host allow rules were replaced with supported `localhost:*`, then both loopback families and non-loopback denial were verified. Rejected policy/log remain in `RUN`. No installed settings changed, no safety check was bypassed, and no configure/build/test attempt was rerun.

## Separate bounded .11 investigation evidence — not a correction

`nostrc-0cfsg.11` remains **OPEN, cause unresolved; no correction authorized**. At failed SHA `5467c02d11f74efc1a85c833024efb654bbf98b5`, `nostr-gobject/tests/test_main_thread_forbidden_calls.c:159–176,215–219` asserts more than five callbacks and at most two inter-callback monotonic gaps strictly over 50 ms, counted across the whole observation (not necessarily adjacent); maximum gap is diagnostic, and the initial interval has no preceding timestamp. Lines 189–206 install a 10 ms timer, manually dispatch after 50 store cycles, then perform 20 existing dispatch/sleep iterations included in the same measurement. The measurement does not attribute delay to store work, another source or off-CPU scheduling.

The fixture is a unique empty local 64 MB NDB/one ingester (`tests/testkit/gnostr-testkit.c:36–65,135–151`). Calls delegate through `nostr_ndb_store.c:350–367` and `storage_ndb.c:914–998` to NDB monitor locking/allocation/try-pop/destruction, not relay I/O or `ndb_wait_for_notes`. Registration is a direct binary with timeout 90 (`nostr-gobject/CMakeLists.txt:483–487`). The failing CI log confirms overlap with `marmot_test_interop` under `ctest -j2`; **scheduler contention is plausible, not proven causal**.

Prior bounded evidence remains separately at `/tmp/nostrc-0cfsg11-4uwk7Y`: exact-failing-SHA target-only Debug ASan/UBSan build; one unchanged CTest execution passed four cases (heartbeat **9 / missed 0 / max gap 12.5 ms**) with all networking denied. A temporary record-only trace using checked Mach thread CPU accounting showed all 50 operations before the first heartbeat; all ten beats occurred during trailing dispatch, max gap 12.7 ms. Maximum subscribe/poll/unsubscribe times were 0.290/0.004/0.021 ms. A deterministic clock probe of the unchanged callback demonstrated the unobserved initial interval and strict 50 ms boundary. Instrumentation perturbs timing; neither probe explains CI. Initial `trace.log` CPU fields were unvalidated and are **not evidence**; use `trace-mach.log` only.

In **this new root run**, untruncated `LastTest.log` confirms all four latency cases executed: heartbeat **9 / missed 0 / max gap 15.0 ms**. JUnit truncates successful output at 1024 bytes, so it alone does not retain that heartbeat line. This non-sanitized serial sample is **not** reproduction of the hosted sanitizer failure or proof of harmlessness. No proven causal fixture/registration defect supports a corrective patch. Proposed next diagnostic, subject to parent direction: record operation/dispatch/sleep boundaries, heartbeat timestamps and native thread CPU time on a failing-equivalent hosted run while retaining the original assertions. Any eventual shipped correction requires separate component version assessment.

## Hash anchors and acceptance limits

New-run evidence is local/temporary, not durable external CI artifacts: logs, fresh enumeration/JUnit/outcomes, complete untruncated `LastTest.log`, source/tool/environment/command receipts, wrappers, private bus/network configs and preflight logs remain under `RUN`. `SHA256SUMS` also covers wrappers/configs/guards; both empty keychain logs accompany successful recorded exits. New-run SHA-256 anchors:

```text
ec9b76b7508d531ea4684bacb5a7f20130ff571c4fb8331d2d3d53d3f5f31818  configure.log
fa41e1eb0832dd0a85664d8604d382aecd9edd2a3625abe44dbb083c64eadf51  build.log
db7d7e726ad691c2f5dd510514584d275e63017ac029cb434f4f9a58775c1ba7  registrations.log
891dcdbb31d74706f78385f3cec33899a010a28b74db6dcb15774f7cf0b7cebf  ctest.log
c8fb9263af73866766d6a8c48c301c623670e99b403534b558477bb0d1e8c050  ctest.xml
c0ce4bc5f9bfa93b0e9f01ca4a5cf46a51498eff9d8bdeb13f998f719086ffed  LastTest.log
4f559a4ceb6763199a6d8a73eb30a291c4d04bf4c819f3c9fc5f2830685eebbb  outcomes.json
b377e6b016514e8a0666aceb90b79ffbf0d50af25baa01551c410afc69bcaf8d  receipt.json
f439faaf8a5423303e1c98213699e611d503935a52032afb1d9b17e0e3ad4aa9  tools-and-inherited-environment.json
```

Separate `.11` evidence anchors (different directory/source/execution; not part of the root result):

```text
8da683daa4ed051369a411213383c58d4b440305f58d948cbfdc0130001f4faa  ctest-exact.log
7c5680cb8e3eaea65d22a5d0187183ce2d9078007050fbc461aebdde31b05b30  trace-mach.log
bfe2e6c038324499d80cd82001a8b4f39b91b03a5022e030e3412ac7f455fece  clock-probe.log
```

This is **root-default native QA only**, not GUI/live acceptance, profile union, sanitizer parity, selector-engine/metadata acceptance, W11's representative-PR/two-broad-run proof, release dry-run/protection/merge-queue proof, or W12/operator cutover. Concurrent A/B core/lifecycle and pending D metadata work is not validated by this SHA's receipt. No default, workflow, protection, schedule or release setting changed. Parent owns integration, merge/push and remaining acceptance; `.5` and `.11` remain open.
