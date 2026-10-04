# Independent post-landing review: W27 macOS Keychain gatefix

**Commit:** `e532b7fd` (`fix(signer,groundhog): gate failures — error mapping, test skips, dbus race`)
**Review branch/worktree:** `review/w27-macos-keychain-gatefix` at `/tmp/rv-c-gatefix`
**Date:** 2026-10-03
**Verdict: CHANGES-REQUIRED**

The D-Bus contract-test correction is valid, and Linux retains the libsecret test paths. The asserted fix for the observed Keychain status is not present or tested: the patch confuses two different OSStatus constants, then skips the two tests that exposed the status. A separate conditional path in the keyring test can still touch a Mac user's login keychain.

## Findings

### 1. Blocker — the observed `-60006` is not handled by the new mapper

The first macOS gate run records `Keychain store failed: -60006` in both `groundhog-store-key` and `groundhog-store-key-keyring` (`.git/nostrc-macos-gate-history/20261003T182413Z-69330-first-run.log`, lines 808–858 and 862–885). The new `kc_map_error()` in `gnome/groundhog/src/store/gh-store-key-keychain.c:101–114` labels `errSecNotAvailable` as `-60006` and maps it to `GH_STORE_KEY_ERROR_UNAVAILABLE`. The installed macOS SDK instead defines `errSecNotAvailable = -25291` (`Security.framework/Headers/SecBase.h:344`) and `errAuthorizationCanceled = -60006` (`Security.framework/Headers/Authorization.h:94`); `security error -60006` likewise reports cancellation. [Apple documents `errAuthorizationCanceled` as a canceled operation](https://developer.apple.com/documentation/security/errauthorizationcanceled).

Consequently, the gate's `-60006` still takes `kc_map_error()`'s default `GH_STORE_KEY_ERROR_FAILED` branch for store/clear. That generic result may be a defensible *honest* classification of a cancellation, but it is not the claimed unavailable mapping and its cause in this headless run has not been established. The new “KC-MAC-4” block in `tests/store/test_store_key_keychain.c:212–220` is only a comment; `main()` registers no error-path test. The one registered test uses a temporary keychain and covers the successful store/search path, not `-60006`.

**Required:** Correct the numeric/semantic comment and commit claim; decide and document what the public error should mean for an authorization cancellation without assuming it proves Keychain unavailability. Add a deterministic assertion for `-60006` and `-25291` (and the other mapped statuses), using an injected status/mapper test or a temporary-keychain fixture, never the login keychain. Do not treat a skipped libsecret test as evidence for a Keychain mapping.

### 2. Blocker — the macOS keyring test can still use the login keychain

`test_store_key.c:710–719` and `test_store_key_keyring.c:374–381` skip their libsecret-specific KC-4 subtests on Apple; neither explicitly constructs a libsecret backend. Here the first CTest executable passed with its final subtest marked `# SKIP`, while the keyring executable was `***Skipped` because `gnome-keyring-daemon` is not installed. This removes macOS KC-4 coverage, though not Linux coverage.

More importantly, `test_store_key_keyring.c:430–449` still registers KC-6 when `gnome-keyring-daemon` exists in `PATH` or `GH_STORE_KEY_TEST_SESSION_KEYRING=1`. Both KC-6 tests call `gh_store_key_new(NULL)` (`:133`, `:224`), which selects `GH_TYPE_STORE_KEY_KEYCHAIN` on macOS (`gh-store-key.c:718–729`) and can read/write the default login keychain. They are not guarded by the new Apple skip. This workstation had no `gnome-keyring-daemon` and the session override was unset, so **this review did not execute that unsafe path**.

**Required:** Ensure *every* macOS registration in this libsecret test executable either uses an explicitly injected libsecret backend or skips before any default-backend operation. If retaining only the temporary-Keychain test on macOS, make the lost libsecret coverage explicit rather than calling the CTest-level skip a pass.

**Linux coverage:** The normal Linux smoke gate selects both `groundhog-store-key` and `groundhog-store-key-keyring`: neither appears in `scripts/linux-gate.sh:117–132`'s exclusion regex, and `scripts/linux-gate-smoke.sh:59–63` selects all other registered tests. Its image installs `dbus-bin` and `gnome-keyring` (`scripts/linux-ci.Dockerfile:19–20`); hosted `groundhog-ci.yml:61,137–142` also builds and runs both and rejects CTest-level skips. Thus the libsecret KC-4 paths remain covered **when those Linux stages run**. I inspected that configuration but did not run Docker in this review.

### 3. Accepted — D-Bus “race” fix is a valid assertion update, not a timing bump

The earlier first-run log failed `nip55l_dbus_contract` at old line 1019, `approved != NULL` (log lines 348–352). `signer_service_g.c:827–847` returns `ERR_NOT_FOUND` if `NameOwnerChanged` already removed the pending request; otherwise a still-pending request whose sender is no longer live returns `(false)`. `on_name_owner_changed()` removes pending requests at `:971–1006`. The patch makes `test_cancelled_private_sender()` accept exactly those two responses, while still requiring the abandoned reply to be null and no allow-grant to be remembered (`test_signer_dbus_contract.c:1010–1039`). The unknown-ID assertion at `:1344–1351` now matches the existing `ERR_NOT_FOUND` service contract. No sleep, timeout, or production race behavior was changed in this commit. The focused test passed once, then passed five more sequential CTest repetitions. This is a real test expectation fix, not a timing bump; it does not itself fix or exercise any Keychain error mapping.

## Local verification and safety

- Ran `scripts/install-hooks.sh`; hook was already installed. All source checkout, submodule initialization, configuration, build, and test work occurred in `/tmp/rv-c-gatefix`; the main checkout's pre-existing Beads changes were untouched.
- Sourced `/tmp/nostrc-macos27-env.sh` before configuration/build and tests. Configured with `-DBUILD_GROUNDHOG=ON` and built `test-groundhog-store-key`, `test-groundhog-store-key-keyring`, `test-groundhog-store-key-keychain`, and `test_nip55l_dbus_contract` successfully.
- Ran the three requested tests serially: `nip55l_dbus_contract` **Passed** (14.19 s); `groundhog-store-key` **Passed**, but `/store-key-libsecret/kc4-no-session-bus` was `# SKIP`; `groundhog-store-key-keyring` **Skipped**, with `/store-key/kc4/bus-without-secret-service` marked `# SKIP` and no KC-6 daemon available. CTest nevertheless summarized “100% tests passed, 0 tests failed out of 3.” Full output is in this worktree's ignored `_build/w27-three-tests.log`.
- Ran `groundhog-store-key-keychain` separately: **Passed** (one registered test, temporary keychain via `SecKeychainCreate` and `gh_store_key_keychain_set_keychain`). `nip55l_dbus_contract` used its env-only test identity, disabled migration, and skipped Mac login-Keychain round trips. No test in this review accessed the login keychain.

**Version assessment:** The Groundhog production error-mapping change is PATCH-class (no new public API), absorbed into its already-declared unreleased `0.12.0`; no additional version bump for this commit. The nip55l changes are test-only, so no nip55l bump. Reassess Groundhog's version impact when remediating the mapping.
