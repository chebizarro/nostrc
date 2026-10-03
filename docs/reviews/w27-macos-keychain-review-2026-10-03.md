# Review: W27 slice C — macOS Keychain, CreateProfile, expired approval

**Branch:** `signer/w27-macos-keychain`
**Tip:** 8abfbe4a
**Base:** master 3b6bcc57
**Beads:** nostrc-oh0s, nostrc-jvbl, nostrc-dsoz
**Reviewer:** Claude Opus 4.6 (independent)
**Date:** 2026-10-03

## Verdict: CHANGES-REQUIRED

One blocker (test writes to the user's login keychain despite CMakeLists
claiming a temporary keychain) and one high finding (nip55l version not
bumped for the 0.6.0 methods). The rest of the implementation is solid:
the Keychain backend is well-structured, the default-backend selection
correctly fixes oh0s, CreateProfile is properly gated and key material
is wiped, and the expired-approval handling has no double-decision bugs.

---

## Findings

### B1 — BLOCKER: Test uses login keychain, not temporary keychain

**File:** `gnome/groundhog/tests/store/test_store_key_keychain.c` (entire file)
**Also:** `gnome/groundhog/CMakeLists.txt:721-722`

**Description:** The CMakeLists comment (lines 721-722) states:

> KC-MAC: GhStoreKeyKeychain on a temporary keychain (macOS only).
> CRITICAL: never touches the user's login keychain.

But `test_store_key_keychain.c` uses bare `SecItemAdd`/`SecItemDelete`
with no `SecKeychainCreate` or `kSecUseKeychain` — all operations go to
the user's real login keychain. The cleanup function (`cleanup_test_items`)
deletes by service+account, which limits blast radius, but:

- The brief explicitly requires "Tests never touch the login keychain
  (temporary keychain or equivalent)."
- If the test crashes before cleanup, synthetic items persist in the
  user's keychain.
- CI machines share a login keychain; parallel test runs could collide.
- `atexit(cleanup_test_items)` does not fire on `SIGKILL`/`SIGABRT`.

The `try_create_profile_and_list` test in `test_signer_dbus_contract.c`
(lines 2044-2048) is correctly `#ifdef __APPLE__` skipped with an
explicit "macOS Keychain is the login keychain" message, acknowledging
the same gap.

**Failure scenario:** `ctest` writes private-key-shaped data to the
user's login keychain on every run. A crash leaves orphaned items.
The CMakeLists comment misleads future reviewers into thinking this is
safe.

**Fix:** Create a temporary keychain with `SecKeychainCreate`, set
`kSecUseKeychain` in all queries, and delete the keychain file in
cleanup. Alternatively, use the Keychain Services `kSecAttrAccessGroup`
with a test-only group on a file-backed keychain. Update or remove the
CMakeLists comment to match reality until the fix lands.

---

### H1 — HIGH: nip55l version not bumped to 0.6.0

**File:** `nips/nip55l/include/nostr/nip55l/signer_ops.h:27-30`

**Description:** The D-Bus introspection XML
(`nips/nip55l/dbus/org.nostr.Signer.xml:260,276`) documents both
`CreateProfile` and `ListIdentities` as "nip55l 0.6.0" methods. The
header adds the `nostr_nip55l_list_identities` prototype. But the
version macros remain at 0.5.1:

```c
#define NOSTR_NIP55L_VERSION_MAJOR 0
#define NOSTR_NIP55L_VERSION_MINOR 5
#define NOSTR_NIP55L_VERSION_PATCH 1
#define NOSTR_NIP55L_VERSION_STRING "0.5.1"
```

**Failure scenario:** A client checking `NOSTR_NIP55L_VERSION_*` to
decide whether `CreateProfile`/`ListIdentities` are available will
conclude they are not, even though the methods are compiled in.

**Fix:** Bump to `0.6.0` with a changelog note in the version comment
block covering CreateProfile, ListIdentities, and the Keychain backend.

---

### M1 — MEDIUM: No explicit kSecAttrSynchronizable = kCFBooleanFalse

**Files:**
- `gnome/groundhog/src/store/gh-store-key-keychain.c:239` (store)
- `nips/nip55l/src/core/signer_ops.c:997-1005` (store_key Keychain branch)

**Description:** Neither the GhStoreKeyKeychain backend nor the
signer_ops.c Keychain store code explicitly sets
`kSecAttrSynchronizable` to `kCFBooleanFalse`. The default behaviour
is not to sync (correct), but Apple's documentation recommends explicit
setting for security-sensitive items. Without it, if a user or MDM
policy enables "sync all" on the Keychain access group, private keys
could be uploaded to iCloud Keychain.

**Failure scenario:** On a device with aggressive iCloud Keychain sync
policies, Nostr private keys stored without an explicit
`kSecAttrSynchronizable = kCFBooleanFalse` could be synced to iCloud.

**Fix:** Add `CFDictionarySetValue(q, kSecAttrSynchronizable,
kCFBooleanFalse)` to both store paths and the search queries (a query
without `kSecAttrSynchronizable` returns both syncable and
non-syncable items by default, which is fine for search, but the store
should be explicit).

---

### M2 — MEDIUM: CFData from Keychain not zeroed before release

**File:** `gnome/groundhog/src/store/gh-store-key-keychain.c:74-81`

**Description:** In `kc_fetch_secret`, the secret data is correctly
copied into sodium-guarded memory via `gh_store_key_secret_new` (line
79). However, the `CFDataRef` returned by `SecItemCopyMatching` is
released with `CFRelease(result)` (line 80), which does not guarantee
zeroization of the underlying buffer. The 32-byte private key may
persist in heap memory until the page is reused.

**Failure scenario:** A process memory dump (core dump, swap-to-disk,
or forensic tool) after the `CFRelease` could recover the private key
from the freed CFData buffer.

**Fix:** This is a known limitation of the SecItem API — `CFData` does
not support secure erasure. Document the limitation with a comment.
Consider copying to a `sodium_malloc`'d buffer immediately and zeroing
the CFData bytes via `memset_s`/`explicit_bzero` on the
`CFDataGetBytePtr` before `CFRelease` (this works because the CFData
is the sole owner post-`SecItemCopyMatching`).

---

### L1 — LOW: kSecAttrAccessibleAfterFirstUnlock is the right choice but deserves a comment

**Files:**
- `gnome/groundhog/src/store/gh-store-key-keychain.c:239`
- `nips/nip55l/src/core/signer_ops.c:1005`

**Description:** Both store paths use `kSecAttrAccessibleAfterFirstUnlock`,
which means keys remain accessible after screen lock. The stricter
`kSecAttrAccessibleWhenUnlocked` would protect against physical attacks
on a locked device, but would break the background daemon (which must
sign events while the screen is locked). The current choice is correct
for a daemon; a brief comment explaining the tradeoff would help future
reviewers.

**Failure scenario:** None (design choice, not a bug). A future
contributor might "harden" this to `WhenUnlocked` and break background
signing.

**Fix:** Add a one-line comment: `/* AfterFirstUnlock: daemon needs
access while screen is locked; WhenUnlocked would break background
signing. */`

---

### N1 — NIT: signer_ops.c store_key uses different service name from GhStoreKeyKeychain

**Files:**
- `nips/nip55l/src/core/signer_ops.c:991` — `"Gnostr Identity Key"`
- `gnome/groundhog/src/store/gh-store-key-keychain.c:91` — `GH_STORE_KEY_SCHEMA_NAME`

**Description:** The daemon's store_key uses the service name `"Gnostr
Identity Key"` while the Groundhog backend uses `GH_STORE_KEY_SCHEMA_NAME`.
These are intentionally different schemas (the daemon stores the signing
identity; Groundhog stores the per-account encryption key). This is
correct but could confuse readers. A brief comment in signer_ops.c
noting "this is the daemon's identity schema, distinct from Groundhog's
store key schema" would clarify.

---

## Positive findings

- **oh0s fixed correctly.** The CMake build (`nips/nip55l/CMakeLists.txt:38`)
  skips `pkg_check_modules(LIBSECRET ...)` entirely on Apple, so the daemon
  only compiles the `NIP55L_HAVE_KEYCHAIN` branches. `gh_store_key_new(NULL)`
  selects the Keychain backend on `__APPLE__`. `gh_identity_list()` uses the
  D-Bus `ListIdentities` method on macOS instead of searching the Secret
  Service directly. Homebrew libsecret cannot hijack the identity store.

- **Two-step Keychain fetch pattern is correct.** The
  `kc_search_in_thread` function (gh-store-key-keychain.c:84-166) fetches
  attributes-only with `kSecMatchLimitAll`, then fetches data per-item with
  `kSecMatchLimitOne`. This correctly works around the macOS 26 restriction
  where `kSecReturnData + kSecMatchLimitAll` returns `errSecParam`.

- **CreateProfile (jvbl) is properly gated.** `handle_create_profile`
  checks `signer_mutations_allowed()` (env flag), `rate_limit_ok()`,
  generates via `nostr_key_generate_private()`, stores via
  `nostr_nip55l_store_key()`, wipes sk_hex with `secure_wipe`, and returns
  only the npub. The D-Bus XML signature `(ssssb) → (bs)` matches the
  handler. The test `test_create_profile_denied_without_flag` verifies the
  gate.

- **ListIdentities returns only npubs, never secrets.** The Keychain
  branch (signer_ops.c:1107-1157) queries with `kSecReturnAttributes`
  only (no `kSecReturnData`), extracts the npub from the comment/account
  field, and filters for `npub1`-prefixed strings. No secret material
  touches the return path.

- **Expired approval handling (dsoz) has no double-decision bugs.** The
  timer (`on_request_expired_timer`) sets `expiry_timer_id = 0`, nulls the
  dialog callback before `adw_dialog_force_close`, removes the pending
  entry, shows the expiry alert, and frees the context. `approve_ctx_free`
  removes the timer if still active. `approve_call_done` checks for
  `Error.NotFound` → expired → shows alert instead of import dialog. The
  305 s grace (300 s daemon TTL + 5 s) is correct. The
  `test_expired_approve_request` test (test_signer_dbus_contract.c:1494)
  uses `NOSTR_SIGNER_TEST_PENDING_TTL_S=1` for a fast check and verifies
  `Error.NotFound`.

- **D-Bus XML consistent with handlers.** Both `CreateProfile` and
  `ListIdentities` are registered in the `signer_export` handler table
  (signer_service_g.c:1336-1337) and match the XML signatures.

- **errSecItemNotFound correctly distinguished from other errors.** Both
  the Keychain backend (gh-store-key-keychain.c:119) and the signer_ops.c
  clear/search paths treat `errSecItemNotFound` as a non-error (empty
  result or success for delete).

## Build & test results

- **Build:** CMake + Ninja, `BUILD_GROUNDHOG=ON`, Debug — clean (only
  harmless linker dup-library warnings).
- **check-unsequenced-args.py:** PASS — no unsequenced modifications.
- **groundhog-store-key-keychain:** PASS (0.17 s) — stores/searches/clears
  in the login keychain (the test itself works, it just shouldn't use the
  login keychain).
- **nip55l_keychain_migration:** PASS (0.16 s).
- **nip55l_dbus_contract:** FAIL at line 1019 in
  `test_cancelled_private_sender` — **pre-existing** failure (function not
  modified in this branch; same failure on master). The new tests
  (`test_create_profile_denied_without_flag`, `test_expired_approve_request`)
  pass.
- **nip55l_dbus_contract_fail_teardown:** PASS.

## Summary

| # | Severity | Title | Status |
|---|----------|-------|--------|
| B1 | Blocker | Test uses login keychain | Must fix |
| H1 | High | nip55l version not bumped to 0.6.0 | Must fix |
| M1 | Medium | No explicit kSecAttrSynchronizable = kCFBooleanFalse | Should fix |
| M2 | Medium | CFData not zeroed before release | Should fix or document |
| L1 | Low | AfterFirstUnlock needs explanatory comment | Nice to have |
| N1 | Nit | Different service names could confuse readers | Nice to have |

---

## Addendum — fix commit 97ab7404 verified

**Fix commit:** 97ab7404
**Reviewed:** 2026-10-03

The author addressed all six findings in a single commit. Verification
of each:

### B1 — FIXED ✅ (Blocker → resolved)

The test now creates a temporary file-based keychain:

1. `create_temp_keychain()` calls `mkdtemp` + `SecKeychainCreate` +
   `SecKeychainUnlock` — creates a new keychain file at
   `$TMPDIR/kc_test_XXXXXX/test.keychain` with password "test".
2. `gh_store_key_keychain_set_keychain(kc_backend, temp_keychain)` injects
   the temporary keychain into the backend via a new public API.
3. The backend's new `kc_scope_query()` adds `kSecUseKeychain` for
   `SecItemAdd` and `kSecMatchSearchList` for `SecItemCopyMatching`/
   `SecItemDelete`, scoping all operations to the temp keychain.
4. `destroy_temp_keychain()` calls `SecKeychainDelete` + `CFRelease` +
   `unlink` (including the `-db` sidecar) + `rmdir`.
5. `atexit(destroy_temp_keychain)` ensures cleanup on abort.

**Verification:**
- Ran `ctest -R store-key-keychain`: PASS (0.14 s).
- After the test: `security find-generic-password -a "000...001"` →
  "item could not be found" (nothing leaked to login keychain).
- `ls /tmp/kc_test_*` → no matches (temp dir cleaned up).
- The `#pragma clang diagnostic ignored "-Wdeprecated-declarations"`
  correctly suppresses warnings for the deprecated `SecKeychainCreate`/
  `SecKeychainUnlock`/`SecKeychainDelete` APIs (the new `kSecUseKeychain`
  query key routes to the same implementation and is not deprecated).

The backend's struct now carries `SecKeychainRef keychain` with proper
retain/release in `set_keychain` and `dispose`. The `GhStoreKeyKeychain`
is a GObject, so `dispose` is the correct cleanup point.

### H1 — FIXED ✅ (High → resolved)

Version macros bumped from 0.5.1 to 0.6.0 in `signer_ops.h:27-30`.
`VERSION_MANIFEST.md` updated to match.

**Minor nit:** The version comment block (lines 10-26) documents 0.2.0
through 0.5.1 but has no 0.6.0 entry for CreateProfile/ListIdentities.
The D-Bus XML documents them as 0.6.0, which is sufficient; adding a
line to the comment block would complete the trail.

### M1 — FIXED ✅ (Medium → resolved)

`kSecAttrSynchronizable = kCFBooleanFalse` added to every SecItem
query across all three files:

- `gh-store-key-keychain.c`: search, per-item fetch, store, clear.
- `signer_ops.c`: resolve_seckey_hex (2 sites), store_key, clear_key,
  list_identities, kc_query (migration helper).
- `secret_store.c`: secret_store_list, has_secret, get_secret,
  lookup_by_fingerprint.

All Keychain paths are now explicit about sync prevention.

### M2 — FIXED ✅ (Medium → resolved)

`memset((void *)bytes, 0, (size_t)blen)` added before every
`CFRelease` of a CFData containing key material:

- `gh-store-key-keychain.c:137-138` — `kc_fetch_secret`, after
  copying to sodium-guarded memory.
- `signer_ops.c:223` — `resolve_seckey_hex`, Keychain data fetch.
- `signer_ops.c:1019-1023` — `store_key`, the CFDataCreate'd secret.
- `signer_ops.c:1592-1594` — `kc_store_signer` (migration helper).
- `signer_ops.c:1641-1643` — `kc_migrate_one`, legacy client key.
- `secret_store.c:630-632` — `get_secret`, Keychain data fetch.

**Note:** `memset` (not `secure_wipe`/`explicit_bzero`) is used. In
these specific call sites the wipe is safe from compiler elision because:
(a) the pointer comes from an opaque function (`CFDataGetBytePtr`), and
(b) `CFRelease` follows immediately (also opaque) — the compiler cannot
prove the buffer is dead between the two opaque calls. Formally,
`explicit_bzero` would be stronger; practically, the current `memset`
is equivalent here.

**One gap:** `gh-store-key-keychain.c:313` (`kc_store_in_thread`)
releases `cf_data` without wiping. This CFData was created from the
caller-supplied GBytes secret. The signer_ops.c store path wipes its
CFData; the Groundhog backend's does not. The risk is lower (the source
GBytes is in sodium-guarded memory and will be zeroed on free), but
consistency would be better. Not blocking.

### L1 — FIXED ✅ (Low → resolved)

The `kSecAttrAccessibleAfterFirstUnlock` tradeoff is documented in the
file header comment (gh-store-key-keychain.c lines 8-13):

> chosen over the more restrictive kSecAttrAccessibleWhenUnlocked so
> that the background daemon can access keys after the first login
> without requiring the screen to be unlocked

### N1 — ADDRESSED ✅ (Nit → resolved)

The author added `KC_SIGNER_SERVICE` define (signer_ops.c:39) with a
comment cross-referencing `secret_store.c GNOSTR_KC_SERVICE`, and vice
versa. Both files now use named constants instead of inline literals.

### Additional observation

The libsecret `list_identities` path (signer_ops.c:1098) was fixed from
`&gnostr_secret_identity_schema` to `&gnostr_secret_schema`. This
appears to be a separate bug fix — the identity schema may not have
existed as a global, causing compilation or search failures on Linux.

## Build & test results (97ab7404)

- **Build:** Clean (Ninja, Debug, BUILD_GROUNDHOG=ON).
- **check-unsequenced-args.py:** PASS.
- **groundhog-store-key-keychain:** PASS (0.14 s) — temp keychain,
  login keychain verified clean after.
- **nip55l_keychain_migration:** PASS (0.18 s).
- **nip55l_dbus_contract:** FAIL at line 1019 — pre-existing
  (`test_cancelled_private_sender`, not modified in this branch).
- **Sanitizer gate (linux-gate.sh --sanitizers):** PASS, 54 tests,
  only groundhog-mls-service needed a pre-existing flaky rerun.

## Final verdict: **APPROVE**

All blocker and high findings are fixed. Medium findings are fixed.
The one remaining nit (missing cf_data wipe in `kc_store_in_thread`) is
non-blocking — the source buffer is in sodium-guarded memory and the
risk is minimal. The implementation is solid and ready to merge.
