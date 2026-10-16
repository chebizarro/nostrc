# Phase 2 — native root QA at d9e8c6294

**Tracker:** `nostrc-0cfsg.5`, left in progress. **Source:** `d9e8c6294a35f04d0c73870d4d27f28990759a24` (`ci/dependency-tests-integration-20261015`), fast-forwarded into the assigned QA worktree; clean source during execution. **Date:** 2026-10-15 PDT / 2026-10-16 UTC.

**Result:** native root configure and all-default-target build exited 0. Unfiltered, first-attempt full CTest exited 0: **437 unique registrations, 432 passed, 5 skipped, 0 failed/errors/disabled**. Skips are not passes or GUI acceptance. No retry, quarantine, production change, default cutover, final Oracle/peer review, merge back to integration, or push occurred. Documentation-only changes require **no component bump**.

## Exact configuration and isolation

- Host: macOS 27.0.1 arm64; Apple clang 21.0.0 (`clang-2100.3.34.2`), `/usr/bin/cc` and `/usr/bin/c++`; CMake/CTest 4.3.2, Ninja 1.13.2, Python 3.14.8 with GI/GLib 2.90. Tools resolved through the existing PATH; no tool installation/settings change.
- Fresh, private run directory: `/private/tmp/nostrc-0cfsg5-native-20261015.m29vg74z` (`RUN` below); build at `$RUN/build`, outside all worktrees. No shared `_build`, sibling resource, Docker container/image, or shared cache was cleaned.
- Required submodules initialized at recorded commits: nostrdb `2d459e8fa9ba0af5af091c37920e5d2078d881ca`, nsync `5b420ef6b16532a4fad4be1fde8e483b6b7bd8f4`; optional `docs/nips` not initialized. No gitlink change.
- Wrapper environment: `HOME=$RUN/home`, `XDG_CONFIG_HOME=$RUN/xdg-config`, `XDG_DATA_HOME=$RUN/xdg-data`, `XDG_CACHE_HOME=$RUN/xdg-cache`, `XDG_RUNTIME_DIR=$RUN/xdg-runtime` (mode 0700), `GSETTINGS_BACKEND=memory`, umask 077. Registered per-test isolation additionally supplies private HOME/XDG paths below the build tree.
- Private bus address: `unix:path=/private/tmp/nostrc-0cfsg5-native-20261015.m29vg74z/bus,guid=376a92be8e903156846903a16ad170bc`. Explicit Unix-socket session config, `EXTERNAL` auth, repository test-bus allow-own/send/eavesdrop policy (`tests/common/nostrc-test-bus.c:219-237`), **no standard activation directories**. No use of the developer's session bus.
- Throwaway Secret Service: `python3 -u "$SRC/gnome/groundhog/tests/app/test_secret_service.py" "$RUN/secrets.json"`, started only inside that bus/private HOME. `gdbus wait --session --timeout 15 org.freedesktop.secrets` awaited ownership; `GetNameOwner` returned `:1.1`. This is the repository's plaintext, test-identities-only service, not real keyring/provider acceptance. Cleanup terminated only this service PID; `dbus-run-session` owned bus lifetime.
- Inherited `DISPLAY=:0`; `GDK_BACKEND` unset. `CC`, `CXX`, `CFLAGS`, `CXXFLAGS`, `CPPFLAGS`, `LDFLAGS`, sanitizer options, `CTEST_PARALLEL_LEVEL`, and inherited backend overrides were unset. No UI-skip or quarantine override was added by the wrapper. Existing test registrations set UI skips as described below.
- `scripts/check-macos-keychain.sh` ran read-only with the original HOME before and after execution: both exit 0. No default/search-list mutation or repair was attempted. The registered `nip55l_keychain_migration` test passed using its existing throwaway-keychain/reference guard; this is not a test against the developer's login keychain.

Commands inside the private session (existing CMake/CTest interfaces, **not** proposed selective-gate commands):

```sh
cmake -S "$SRC" -B "$RUN/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DNIP55L_SECRET_BACKEND=libsecret \
  -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service
ctest --test-dir "$RUN/build" --show-only=json-v1
cmake --build "$RUN/build" --parallel 4
ctest --test-dir "$RUN/build" --parallel 1 --output-on-failure \
  --output-junit "$RUN/ctest.xml"
```

`BUILD_TESTING=ON`, `BUILD_TESTING_FRAMEWORK=ON`, `BUILD_APPS=ON`, `SIGNET_ENABLE=ON`, **`BUILD_GROUNDHOG=OFF`** remained root defaults. Signet passkeys/test hooks were OFF. No target restriction or CTest selection filter was supplied. Groundhog's backend argument was unused (CMake warning) because Groundhog was not enabled; **Groundhog build/tests were NOT RUN**. Optional missing hidapi/WebKitGTK and disabled plugin dependencies were reported by configure, not silently turned into coverage. No sanitizer instrumentation was requested.

## Execution receipt

| Phase | UTC start → end | Exit | Measured subprocess seconds |
|---|---|---|---|
| Configure | 00:33:01.009 → 00:33:09.727 | 0 | 8.717 |
| Fresh registration enumeration | 00:33:09.727 → 00:33:09.784 | 0 | 0.057 |
| Build, all default targets | 00:33:09.784 → 00:33:58.144 | 0 | 48.360 |
| Full serial CTest | 00:33:58.145 → 00:39:28.082 | 0 | 329.938 |

JUnit cases were compared against the freshly enumerated names with exact multiset equality: **437 expected = 437 unique observed**, including 7 fixture-setup registrations (no fixture-cleanup registrations). This checks CTest result accounting, **not policy-inventory membership, impact edges, selected-check semantics, or deduplicated logical-check counts**. CTest's summary prints 329.90 seconds; JUnit rounds suite time to 329 seconds. These are one uncontrolled native sample, not the representative clean/incremental cost matrix or a savings claim.

### Actual non-pass outcomes

All five are JUnit `status=notrun`, `<skipped message="SKIP_RETURN_CODE=77">`; none is a failed case, but none establishes executed coverage.

| CTest name | Observed evidence / cause |
|---|---|
| `test_nip5f_tcp` | Output: `Skipping TCP test (no tcp endpoint/token configured)`; recommends the separate in-process loopback test. No live TCP endpoint acceptance. |
| `signer/qr-scanner` | Exit 77; registration explicitly contains `GNOSTR_SKIP_UI_TESTS=1`. Source skip guard at `apps/grotto/tests/test-qr-scanner.c:102`. TAP header alone is not QR/UI acceptance. |
| `signer/onboarding` | Exit 77 with empty output; same registered skip environment; Apple guard at `apps/grotto/tests/test-onboarding.c:477-479`. |
| `signer/ui-layout` | Output: `1..0 # SKIP native UI disabled`; same registered environment. |
| `signer/ui-accessibility` | Output: `1..0 # SKIP native UI disabled`; same registered environment. |

The Apple UI environment originates at `apps/grotto/tests/CMakeLists.txt:225-227`; it was **not** added to make this run green. A future selected blocking GUI check cannot accept these skips. No code failure needs a production fix from this run; skip/coverage constraints remain visible for parent integration.

### Preflight failure retained, not hidden

Before any configure/build/test, plain `dbus-run-session` failed with exit 127: `Failed to start message bus: Check-in failed: No such process`, then `EOF reading address from bus daemon`. Homebrew's `/opt/homebrew/share/dbus-1/session.conf:15` listens via launchd. A private Unix-socket config replaced that unsuitable **temporary invocation**, not the installed config. Service ownership was then verified before the single configure/build/CTest attempt. The failed preflight's after-keychain guard also exited 0. No safety check was bypassed and no test was retried.

## Evidence location and remaining limits

Local evidence remains under `RUN`: `receipt.json` (SHA, commands, timestamps, environment), `tools-and-inherited-environment.json` (tool commands/versions, selected inherited variables and source status), `registrations.log` (CTest JSON), `configure.log`, `build.log`, `ctest.log`, `ctest.xml`, `outcomes.json`, `keychain-before.log`, `keychain-after.log`, `bus.conf`, `secret-service-owner.txt`, wrapper scripts, and `preflight-launchd-failure.json`. These temporary files are not durable external CI artifacts; the bounded receipt above preserves the result and exact source identity. SHA-256 anchors:

```text
73d0a3693cb489c70305253e6b5767d1b4d2b3f06dabf136aa1e8ac6515c445c  configure.log
ec8ad1af5dd55698c999bcc293dfc929c60a2f4117dbb1af60d878d226f6bf9f  build.log
275b03e496199ef7060c1b073a7e3ef0eaf1cd11b18ed96a80c017cdf870ec9c  registrations.log
616c9b0e1437731667fde3daff00d498b29c0b481214763f13488ae6b95e22c2  ctest.log
3434ddb884acd5ae5b48aae817076df9db97f9dde8d0733e7e41ceeee07231be  ctest.xml
8aabef499762e663b6f8d6e496aae5380f36866cdd0ea58ecb3cd6dded2f06a2  receipt.json
ac9522030b20c7bd8da4b907a2b1c4b77acf670dd93f20be6d67e7384afa9357  tools-and-inherited-environment.json
```

Parent reports 112 Python tests passed independently and zero-unowned probes for root and the 433-registration Groundhog configuration. Those are **parent-reported**, not rerun or promoted to selector-audit acceptance here. Exact policy membership and dependency-edge gaps still block audits; macOS native profile support is pending `.4`. `.3` local adapters and `.4` workflows/inventory are not accepted as integrated by this run. Final integrated Oracle/peer review is explicitly **DEFERRED** until parent steers after C/D integration; no Oracle routing retries were made.

W1 representative measurements, W11 **20 PR / 2 broad integration runs**, release replacement proof, selected-skip/cancellation/merge-group checks and no-gap protection cutover remain outstanding. W12 defaults/schedule cleanup remains NOT RUN. No live MDK, installed-desktop/manual, public-relay or cross-platform acceptance is claimed. Bead `.5` stays open; no default, protection, requirement or schedule changed. The earlier [W1 baseline](dependency-aware-testing-ci-2026-10-15-w1-baseline.md) remains a historical first-phase snapshot, not the current implementation inventory.
