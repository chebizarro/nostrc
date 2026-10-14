# G09 identity onboarding: security and custody review

## Context / Scope

**Verdict: REQUEST CHANGES.** The central daemon-custody and asynchronous-lifecycle changes are substantially sound, but a reachable unconfirmed deletion route and several backup/cleanup/state-truth gaps remain.

Reviewed `git diff 0d4dd1faf...origin/grotto/g09-identity-onboarding`, one commit **`f1a58b4b058e63bfb4415cf35e63ed276e0e46b5`**. Verified `git merge-base master origin/grotto/g09-identity-onboarding` is `0d4dd1fafa512379f36917d2d25e41b9d1c2485f`, and the specified worktree HEAD equals the remote-tracking branch. References below are branch-relative paths and line numbers at that commit.

Contract: `docs/plans/grotto-signer-ux-2026-10-12.md` §5.7 in full, §4.4, §2 OC-3/OC-7, and §9 `signer/onboarding` / `signer/home-model`. Only the seven requested review areas are covered. No implementation, tracker, version-source, hook, commit, push, or developer-keychain changes were made. This report is the sole tracked-tree addition.

**Evidence labels:** Confirmed = directly established by source or the stated local check; it does not imply live Linux acceptance. Plausible = conditional runtime consequence not reproduced. Retained gaps are identified explicitly rather than attributed to this commit as newly introduced regressions.

## Findings (severity order)

### F1 — P1 / High — Confirmed: the account picker still removes identities without confirmation

- Reachable path: `apps/grotto/src/ui/page-settings.c:181-184,847` opens `SheetSelectAccount`; its trash button is wired at `apps/grotto/src/ui/sheets/sheet-select-account.c:169-188`.
- `sheet-select-account.c:55-71` immediately removes metadata, calls `secret_store_remove(data->npub)`, and removes the row. There is no confirmation, backup check, or result check. This is **a retained reachable gap in a file changed by G09**, not a defect in the new detail-page confirmation.
- For a Grotto-created identity, the daemon clear path now also deletes its mnemonic credential (`nips/nip55l/src/core/signer_ops.c:1764-1778`). Thus this alternate route can destroy both signing and recovery custody with one accidental click, including the old identity after rotation. It contradicts §5.7's “old identity remains until removal is confirmed.” On failure, the picker also hides the row as though deletion succeeded.
- **Recommendation:** route picker removal through the same identity-pinned confirmation and authoritative completion handling as `page-identity-detail.c:188-212`. A cancel must make no deletion call; failed deletion must retain an honest visible state.

### F2 — P2 / Medium — Confirmed: failed seed export can still be acknowledged as a completed backup

- After successful self-test, `apps/grotto/src/ui/onboarding-assistant.c:188-190` enters Backup and requests the phrase. Seed-export failure at `:151-156` only replaces the label/status; it does not gate acknowledgement or change the backup state.
- Next is enabled by the checkbox alone (`:59-60`); `:273-279` then persists `setup=ready` and enters Ready without checking whether seed export succeeded or an alternative export was made.
- For **newly created** identities, the generated phrase was never shown before storage and was already wiped from the assistant (`:239-242`). A locked/unavailable recovery backend can therefore leave the user with no backup ever presented, while the normal success flow still permits a “safely backed up” acknowledgement. Imported identities legitimately may have an existing external backup; that case should not be confused with this newly created case.
- **Recommendation:** keep generated-identity backup pending on export failure, offer Retry, and require successful presentation or an explicit alternative-backup flow before the normal acknowledgement. Keep Skip available as dismissal, not successful backup.

### F3 — P2 / Medium — Confirmed cleanup omissions; plausible post-free recovery of secret bytes

The assistant's explicit buffers are wiped, but its real derivation path still creates ordinary heap copies that are freed without wiping:

- `apps/grotto/src/backup-recovery.c:235-237` makes an auto-freed normalized mnemonic; the create path calls this helper at `:347`.
- `nostr-gobject/src/nostr_bip39.c:185-195` duplicates the generated phrase into object state, then ordinary-frees the original generated allocation. The object's own cleanup does not erase this separate allocation.
- `libnostr/src/crypto/bip39.c:224-225,256-257` frees the normalized mnemonic and optional mnemonic passphrase through plain `nfkd_free` (`:145,155`). Only the salt is cleansed (`:253-255`). The new daemon create/import/rotation/export paths all use this primitive.
- The newly used management-key path also reaches the Keychain branch's plain `free(sk_hex)` after storage (`nips/nip55l/src/core/signer_ops.c:1752-1755`); wiping `skb`/`secretData` does not wipe this independent string.

These are retained primitive/backend gaps reached by the new flow. They do not establish a log leak or cross-process key disclosure; recovery from allocator residue is **plausible, not demonstrated**. They do defeat a strong claim that all owned transient recovery copies are wiped at hand-off/cancel. Immutable D-Bus message buffers are a separately documented transport limitation (`docs/dbus-interface.md:124-125`), not treated here as an approver bypass.

**Recommendation:** wipe each owned generation/normalization/backend allocation using the existing non-optimizable clear primitive before free, including error paths. Track allocation length before destructive trimming when clearing import copies (`onboarding-assistant.c:247,260`). Preserve the existing derivation output and formats; no new custody abstraction is needed.

### F4 — P2 / Medium — Confirmed: “enter management with warning” drops the warning at the management boundary

- `apps/grotto/src/ui/onboarding-assistant.c:143-150,281` correctly refuses Ready after self-test failure, but finishes with `FALSE` and closes the only visible warning.
- Production first-run handling `apps/grotto/src/main_app.c:227-254` only logs the boolean and presents an ordinary window; it creates no warning or retry affordance. The rerun handler at `:590-593` likewise only logs it. Assistants opened by `onboarding_assistant_present` / `verify_identity` (`onboarding-assistant.c:360-372`) have no completion callback at all.
- Pending setup metadata is recorded, but the account picker never consumes `setup_state` (`sheet-select-account.c:216-223`), and the detail page reads only the label (`page-identity-detail.c:53-55`). Its generic Retry button does not tell the user that this identity has not passed verification.
- **Recommendation:** surface the selected identity's persisted verification/backup-pending state in management with a clear warning and exact-identity Retry. Do not manufacture Ready or block legitimate maintenance. This is a contract gap, not evidence that the daemon grants permission after a failed test.

### F5 — P2 / Medium — Confirmed: the required coverage is materially narrower than the suite names suggest

- `apps/grotto/tests/test-home-model.c:147-153` registers seven **identity metadata** tests. It exercises neither `GetStatus`, `RequestsChanged`, `ActivityChanged`, signed-today/client/pending tiles, client rows, nor pending-group appearance/disappearance. Those are precisely the §9 `signer/home-model` obligations. These tests are useful, but do not satisfy that row; dashboard coverage may remain a later step, and must be reported as such.
- `test-onboarding.c:248-257` covers real widget/state logic with mocked external bus I/O, delayed creation, cancellation, seed-label cleanup, error outcomes and clipboard ownership. It does not cover successful mnemonic/NIP-49 imports with their distinct passphrases, delayed import close/failure, seed-export failure followed by acknowledgement, warning hand-off into real management, the picker deletion route, or encrypted-file cancellation/write failure. The seed cleanup test at `:150-165` supplies only a successful export.
- `test-identity-management.c:17-72` gives useful real gate/no-grant tests. The full storage/export/rotation/two-consumer case at `:131-207` requires a disposable Secret Service and **skipped locally**. There is no demonstrated real locked-keyring acceptance here. UI injected `BackendUnavailable` is not proof of a genuinely locked backend, and tampered self-test results are not injected into the real crypto path.
- **Recommendation:** add focused counterexamples for F1–F4 and declare the remaining §9 dashboard obligations separately. Require Linux execution of the real assistant and disposable-keyring round-trip, including locked/unavailable, deny/expire, no-grant and two-consumer isolation outcomes, before treating G09's custody acceptance as complete.

### F6 — P3 / Low — Confirmed: reload clears the INI active value, not the GSettings legacy default

- Atomic replacement at `apps/grotto/src/accounts_store.c:186-199` clears missing `active`/`selected` values in memory, and deleting the INI produces an empty metadata snapshot (`:211-219`). It does not reconcile `default-identity` in GSettings.
- Default cleanup occurs only in explicit `accounts_store_remove` (`:442-443`) or successful D-Bus `ClearKey` (`nips/nip55l/src/glib/signer_service_g.c:498-510`), not monitored reload or an authoritative inventory losing a key (`accounts_store.c:904-918`). `test-home-model.c:48-62` checks only `get_active`, not the GSettings default after reload.
- A stale default after external key removal is a **plausible runtime consequence**; this review does not claim silent redirection. Removing metadata alone also does not prove the signing key was removed, so blindly clearing a still-signable default would be wrong.
- **Recommendation:** explicitly reconcile default validity against the authoritative signable inventory, preserving still-stored defaults; test removed-key and metadata-only replacement separately. Continue avoiding GSettings mutation after failed INI saves.

### F7 — P3 / Low — Confirmed contract exception needs explicit justification: daemon CreateProfile remains

- The new assistant no longer calls `CreateProfile`, and no production caller of `sheet_create_profile_new` was found. The remaining sheet makes the legacy call (`apps/grotto/src/ui/sheets/sheet-create-profile.c:326-340`) but is not a reachable production create route.
- The daemon still implements key generation/storage with ignored creation passphrase/hardware arguments (`nips/nip55l/src/glib/signer_service_g.c:554-589`); G09 even adds `IdentitiesChanged` there (`:624`). The public XML still exposes those reserved arguments (`nips/nip55l/dbus/org.nostr.Signer.xml:307-323`).
- §5.7 requires deletion unless a live caller justifies retention. The examined source, changed interface documentation, and commit message supply no such live-caller evidence. Existing public API compatibility is a reasonable possible justification, but is not the specified evidence and should be recorded rather than assumed.
- **Recommendation:** document the explicit compatibility exception and its no-mnemonic behavior, or remove the path with the required public-surface version assessment. Do not reintroduce a creation password or advertise the reserved hardware option. No unused passphrase remains in the **reachable new creation UI**.

## Confirmed properties and bounded limitations

### Secret handling / diagnostics

- Generated assistant mnemonic/nsec are wiped immediately after creating the store tuple (`onboarding-assistant.c:239-242`); import input/passphrase are cleared after submission and on close (`:50-54,255-260,310-320`). Explicit assistant-owned mnemonic storage is wiped when leaving Backup and all seed-label text is removed (`:72-75`); detail-page labels/passphrases clear on selection and unmap (`page-identity-detail.c:48-51,220-225`). These logical cleanup guarantees do not fix F3's deeper allocation copies.
- Clipboard clearing compares the exact content-provider identity, not content equality (`apps/grotto/src/secret-clipboard.c:10-18`), and wipes its owned bytes on provider release (`:6-8`). An unrelated later clipboard is not erased; the ownership test exists (`test-onboarding.c:221-232`), but the UI suite skipped locally. Clipboard managers remain explicitly warned about.
- The libnostr rejected-token `fprintf` is actually removed. The remaining BIP-39 diagnostic prints only checksum failure and word count (`libnostr/src/crypto/bip39.c:215`), not seed tokens. The subprocess regression checks stdout/stderr for rejected private-hex input (`test-identity-management.c:209-218`) and passed locally. Examined NIP-06/NIP-49/key-generation diagnostics contain no secret interpolation. Control1 errors are static and do not forward backend messages (`grotto_control.c:166-167,261-265`); new assistant/detail statuses likewise do not print raw errors. Existing generic legacy error logging (`secret_store.c:220-228`) is not a demonstrated secret leak on this new Control1 path.
- Exports bypass activity-history recording; self-test history whitelists only metadata (`grotto_control.c:126-153`; `event_history.c:850-852,1008-1014`). Review previews contain operation descriptions, not key material. No secret-in-log/notification/history path was confirmed for the new flows.
- Daemon-owned export results/passphrases are wiped (`grotto_control.c:195-200,238,277-278`). File jobs wipe their owned bytes after completion (`page-identity-detail.c:64-75`). Raw export is handed to the wiping clipboard provider; seed labels are bounded by leaving the page. The reused encrypted QR sheet ordinary-frees its `ncryptsec` copy (`sheet-qr-display.c:87-93`) and copies it with ordinary clipboard handling (`:55-62`): this is ciphertext, not a raw-nsec route, but it does not provide the same cleanup promise.
- Destination choice precedes export (`page-identity-detail.c:153-172`); the only secret-derived file output in this page is **encrypted ncryptsec**, using private replace-contents with cancellation, no backup file (`:71-75`). Migration JSON is public signed data. No partial **plaintext key** file path was found. Interrupted writes, especially nonlocal GFile backends, were not dynamically tested.

### Custody / storage truth / self-test / rotation

- Mnemonic custody has a distinct schema/service keyed by canonical npub (`signer_ops.c:805-809,847-854`), separate from enumeration's identity schema/service (`:1861-1862,1894-1899`). Creation checks mnemonic-to-key binding (`:891-911`); import does not create a mnemonic item (`:919-952`). Existing Grotto-created credentials remain associated with that identity if the same key is later re-imported; genuinely import-only identities return `NotFound`, and both backup UIs display “no seed words” rather than inventing them.
- Control1 export/raw-export/mnemonic-export dispatch is freshly approver-gated (`grotto_control.c:380-388,522-539`), with re-attestation before delivery (`:255-260`). `mnemonic-export` reflects compiled backend support (`:91-93`; `signer_ops.c:811-816`), **not per-identity seed availability or current keyring unlock state**. This is honest with the documented capability meaning. PID-less macOS approver isolation is an inherited, explicitly documented limitation (`docs/dbus-interface.md:135-139`), not newly solved here.
- `identity_stored` is set only from the store/import reply; borrowed reply identity is copied before unref (`onboarding-assistant.c:169-199`). Operation contexts retain assistant/cancellable/generation (`:92-107,221-231`); stale results cannot continue setup. Back/Skip and their handlers refuse busy commits (`:55-60,285-294`); closing cancels local continuation but never calls ClearKey (`:310-320`). Completed daemon stores emit identity changes even when the UI has gone (`grotto_control.c:244-245`).
- Self-test is one MANAGEMENT request (`grotto_control.c:503-510`) with three named operations in review (`approval_dialog.c:600-604`). The real gate forces ASK and strips remember for both allow/deny (`signer_approval_service.c:779,1148-1152`); management never admits a client (`grotto_control.c:143`). Local no-grant tests passed.
- The event is kind 20000 and never published (`signer_ops.c:969-974`). Verification checks canonical ID **as well as** signature (`libnostr/src/event.c:635-658,698-699`) and exact account pubkey. Random challenge encryption, independent ephemeral-peer decryption/re-encryption, same-identity decryption and comparison are real (`signer_ops.c:975-994`); challenge/peer secret are wiped (`:995-1001`). No relay call exists in this path.
- Waiting/testing/passed/denied/expired/verification-failure states are represented; backend failures fail closed and Ready is blocked. Locked and unavailable share a broad outcome, with no separate locked-specific presentation (`onboarding-assistant.c:145-150`; `onboarding-assistant.h:12-14`). Actual locked-backend behavior is unverified; F4 concerns warning continuity, not unsafe consent or grants.
- New rotation stores a successor without selecting it, saves the old-key-signed kind-1776 event, and neither redirects clients, copies grants, removes the old key, nor publishes (`grotto_control.c:208-236,267-275`; `key-migration-event.c:14-27`). Publication is left to an explicitly chosen consuming application; there is no automatic or built-in publisher in the new detail page. Its own removal is confirmed and identity-pinned (`page-identity-detail.c:188-212`); F1 is the alternate route.
- Accounts reload swaps temporary maps only on successful parse (`accounts_store.c:186-199`). GFileMonitor observes the directory before initial load (`:143-150`). INI save is consistent/durable and GSettings mirroring is conditioned on success (`:343-375`); changed mutators roll back and notify only after saving (`:401-404,518-527,589-596`). Entry arrays own their elements (`:483-499`; picker `:216-226`). Management selection is in-memory (`:544-550`), and “Default for older apps” calls Control1 rather than management selection (`page-identity-detail.c:183`; `grotto_control.c:464-471`). F6 bounds the removed-default claim.

### OC-3 reachability / platform floor

- Reachable Settings social recovery is hidden and its callback disconnected (`page-settings.c:849-851`); old backup/rotation entry points now open the detail page (`:190-194,853`; `signer-window.c:651-654`). The new assistant/detail templates expose none of hardware/TPM/Secure Enclave/PKCS#11/multisig/delegation/social recovery.
- Searches traced residual hardware construction to legacy `gnostr_settings_page_new` (`settings_page.c:232,300-310`), with no production caller; residual legacy account/create/rotation sheets likewise have no reachable production constructor chain. Definitions/resources being compiled is not proof of reachability. No forbidden-feature user-facing claim was found in README/metainfo/desktop assets; the desktop file's old multisig comment is not a visible claim. This is source evidence, not a real accessibility/click-through acceptance run.
- CMake requires GTK ≥4.14 / libadwaita ≥1.5 (`apps/grotto/CMakeLists.txt:19-22`), matched by Meson (`apps/grotto/meson.build:29-34`). Syntax-only compilation of the assistant, detail page and secret-clipboard passed with `ADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5` and `GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14`, deprecation diagnostics as errors. This checks API usage with current headers, not an actual libadwaita 1.5 runtime.

## Verification / Recommendations

Local checks used the pre-existing isolated `_build` (configured with `NIP55L_SECRET_BACKEND=libsecret` and `GROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`), without rebuilding or altering generated source:

- `git diff --check 0d4dd1faf...HEAD`: passed.
- `ctest --test-dir _build -R '^signer/(onboarding|home-model|identity-management|permissions)$' --output-on-failure`: **5 CTest entries passed, 1 skipped**, including two fixture setup entries. Actual suites: permissions, home-model and identity-management passed; onboarding skipped (`GNOSTR_SKIP_UI_TESTS` on macOS).
- Verbose identity-management: five cases passed; `/identity/live-custody` internally skipped because `gnome-keyring-daemon` was unavailable. The CTest-level pass is not full custody/rotation acceptance.
- Three targeted API-floor syntax checks passed as described above. No full build, full CTest, real GTK onboarding run, live Linux custody run, sanitizer run, or 1.5 runtime acceptance is claimed.

Prioritize F1 before merge, then close F2–F4 with direct failure/cancel tests. Resolve or explicitly bound F5–F7 rather than declaring §5.7/§9 complete from green aggregate test counts. Report-only version assessment: **no bump**; this review does not alter a shipped artifact.

Tooling note: Jev relevance probes narrowed large-file/search reads; source was inspected for all load-bearing claims. One low-confidence relevance result for AccountsStore missed the later async section; a direct function lookup located it. No advisory model result was treated as proof. Usage across these probes and the doctor check: 14 requests, 73,021 input tokens, approximately $0.00307.

**REQUEST CHANGES**
