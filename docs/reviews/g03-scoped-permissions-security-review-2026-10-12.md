# G03 scoped permissions: security critique

Branch: `origin/grotto/g03-scoped-permissions` (commit `09749959f`), compared with `master` using `git diff master...`.
Spec: `docs/plans/grotto-signer-ux-2026-10-12.md` §5.4, §5.10 "Permissions", and the §9 rows `signer/permissions` and `signer/trust-tiers`.
Method: static read of the diff and the surrounding service code. **No tests were built or run.** "Confirmed" means the defect follows directly from the code on the branch. "Plausible" means it depends on a caller or deployment behaviour I could not see.
Paths are relative to `nips/nip55l/` unless stated. `perm.c` is `src/glib/signer_permissions.c`. `svc.c` is `src/glib/signer_approval_service.c`.

## 1. Summary

No path was found that lets an explicit allow, a tier, or a Needs-review record authorise an operation that the spec says must prompt. In the reviewed code the evaluation order is correct, and review rows are inert.

| Spec requirement | Result | Evidence |
|---|---|---|
| Explicit deny wins | OK | `perm.c:300` returns on the first live deny. |
| Required-confirmation kinds bypass allows | OK | The check at `perm.c:303` runs before `:306` and before every tier branch. |
| Needs-review is never executable | OK | Only `"allow"` is counted (`:301`). A `"review"` row matches nothing. |
| Absent kind is not kind 0 | OK | `-1` means absent. An `event` with kind `<0` returns ASK at `:290`. Kind 0 is a real kind and is in the required list. |
| No unchecked `guint16` cast | OK | The range check is `> G_MAXUINT16` (`:45`, `:289`, `:179`). The `(guint32)kind` cast at `:282` is guarded by `kind >= 0`. `svc.c:~893` bounds kind to 65535 before the `(gint)` cast. |
| Legacy event allows cannot be executed | OK | `evaluate`, `list`, `remember`, `clear` and `replace` all call `reload()` first. Event allows are stripped there and the legacy file is never read for authority. |

The defects below are mostly availability, over-broad cancellation, silent loss of existing grants, and weak points in the API contract that G04 will build on.

The most important findings:
- **F1:** the revision gate cancels requests the user has already approved.
- **F2:** legacy non-event allows stay executable on every load and can upgrade or extend existing v2 grants.
- **F3:** legacy wildcard-identity allows are silently dropped. This breaks the `nostr-seal` test fixture, which this branch does not update.

## 2. Findings, ranked

### F1. Medium, confirmed. The revision gate cancels user-approved work on any unrelated permission change

- `svc.c:654-655`: `execute()` captures `permission_revision` and then calls `request_policy()`. The call runs `reload()`, which can bump the revision through `changed()`. The job is therefore born stale whenever the on-disk state differs from memory.
- `svc.c:751`: `job_done` cancels when `j->stale_policy ||` is true, and this is not gated on `r->decision`. `svc.c:790` and `:808` set `stale_policy` when the global revision differs. `revision()` also moves on any unrelated mutation, on any `policy_changed`, and on an expiry elapsing (`perm.c:160-168`).
- Concrete scenario, using only code on the branch:
  1. The worker is busy.
  2. The user approves requests A and B with "Remember". A and B are admitted at revision R.
  3. A's `job_done` runs `remember_permission` (`svc.c:760`). That calls `commit` and then `changed`, so the revision becomes R+1.
  4. B is cancelled with "permissions changed before execution", although the user approved it and nothing was denied.
  5. Whether B runs before or after A's `job_done` depends on thread timing.
- Impact: this fails closed, so no authority is gained. It makes "Remember" unreliable and gives the user a confusing result. The spec (§5.2 step 7) says approval **rechecks effective permissions**. It does not say any permission change cancels approved work.
- Fix: gate the revision check on `!r->decision`. Approved work should be re-evaluated for DENY only. Alternatively, keep a per-scope generation. If you keep the single global counter, capture the revision *after* the evaluation.
- `test_deny_queued_execution` semantics are preserved. A deny still cancels queued work through `permissions_changed` (`svc.c:913`). The new tests cover tier-lowering and expiry, but not approved work or two consecutive "Remember" approvals.

### F2. Medium, confirmed. Legacy non-event allows stay executable after migration and can overwrite or extend existing v2 grants

- Spec §5.10 says the daemon "treats it [`signer-grants.ini`] as untrusted input" on every load. The branch only demotes *event* allows (`perm.c:222-223`). Every other valid legacy allow is re-imported on every load.
- Import is skipped only when the fingerprint of the legacy value is unchanged (`:235`).
- A new or changed legacy line therefore becomes live authority with no review and no scope or duration confirmation. Three cases:
  - A new line such as `nip44_conversation_key|P|I=allow` becomes a permanent allow. That is the export operation Full tier deliberately excludes.
  - An unexpired v2 allow such as `get_public_key|P|I` at `allow:+600` is overwritten when a legacy `get_public_key|P|I=allow` is written later. The overwrite is unconditional except for deny (`:237`), so the expiry becomes "forever". This contradicts "expiry is absolute (not extended)".
  - A lowercase `deny` in the legacy file overwrites a v2 allow. That direction is safe.
- This matters because §5.10 names an older GUI writing the file after migration as the threat, and the test covers only the event case (`tests/test_signer_permissions.c:122-130`).
- Fix: after the first import, stop treating changed legacy rows as grants. Import only denies, and demote every legacy allow after the initial migration to `review`. At minimum, never let a legacy write replace an existing v2 row for the same id. Compare `existing` for allow rows as well as deny rows.

### F3. Medium, confirmed. Legacy identity-wildcard non-event allows are silently dropped. The `nostr-seal` test fixture still depends on them

- `valid_row` rejects `allow` with identity `*` for every operation except `get_relays` (`perm.c:47`). `migrate` simply `continue`s (`:230`).
- The row gets no Needs-review marker and no log line, and it stays in the legacy file. It is retried and dropped on every load.
- Spec §5.10 says to "preserve valid non-event legacy permissions". The pre-0.7 format and docs (`nips/nip55l/README.md:105`, the D-Bus XML around `:279`) document `<principal>|*` as valid. Existing user grants such as `get_public_key|exe:…|*=allow` therefore stop working with no explanation.
- `gnome/nostr-seal/tests/test-nseal-signer.c:81-85` seeds exactly `[get_public_key] exe:%1$s|*=allow` and `claimed:|*=allow`. It is not in this diff. I did not run it, but it should now prompt and hang or fail.
- The branch updated the `test_nm_e2e.c` fixtures the same way (`apps/grotto/native-host/tests/test_nm_e2e.c`, `%2$s` replacements), so the author knew about this. The other consumer was missed.
- Fix: represent a wildcard non-event legacy allow as a `review` row, with `legacy-scope` set to `"identity-wildcard allow"`. Add a test for it. Update the `nostr-seal` fixture.

### F4. Low–Medium, confirmed. The untrusted legacy file can disable all evaluation, and one bad record disables the store

- `perm.c:250`: `load_optional(legacy)` returns FALSE on a parse error. `reload` then fails, and every `evaluate` returns `ERROR`.
- `svc.c:633-637` and the `policy_denies` helper treat ERROR as deny, and `permissions_changed` (`:913`) cancels every resolved request when it sees one. `resolved()` and `execute()` return `FAILED` with "cannot load permissions".
- A truncated or half-written legacy file from an older GUI, which the spec already treats as untrusted, therefore takes down all signing and all prompting, including explicit v2 allows and denies.
- Likewise, any single invalid `record-*` group makes `read_row` fail and takes down the whole store (`:256`, `:264`).
- Failing closed is correct here. But the *legacy* file is never authority, so a legacy parse error should not gate the v2 store. Treat an unparsable legacy file as "no legacy rows", quarantine it, and report a warning. Keep the strict failure for the v2 file.

### F5. Low–Medium, plausible. `connect` and `nip44_conversation_key` can be authorised by an explicit remembered allow or a first-party default. Only the tiers exclude them

- `perm.c:306` (`if (allow || first_party_default) return ALLOW`) is evaluated **before** `:309`, which excludes `connect`, `nip44_conversation_key` and a NULL principal. The comment at `:305` says "Export is never implied by a tier", which is true only of the tier branches below it.
- `valid_row` accepts allow rows for both operations. `decide(allow, remember=TRUE)` on a `connect` request therefore writes a forever allow.
- Consequences:
  - A client whose pairing admission is revoked in the G04 client store can still match a stale `connect` allow in the permission store.
  - A build-time `NIP55L_DEFAULT_GRANTS` entry naming `nip44_conversation_key` or `connect` skips the Full-tier exclusion.
  - The NULL-principal clause cannot be reached with `first_party_default` today, because `grants_lookup_default` requires an EXE or SYSTEMD_SCOPE caller. It is a contract weakness only.
- Spec §5.4 says "pairing admission is not a signing grant" and "unidentified callers cannot receive remembered permissions".
- Fix: either reject `connect` rows in `valid_row` and have G04 own admission, or move the `:309` exclusion above `:306` and add an explicit "export needs a prompt unless explicit" rule. Document which.

### F6. Low, confirmed. Unknown operation, absent kind, or out-of-range kind returns ASK before the deny scan

- `perm.c:289-290` returns `ASK` for an unknown op, for kind `> 65535`, and for an `event` with kind `-1`. A stored operation-wide `event` deny is therefore never consulted.
- Today this is unreachable. `svc.c` always supplies a validated kind for `sign_event` (`:893-895`), and the evaluator rejects out-of-range values elsewhere.
- Spec: "explicit deny wins". G04 and NIP-46 adapters will call `evaluate()` directly. A malformed request that gets a prompt instead of a deny is exactly the case an operation-wide deny exists for.
- Fix: for a known op, scan denies first. Operation-wide denies (`kind == -1`) should match any event, including a malformed one. Return `ASK` only if no deny matched.

### F7. Low, confirmed. Remembered allows for required-confirmation kinds are stored dormant

- `decide()` and `job_done` (`svc.c:760`) write an allow row even when the kind is in the required list. The row has no effect (`perm.c:303`).
- The list is mutable and can be empty (`:176-184`, `count == 0` is accepted). If a kind is later removed from Settings, the dormant allow becomes live. The user never confirmed that for the new setting.
- The dormant row also tells the user "Remember" worked when it did not.
- Fix: do not persist an allow for a kind that is currently required, or return a flag to the caller. Also consider refusing `count == 0` or logging it.

### F8. Low, plausible. The expected-revision is in-memory only (conflict semantics)

- `p->revision` starts at 1 (`perm.c:151`) and is not persisted. A UI that read revision N before a daemon restart can match a different state after N increments in the new process (an ABA conflict).
- `revision()` also increments when any expiry elapses (`:164-166`), so a stale-looking `replace` can be rejected for no content change. That fails safely.
- The comparison correctly happens *after* `reload()` (`:421-422`), so an external edit is caught.
- Fix: persist a monotonic epoch in the `[permissions]` group, or include a per-process random nonce in the revision.

### F9. Low, plausible. The tier gate relies on callers remembering to call `policy_changed`

- `signer_permissions_policy_changed()` is the only way an in-flight, tier-admitted job learns the tier changed. There is no check that the lookup result is stable.
- The branch tests it manually (`tests/test_signer_approval_service.c`, `test_tier_lowered_queued_revision`).
- No production code calls `set_trust_lookup` or `set_required_confirmation` yet (G04/G07 work). Everyone is Low in production today. The tier code is correct but unexercised by real traffic.
- Recommendation: when G04 lands, add a test that lowering a tier through the client store bumps the revision.

### F10. Low, confirmed. Store file and directory hygiene

- `save_file` runs `g_chmod(dir, 0700)` on the shared `$XDG_CONFIG_HOME/gnostr` directory (`perm.c:137`). That silently tightens other gnostr data.
- `g_chmod` follows symlinks. There is no ownership or mode check on an existing file or directory.
- The profile guard protects only test isolation (`path_for`, `:120-128`).
- File write is atomic and durable with mode 0600 (`:141-142`).
- Directory fsync is delegated to GLib (`G_FILE_SET_CONTENTS_DURABLE`).

## 3. Per-question answers

### (1) Evaluation order (`perm.c:285-316`)

- Order is: validate → scan (deny returns at once, allow noted) → required-confirmation → `allow || first_party_default` → NULL principal / export / connect → tiers.
- **Explicit deny wins:** yes, including over first-party defaults (tested at `tests/test_signer_permissions.c:228-229`).
- **Required kinds** beat every allow and every tier. They do not beat denies. Tested for every default kind under Full (`:211-217`).
- **Needs-review:** never matches. `read_row` marks it `decision="review"` and the scan only looks at `"allow"` and `"deny"`.
- **Absent kind ≠ 0:** yes, aside from F6.
- **Open points:** F5 (export/connect shortcut) and F6 (deny not consulted for malformed input).

### (2) Migration safety

- **Broad event allows leave the executable path before scoped evaluation is enabled:** yes. `reload()` runs in `evaluate`, so scoped evaluation never sees a legacy file that still contains an event allow.
- **Order of writes:** the v2 review marker is saved first, then the legacy file (`perm.c:272-273`). Neither snapshot is installed in memory until both writes succeed. A crash between the writes is handled: on the next load `unsafe && existing` still sets `dirty` and the legacy file is cleaned.
- **Restart-idempotent:** yes for the event case, via `imports` fingerprints (`:235`). There is no test of the partial-write window (v2 written, legacy write fails), only a load-failure test (`tests/test_signer_permissions.c:135-159`).
- **Re-sanitised on every load:** yes, tested for event allows (`:122-130`). Not true for non-event allows (F2).
- **An older GUI writing after migration:** an event allow is stripped on the next reload and becomes a review row. Non-event allows are imported as live authority (F2). Wildcard allows are silently ignored (F3).
- **Not converted:** bunker auto-approve kinds and the remote allowlist do not feed the store. I found no code path from them to the permissions file.

### (3) Store integrity

- **Record IDs:** `record-<sha256>` over length-prefixed principal, identity, operation and kind (`perm.c:52-64`). Section names never contain the principal. `read_row` re-derives the id and rejects mismatches (`:97-98`), so section-name injection is not possible. The test at `tests/test_signer_permissions.c:63-66` uses `=`, `[`, `]`, `|` and UTF-8.
- **Directory and file modes:** see F10.
- **Write failure:** `remember`, `replace` and `clear` mutate a copy and swap it in only after `save_file` succeeds. Memory is never ahead of disk.
  - `job_done` withholds the result with `FAILED`/`Internal` when the remember write fails (`svc.c:760-761`). The service test uses `RLIMIT_FSIZE` to cause a real write failure.
  - The signature or decryption has already been *computed* by then. Only delivery is blocked. This matches "persistence failure blocks confirming remembered access". Persisting before execution would be stricter.
  - `tests/test_signer_permissions.c:157` is labelled as a write failure but it makes the store path a directory, so the failure occurs at *load* time, not at save time.
- **Revision conflict:** returns `G_IO_ERROR_BUSY` and leaves memory unchanged. See F8 for the limits.

### (4) Trust-tier policy

- **Data, not logic:** yes. The public-publishing and required-confirmation lists are `static const guint32[]` in `signer_permission_policy.h`, separate from the evaluator.
- **Literal kind numbers:** none in `perm.c` or the policy header. Only constants from `nostr-kinds.h` appear. The only remaining literal is the existing `<= 65535` at `svc.c:~893`.
- **Where the list lives:** spec §5.4 wants the public-publishing set "maintained as a named list in cascadia-nips". This branch puts it in nip55l. The header comment and `docs/proposals/55L.md` admit that membership awaits §12.6 owner sign-off. Low, confirmed.
- **Full tier:** excludes `nip44_conversation_key`, `connect` and the required kinds. `raw_key_export` is not a known operation and returns ASK. See F5 for the explicit-allow path.
- **Unknown tier:** the default is Low (no lookup, or a NULL principal). An out-of-range enum value falls through to ASK. That is stricter than Low and is acceptable.
- **Low:** allows `get_public_key` only after `first_consented` (`perm.c:314`).
- **Medium:** ping, get_relays, get_public_key and the public-publishing set only.

### (5) G02 interaction

- **Position:** `resolved()` runs the pause and generation check (`svc.c:673`) and then `request_policy` (`:676`). That is step 4 followed by step 5, after identity pinning. `execute()` and `job_done` re-run the evaluator, so a deny that arrives after approval still wins.
- **Revision gate:** the worker rechecks `signer_permissions_revision()` before identity use and before `perform` (`svc.c:790`, `:808`). Expiry is covered by the fence (`perm.c:160-168`, `:185-195`) because every `evaluate` goes through `reload`, which recomputes the fence. See F1 for how the gate over-cancels.
- **`test_deny_queued_execution`:** kept. A persisted deny cancels queued work through `permissions_changed` before the decision returns (`svc.c:913`). One change: the deciding request itself is now finished as "denied by policy" rather than "user denied", because the old `r == decision` exclusion is gone.
- **Method allowlist and transport admission** are not applied by the evaluator. The header says that stays the adapter's ceiling.

### (6) Coverage gaps against §9

`signer/permissions` and `signer/trust-tiers` both exist and cover the headline rows. Gaps:

- The Medium-tier test (`tests/test_signer_permissions.c:199-200`) iterates the same public-publishing table that the evaluator uses. It would pass for any membership. There is no negative assertion that sensitive kinds (3, 4, 5, 10002, 22242) are not auto-allowed at Medium.
- No test for `first_party_default=TRUE` combined with a required-confirmation kind, with `connect`, or with `nip44_conversation_key` (F5).
- No test for a non-event legacy allow written after migration (F2) or for a wildcard allow (F3).
- No test for a corrupt legacy file with a valid v2 store (F4).
- No test for the v2-written / legacy-write-failed ordering (`perm.c:272-273`).
- The import tombstone is never actually exercised. The test clears the legacy entries together with the v2 rows (`:131`), so a surviving unchanged legacy row that must not resurrect a cleared grant is not covered.
- No test for approved work cancelled by an unrelated revision change (F1).
- No test that an unidentified caller (NULL principal) cannot receive a remembered permission, either through the service or `evaluate`.
- No restart/ABA test for expected-revision (F8).
- `gnome/nostr-seal/tests/test-nseal-signer.c` was not updated (F3).
- Nothing was run for this review. Please run `ctest -R '^signer/(permissions|trust-tiers)$'` and the nip55l approval-service suite before relying on the "OK" rows above.

## 4. Recommended order of work

1. F1 and F2 before this branch is merged. F1 is the visible regression. F2 contradicts a spec requirement.
2. F3 together with the `nostr-seal` fixture update.
3. F4, F5 and F6 as small changes in `perm.c`.
4. F7 and F8, when the G04 client store lands.
5. F9, F10 and the coverage gaps, along with G04.
