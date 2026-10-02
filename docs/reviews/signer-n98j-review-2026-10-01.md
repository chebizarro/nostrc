# Review: gnostr-signer approval-dialog crashes (nostrc-n98j)

- Branch: `fix/signer-keychain-secure-strdup` (base master `267779a8`)
- Reviewed commits: `287e0244`, `48f10526`, `e874de00`, `3b0ba378`
- Commits the reviewer added on the branch: `8065bbd1`, `77ca1c86`, `f5d4eaaf`, `287900d8`, `86290094`
- Reviewer: independent review, 2026-10-01

## Verdict: APPROVE, with the five added commits

The four live-smoke fixes are correct and fix what they say they fix.
The review found the same bug classes in more places; those are fixed on the
branch now:

- five more allocator mismatches, four of which crash on a path users reach;
- one more double-callback dialog;
- a re-entry path in the approval dialog;
- an unguarded `::closed` chain-up. On libadwaita 1.5 (Ubuntu 24.04) it
  SEGVs every time the approval dialog closes, including on master.

The branch also adds a regression test that fails on master. Merge
the branch as it stands (9 commits). Three follow-ups are filed below.

## 1. The four fixes

| Commit | Change | Assessment |
|---|---|---|
| `287e0244` | Keychain `get_secret` returns `gn_secure_strdup` | Correct; both backends now return `gn_secure_*` memory. Its comment named the wrong free function (`gnostr_secure_strfree`); corrected in `8065bbd1`. |
| `48f10526` | `accounts_store` frees with `gn_secure_strfree` | Correct at the time; superseded by `3b0ba378`, which stops reading the secret. |
| `e874de00` | Drop manual frees of `accounts_store_list()` entries | Correct and complete. All 8 callers are checked (approval_dialog, main_app, settings_page ×4, sheet-select-account, sheet-create-multisig), and none frees an entry now. `accounts_store_find()` has no callers. The header said "caller owns array; use accounts_store_entry_free", which invites exactly this bug; reworded in `8065bbd1`. |
| `3b0ba378` | Approval decision fires once; `secret_store_has_secret()` | The callback fix is correct (clear it before invoking, then close). `has_secret` is right on both backends (§4). Its declaration split `get_secret()` from its doc comment; fixed. |

## 2. Other allocator mismatches (question 1)

There are two allocators: `secure-memory.c` (`gn_secure_*`) and
`secure-mem.c` (`gnostr_secure_*`). Each puts a header in front of the user
pointer and frees `header` with `sodium_free` or `free`. Three kinds of
mismatch therefore abort or corrupt the heap:

- `g_free` of either allocator's string;
- either allocator's free of a `g_malloc` pointer;
- one allocator's free of the other's pointer.

I paired every `*_secure_{alloc,strdup,strfree,free}`, `secure_free_string`,
`gn_secure_clear_string` and every `secret_store_*` output with its consumer.
Fixed in `8065bbd1`:

| Site | Bug | Reachable |
|---|---|---|
| `sheet-backup.c`, `sheet-account-backup.c`, `sheet-social-recovery.c` | `cached_nsec` from `secret_store_get_secret()` (`gn_secure_strdup`) freed by a local `secure_free_string()` that ends in `g_free()` | Yes: any backup, export or social-recovery sheet that showed the key aborts when it closes (on macOS; see qgsn for Linux CMake) |
| `onboarding-assistant.c` passphrase-step validation | `gn_secure_entry_get_text()` results `g_free()`d on all three exits | Yes: every validation of the create-profile passphrase step |
| `multisig_coordinator.c:329` | `secret_store_sign_event()` returns `g_strdup`; freed with `gn_secure_strfree` | Yes: local co-signing |
| `multisig_wallet.c:767` | `partial_sigs` filled with `gn_secure_strdup`, array free func `g_free` | Yes: session free after any partial signature |
| `gn-secure-entry.c` `PROP_TEXT` | secure memory handed to `g_value_take_string()` (`g_free`) | Latent: nothing reads the property today |

Checked and consistent: `key_rotation.c`, `sheet-sign-message.c`,
`multisig_store.c`, `key_provider_secp256k1.c`, `sheet-multisig-signing.c`,
every `gn_secure_entry_get_text()` caller other than the one above, and the
backup-recovery and social-recovery outputs (`g_strdup` and nip19 malloc,
freed with `g_free`). No signer file mixes `gnostr_secure_*` and
`gn_secure_*` on one object any more.

## 3. Callback-twice in other dialogs (question 3)

- `confirm-delete-dialog.c` had the approval dialog's exact bug. Delete or
  Cancel invoked the callback, then `adw_dialog_close()` ran
  `on_dialog_closed()`, which invoked it again with FALSE. It has no callers
  yet (latent). Fixed in `77ca1c86` with one `fire_callback_once()` helper.
- In `approval_dialog.c`, a second `do_finish()` (Approve plus Ctrl+A, or a
  double click before the close completes) no longer fired the callback, but
  it still created a second remembered client session. It also closed an
  already-closed dialog, which logs an Adwaita critical. A `finished` flag now
  makes `do_finish()` run once. `on_dialog_closed()` clears the callback
  before invoking it (`77ca1c86`).
- `sheet-create-account.c` `on_finish` and `::closed`: the closed handler only
  clears data, so it's fine. No other signer dialog pairs a decision callback
  with a closed/close-request handler.
- New, found by the test on Linux: both signer dialogs chained up with
  `ADW_DIALOG_CLASS(parent)->closed(dialog)` without a NULL check. libadwaita
  1.5.0 has no default (`AdwDialogClass.closed == NULL`); 1.9.0 does. So on
  Ubuntu 24.04 every approval-dialog close jumped to address 0
  (ASAN: SEGV at pc 0 from `adw_dialog_force_close`), on master too. Fixed in
  `287900d8`, matching the guard `gh-new-message-dialog.c` already has.

## 4. `secret_password_searchv_sync` (question 4)

Correct:

- **Schema.** `IDENTITY_SCHEMA` and the daemon's writer schema
  `gnostr_secret_schema` (gnome/seahorse) are both
  `"org.gnostr.Signer/identity"` with `SECRET_SCHEMA_NONE`. libsecret
  therefore matches `xdg:schema`, and items the daemon wrote are found.
- **Attributes.** `npub` and `key_id` exist in both schemas, so
  `searchv`'s attribute validation passes. A table without destroy funcs is
  right, since the keys and values are borrowed.
- **Flags.** `SECRET_SEARCH_NONE`: no `LOAD_SECRETS`, so no secret enters the
  process. No `UNLOCK` either: libsecret still returns locked items, so a
  locked keyring reports TRUE without an unlock prompt. That is better than
  the old `lookup_sync`. Returns are `SecretRetrievable` objects, so
  `g_list_free_full(..., g_object_unref)` is right.
- **Version.** It needs libsecret ≥ 0.19; Noble and Homebrew have 0.21.
- **Build coverage.** The libsecret branch is compiled only by the meson build
  (`meson.build:252`). The CMake GUI deliberately leaves
  `GNOSTR_HAVE_LIBSECRET` undefined (nostrc-bml6). I compiled
  `secret_store.c` with `-DGNOSTR_HAVE_LIBSECRET=1` against libsecret 0.21.7:
  clean, and the only warnings predate this branch.
- **Keychain.** The attributes-only query (`kSecReturnAttributes`, no
  `kSecReturnData`) doesn't prompt. Its service and account pair
  (`"Gnostr Identity Key"`, npub) matches the writer in `signer_ops.c` and
  `get_secret()`.

Not a blocker, filed as nostrc-qgsn: on CMake Linux builds neither backend
macro is defined, so `has_secret` is always FALSE and the backup sheets can't
read the nsec. This predates the branch.

## 5. Tests (question 5)

`apps/gnostr-signer/tests/test-approval-dialog.c` (`signer/approval-dialog`,
`f5d4eaaf` + `86290094`):

- **Setup.** It builds the real `approval_dialog.c` and its template
  (single-file gresource) against stubbed accounts store, client sessions and
  keyboard navigation.
- **Presentation.** It presents the dialog in an 800×600 AdwWindow and waits
  until it is mapped and has drawn two frames. libadwaita 1.5 drops a close
  requested before the first frames.
- **What it checks.**
  - Approve, Deny and a bare close each report exactly one decision through
    libadwaita's real close path.
  - A second Approve is a no-op: one decision, one remembered session.
  - `set_accounts()` leaves `accounts_store_list()` entries to the array:
    3 entries, 3 frees per call.

| Run | Result |
|---|---|
| macOS, libadwaita 1.9 | 5/5 pass; 5/5 under ASAN |
| Against master `267779a8`'s dialog | approve test fails (`calls == 2`); list test aborts (SIGABRT, macOS malloc double free); under ASAN, heap-use-after-free in the array free func from `gnostr_approval_dialog_set_accounts:639` |
| Against `3b0ba378`'s dialog | second-approve test fails (Adwaita critical: close of a non-presented dialog) |
| linux-ci image (Noble, GCC, `-DGNOSTR_ENABLE_ASAN=ON -DGNOSTR_ENABLE_UBSAN=ON`, gate strict CFLAGS, Xvfb, libadwaita 1.5) | 5/5 pass, once `287900d8` is in |

It runs under ctest with `signer/ui`'s environment and skip policy: skipped on
Apple (`GNOSTR_SKIP_UI_TESTS`), and needs DISPLAY elsewhere.

Allocator pairing is not unit-tested. The fixed sites need the Keychain or
the daemon, GTK sheets, or multisig sessions. nostrc-sq3j proposes removing
the class instead.

## 6. Builds and gates

- macOS: `cmake -S . -B /tmp/rv-signer -G Ninja -DBUILD_GROUNDHOG=ON`;
  `ninja gnostr-signer gnostr-signer-daemon signer-tests` builds clean, with
  no new warnings in touched files. `ctest -R '^signer/'`: 13/13.
- Linux (linux-ci image, ASAN+UBSAN, GCC strict flags): `gnostr-signer`,
  `gnostr-signer-daemon` and `signer-tests` build with no warnings in touched
  files. `signer/`: 11/13. The 2 failures predate the branch: their sources
  (`secure-mem.c`, `social-recovery.c`, `backup-recovery.c` and their tests)
  are untouched. UBSAN: misaligned `guint64` canary store at
  `secure-mem.c:301` (nostrc-w8lf).
- `scripts/linux-gate.sh --sanitizers` on `f5d4eaaf`: passed, 53 tests. It
  runs Groundhog's sanitizer job only, so it does not exercise the signer.
  No signer test runs in any sanitizer gate; noted in nostrc-w8lf.
- Not run: the nip55l and Testing/ suites in the `signer` ctest label. The
  branch doesn't touch them, and `nip55l_keychain_migration` writes the real
  Keychain.

## 7. Nits (not fixed)

- `secret_store_has_secret()` swallows libsecret errors (D-Bus down → FALSE).
  That's acceptable for a display flag; a `g_debug` would help diagnosis.
- `approval_dialog.c` still never answers the caller if the dialog is
  disposed without `::closed`, e.g. the parent window is destroyed mid-prompt.
  The caller's context then leaks, and a D-Bus invocation would go
  unanswered. The test teardown shows the path exists; it was not observed in
  the smoke.

## Follow-ups filed

- **nostrc-w8lf** (P2 bug): `secure-mem.c` misaligned canary (UBSAN) fails
  `signer/secure-mem` and `signer/social-recovery` on Linux. No signer test
  runs in a sanitizer gate.
- **nostrc-sq3j** (P2 task): retire one of the two secure allocators, or
  return a typed secure string, so this bug class cannot recur.
- **nostrc-qgsn** (P3 bug): CMake Linux GUI has no `secret_store` backend:
  `has_secret` is always FALSE and the backup sheets can't read the nsec.
