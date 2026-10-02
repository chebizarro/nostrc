# Review 2: gnostr-signer allocator and dialog fixes (nostrc-n98j)

- Branch: `fix/signer-keychain-secure-strdup` at `86290094`
- Reviewed: `git diff 3b0ba378 86290094` (`8065bbd1`, `77ca1c86`, `f5d4eaaf`,
  `287900d8`, `86290094`), i.e. the commits the first reviewer added
- Reviewer: independent second review, 2026-10-01
- Environments: macOS 27 (libadwaita 1.9.0, GTK 4.22.3, sodium), and
  `nostrc-linux-ci:arm64` (libadwaita 1.5.0, GTK 4.14.5, GCC ASAN+UBSAN, Xvfb)

## Verdict: CHANGES-REQUIRED (small)

All five commits are correct. They add no leaks and no double frees. The
`::closed` guard is right on libadwaita 1.5 and 1.9. The new test fails
without each dialog fix it claims to pin.

The allocator sweep is not complete, though. A live crash in the same
class remains in a file that `8065bbd1` edited. The first review listed
"every `gn_secure_entry_get_text()` caller" and `sheet-multisig-signing.c`
as consistent, but each has an exception (§1). Fix §1.1 before merge (a
3-line change). §1.2 and §1.3 are a one-line change and a deletion; fix
them in the same commit.

## 1. Allocator pairs still inconsistent

I grepped every producer in `apps/gnostr-signer`: `gn_secure_{alloc,strdup}`,
`gnostr_secure_*`, `bin_to_hex_secure`, `decrypt_from_storage`,
`gn_secure_entry_get_text`, `secret_store_get_secret`,
`multisig_signing_get_final_signature`, `multisig_store_get_partial`,
`gn_sss_combine`. I also grepped every free: `gn_secure_{free,strfree}`,
`gnostr_secure_{free,strfree}`, `gn_secure_entry_free_text`,
`gn_secure_clear_string`, `secure_free_string`, `g_free`/`g_autofree` on the
same names. I followed each object through its struct fields and ctx
handoffs. Three mismatches remain. All three predate `3b0ba378`.

1. **Live: `onboarding-assistant.c:660-663` `import_profile_ctx_free()`**
   does `memset` + `g_free(ctx->passphrase)`. That passphrase is
   `gn_secure_entry_get_text()` memory (`:746`), whose ownership moves into
   the ctx at `:803`. Every D-Bus reply path frees the ctx (`:672`, `:695`,
   `:723`). So an onboarding import with a non-empty passphrase aborts when
   the reply arrives. That is always the case for a NIP-49 `ncryptsec`
   (`:761` requires a passphrase), and for a mnemonic when a passphrase is
   given. The import path is live: the entry is created at `:1376` and
   `perform_profile_import()` is called at `:1146`. A probe that runs the
   same sequence on a `gn_secure_strdup()` string, built with
   `GNOSTR_HAVE_SODIUM` as the app is, dies with SIGABRT (exit 134).
   Fix: `gn_secure_entry_free_text(ctx->passphrase)`, and drop the `memset`.
   `gn_secure_free` zeroes the allocation itself, and a zeroed string makes
   the `strlen()`-based size check log a size-mismatch critical.
2. **Dead: `onboarding-assistant.c:396-405` `create_profile_ctx_free()`**
   has the same `memset` + `g_free` on a passphrase, but `CreateProfileCtx`
   is never constructed. Delete it, or fix it like 1.1, so nobody copies
   the pattern.
3. **Latent: `sheet-multisig-signing.c:454`** calls
   `g_free(self->final_signature)`, but the field only ever holds
   `gn_secure_strdup()` memory (`:455`; dispose frees it correctly at `:503`).
   A second successful completion on the same sheet would crash. The sheet
   has no callers today. Fix: `g_clear_pointer(&self->final_signature,
   gn_secure_strfree)`.

Everything else pairs correctly, in both directions:

- every `gn_secure_*` and `gnostr_secure_*` free receives its own
  allocator's pointer, with the size it was allocated with;
- no `g_free` receives secure memory, apart from the three above;
- no string is zeroed before a `strlen()`-sized secure free.

Files covered: `key_rotation`, `secret_store`, `multisig_{wallet,store,coordinator}`,
`key_provider_secp256k1`, `social-recovery` (all `gnostr_secure_*`), the
backup, account-backup, social-recovery and sign-message sheets, the
change-password, create-profile, create-account and import-profile sheets,
and `gn-secure-entry`. The `ncryptsec`, `verified_nsec` and `recovered_nsec`
fields that stay on the `g_free` helper hold `g_strdup` or nip19 `malloc`
memory, so `g_free` is right for them.

## 2. The five commits

| Commit | Assessment |
|---|---|
| `8065bbd1` allocator fixes | Each site is correct. In the three sheets, `cached_nsec` is assigned only from `get_secret()`, which returns `gn_secure_strdup` on all three backends. The onboarding validation now borrows the legacy entries' text and frees the secure text through `gn_secure_entry_free_text`; the logic is the same as before and nothing leaks on any exit. In the coordinator, `secret_store_sign_event()` returns `g_strdup` (`secret_store.c:399`), so `g_free` is right. In the wallet, `:897` is the only insert into `partial_sigs`, and nothing steals from the array. The secure entry's `text` property now hands the GValue a GLib copy. That copy is not zeroed when freed, which a GValue cannot avoid; nothing reads the property today. The header comments are accurate, and all 8 `accounts_store_list()` callers leave entries to the array. |
| `77ca1c86` dialogs decide once | Correct. `finished` is never reset, which is fine: every `gnostr_show_approval_*` helper makes a new dialog for each request. Nit: `confirm-delete-dialog.c` got `fire_callback_once()` but no `finished` guard. A second Delete click before the close completes still calls `adw_dialog_close()` again (an Adwaita critical), and at CRITICAL severity it re-authenticates. The dialog has no callers. |
| `287900d8` guard the `::closed` chain-up | Correct for both versions. A probe of `ADW_DIALOG_CLASS(g_type_class_ref(ADW_TYPE_DIALOG))->closed` gives NULL on 1.5.0 and non-NULL on 1.9.0, so the guard skips the chain-up on 1.5 and keeps it on 1.9. No other unguarded libadwaita vfunc chain-ups remain in the repo (groundhog's is already guarded). |
| `f5d4eaaf`, `86290094` test | The test is real (§3). Its stubs are type-checked against the real headers. The fixture removes its timeouts and tick callbacks, and teardown frees everything it owns. |

## 3. Does the test fail without the fixes?

I ran each case in its own process, with `approval_dialog.c` swapped as listed:

| `approval_dialog.c` | approve | deny | close | second-approve | set-accounts |
|---|---|---|---|---|---|
| tip, macOS 1.9 | ok | ok | ok | ok | ok |
| tip, Linux 1.5 (ASAN+UBSAN, Xvfb) | ok | ok | ok | ok | ok |
| `267779a8` (pre-n98j master), macOS | **2 calls** | **2 calls** | ok | **abort** | **abort: "pointer being freed was not allocated"** |
| `3b0ba378`, macOS | ok | ok | ok | **fatal critical: close of a dialog that isn't presented** | ok |
| tip without the `finished` guard, macOS | ok | ok | ok | **same critical** | ok |
| tip, `do_finish` leaves the callback set, macOS | **2 calls** | **2 calls** | ok | **2 calls** | **2 calls** |
| tip, unguarded chain-up, Linux 1.5 | **SEGV pc 0** | **SEGV pc 0** | **SEGV pc 0** | **SEGV pc 0** | **SEGV pc 0** |

Coverage gaps (not blockers):

- No test covers the `8065bbd1` allocator fixes or `confirm-delete-dialog`.
- On macOS, ctest sets `GNOSTR_SKIP_UI_TESTS=1`, so `signer/approval-dialog`
  "passes" there in 0.02 s without running. It does run in the Linux
  pre-push smoke stage, which wraps ctest in `xvfb-run`, and it runs when
  started by hand on macOS.

## 4. Build and tests

I built `/tmp/rv-signer2`, a fresh configure of a worktree at `86290094`,
with `source /tmp/nostrc-macos27-env.sh; ninja gnostr-signer signer-tests`.
It is clean, with no warnings in the touched files.

`ctest` in `apps/gnostr-signer` passes all 17 tests that were built. The
6 `nostr_signer_webext_host_*` tests show Not Run because those binaries
are outside the targets I built. `test-approval-dialog` run directly on
macOS: 5/5 ok.

## Addendum: `9acd502c` verified. Final verdict: APPROVE

`9acd502c` fixes all three §1 findings correctly:

- `import_profile_ctx_free()` and `create_profile_ctx_free()` free the
  passphrase with `gn_secure_entry_free_text()`, which is NULL-safe, with no
  `memset` before it.
- `sheet-multisig-signing.c:454` frees with `gn_secure_strfree()`.

A re-grep finds no `g_free` on a secure field anywhere in the signer. The
fixed files build clean on macOS, and the same 17 gnostr-signer ctest tests
pass. `test-approval-dialog` run directly passes 5/5.

I reran the import probe on the real `ImportProfileCtx` and
`import_profile_ctx_free()`, extracted verbatim from the source, with the
passphrase taken from a real `GnSecureEntry` via `gn_secure_entry_get_text()`:

| Version | Result |
|---|---|
| `9acd502c` (fixed) | exits 0; 3 passphrase frees and 1 NULL free; no secure-memory critical; nothing outstanding beyond the live entry's own buffer |
| `86290094` (control) | SIGABRT (exit 134) |

I accept leaving out the confirm-delete `finished` flag, with one
correction to the reasoning. The callback can't fire twice, but a second
Delete click is not entirely free of side effects: at CRITICAL severity it
re-runs `gn_session_manager_authenticate()`, and it calls
`adw_dialog_close()` again, which logs an Adwaita critical. Neither is a
second decision, and the dialog has no callers, so this stays a nit.

Hygiene note: `9acd502c` also carries 59 lines of unrelated
`.beads/issues.jsonl` churn. Consider splitting it out before merge.
