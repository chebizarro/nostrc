# Review: Groundhog W21, the encrypted-group UI (nostrc-9xf5, qp24.13 part 2)

- **Branch:** `groundhog/w21-mls-ui`, reviewed at `5a5dc8c5` (worktree branch `review/w21-mls-ui`): 3 commits on `2ca0dfee`.
- **Commits:**
  - 1d07c624: libmarmot `marmot_key_package_event_has_account_proof()`
  - 0de94e30: the encrypted-group UI
  - 5a5dc8c5: a test leak fix (`settings_with_discovery()` backend)
- **Reviewer:** independent peer review, as required by AGENTS.md.
- **Date:** 2026-09-30.
- **References:**
  - the privacy charter `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`: §2.2 surfaces 1 and 3, §7.5–§7.10, §7.15 #13, PD-8/PT-8, D7
  - the GhMlsService API (`gnome/groundhog/src/mls/gh-mls-service.h`)
  - `docs/reviews/groundhog-w20-mls-service-review-2026-09-29.md` and `-rereview-2026-09-30.md`

## Verdict: **APPROVED**

Nothing blocks the merge. The UI is correct and the privacy boundaries hold:

- **Consent.** Only accepted contacts are listed, using the same predicate as GhMlsService's `may_look_up()`. A person is looked up only after being ticked; a request never is.
- **Invitations.** Nothing is joined before Accept, nothing is looked up, and a stranger is shown only as an npub.
- **Composer.** The multi-delegate change is a strict superset of the single-delegate behaviour. NIP-17 and NIP-29 routing is unchanged.
- **Flag.** `GH_FEATURE_ENCRYPTED_GROUPS` is 0 in every compilation unit that reads it. The preview option is OFF by default and nothing in packaging or CI sets it.
- **libmarmot.** The new function *verifies* the account proof; it does not merely detect it. Its answer uses exactly the predicate that the service's Add enforces.
- **Tests.** The builds and all test suites pass on macOS and Linux, and Blueprint is in sync under 0.12.0 and 0.20.4.

Five medium findings (M1–M5) should be fixed or filed as beads **before the flag is flipped**. M1 is a one-assertion test and is best fixed before merge: it guards the only path this commit changes for release users.

## Verification performed

| Check | Result |
|---|---|
| `git submodule update --init third_party/nostrdb third_party/nsync` | ok |
| `cmake -S . -B /tmp/w21ur -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w21ur` | builds; no new warnings in groundhog or libmarmot. The build rewrote `apps/gnostr/data/ui/dialogs/gnostr-profile-edit.ui`, which I restored. |
| `ctest --test-dir /tmp/w21ur -R 'groundhog-\|marmot\|mls' -j6` (macOS, blueprint-compiler 0.20.4) | **96/96 passed**, with 4 skipped as on base (`-launch`, `-store-key-keyring`, `-background-gui`, `-notifier-gui`). `groundhog-mls-ui` and `groundhog-mls-ui-gui` both ran; the GUI test did not skip. |
| `BUILD_VOLUME=w21ur-linux scripts/groundhog-linux-ci.sh` (Ubuntu 24.04, blueprint-compiler 0.12.0, `dbus-run-session -- xvfb-run`, CI regex) | **74/74 passed**, including `groundhog-blueprint`, `groundhog-mls-ui`, `groundhog-mls-ui-gui` (7.7 s) and `groundhog-group-ui-gui` |
| `scripts/linux-gate.sh` (arm64, GCC) | all targets built; **424 smoke tests, all pass** |
| `python3 scripts/check-unsequenced-args.py` | clean |
| Blueprint drift | The 12 new or changed `.ui` files are byte-identical to 0.20.4 output. `groundhog-blueprint` passes under 0.12.0 in the container. All 8 new blueprints are on `GROUNDHOG_BLUEPRINTS`. |
| Mutation: `gh_mls_invitee_classify()` returns READY whatever the proof says | `groundhog-mls-ui` **fails** (the NEEDS_UPDATE case is covered) |
| Mutation: the picker starts every row's check in `setup()` (a lookup before a tick) | `groundhog-mls-ui-gui` **fails** (`key_package_asked()` guards PT-8) |
| Mutation: `gh_new_group_dialog_init()` no longer replaces the stack with `"form"` (at flag 0 the dialog opens on the chooser, with an insensitive "Encrypted Group" row) | **all four group/MLS UI suites still pass** (M1) |

The tree was restored after each mutation (`git status` clean) and the test binaries were rebuilt.

## Findings

### M1: the flag-off New Group path has no test

At flag 0, New Group must open directly on the relay form, with no chooser and no disabled "Encrypted Group" placeholder (charter §7.9: "There are no disabled placeholders in release builds").

- **What depends on it.** This is now enforced by a new line in `gh_new_group_dialog_init()`: `adw_navigation_view_replace_with_tags(…, { "form" })`. Without that line, the template's first page is the new `type` chooser.
- **Why it matters.** This is the only behaviour of 0de94e30 that release users see.
- **Evidence.** Dropping the line fails no test. `test_group_ui.c` never checks the initial page, and `test_mls_ui.c` always adds the encrypted page.
- **Fix.** In `test_group_ui.c`'s New Group test, assert that the visible page's tag is `"form"`, that no `"Encrypted Group"` row exists, and that `new-group.choose-encrypted` is disabled.

### M2: a removed member is shown as still in the group

libmarmot sets `MARMOT_GROUP_STATE_INACTIVE` only in `marmot_leave_group()` (`groups.c:1104`). GhMlsGroup:active mirrors that state (`group_refresh()`), and the service has no self-removal detection. So after an admin removes you:

- `gh_mls_send_reason()` returns NULL, so the composer sends. The kind 445 is accepted by a relay and looks sent, but no remaining member can read it.
- The Commit that removed you and everything after it is held, so the view shows "Unable to decrypt N messages yet". The number keeps growing, and "yet" promises something that will never happen. The blueprint comment already concedes "or never after a removal".
- Group Info shows "Up to date" or "Catching up…", and the member list of the last epoch you read.

This isn't a regression, and the flag is off. But it is the honest-copy counterpart of the removal this UI ships ("%s can't read anything sent to the group after this"): the removed person's own view must say so.

**Fix.** Detect that a merged or held Commit removes the account's leaf, or that the account is missing from the members of a later epoch. Then either mark the group inactive with a distinct state and copy (e.g. "You were removed from this group"), or drop "yet" once removal is known.

**Before the flip:** file a bead, service plus UI, and make it block the flip.

### M3: the flip conditions leave out nostrc-oya4

nostrc-oya4 is open (P2): GhMlsGroup:unreadable counts held Commits, so "Unable to decrypt N messages yet" overcounts. The GUI test shows +2 for one withheld Commit and one message. That is copy the charter would call dishonest.

The flip conditions don't include it:

- `gh-features.h`, VERSION_MANIFEST.md and nostrc-9xf5's notes name only nostrc-5rfp, nostrc-cpwf and nostrc-kzun.
- nostrc-9xf5's blocking dependencies are cpwf, dha5 and kzun. They list neither 5rfp nor oya4.

**Fix.** Add oya4, the M2 bead and 5rfp as blockers of the flip, both as bead dependencies and in the `gh-features.h` comment. That comment is the one-line switch people will read.

### M4: the real composer routing has no test

`gh_send_ui_add_delegate()` and `delegate_of()` in `gh-send-ui.c` are not linked into any test:

- `test-groundhog-group-ui` and `test-groundhog-mls-ui` both link `tests/ui/group-send-stub.c`, which re-implements the "first `handles()` wins" rule.
- No test attaches the real send UI with a delegate.

The following could therefore regress silently:

- a `set_delegate()` call after `add_delegate()` wiping the MLS delegate;
- `retry()` or `delivery_report()` looking up the wrong delegate;
- a draft being stored for a delegated room.

I checked all of these by reading:

- `handles()` is a pure backend test (NIP-29 / MLS), so the delegates are disjoint and a NIP-17 room never reaches one.
- `set_delegate()` is called once, at attach (`gh-group-ui.c:351`), before `gh-mls-ui.c` adds its delegate.
- The `n_delegates == 0` path equals the old NULL-delegate path, so the NIP-17 composer tests (`groundhog-composer`, `-multi-send`) still exercise it.

This isn't a regression, since G20b had the same gap. **Fix (follow-up):** one test on `GROUNDHOG_SEND_TEST_SOURCES` with two fake delegates and a NIP-17 room. It should assert the reason, the send, the retry, and that no draft is stored per backend.

### M5: the preview option has no guard against release builds

`-DGROUNDHOG_ENCRYPTED_GROUPS_PREVIEW=ON` only prints a CMake WARNING.

- **Why it hasn't leaked.** Nothing in `packaging/`, `debian/`, flatpak/snap manifests or the workflows sets it; I checked with `git grep`. The define is PRIVATE to `groundhog`, and the only reader of `gh-features.h` is `gh-app-services.c`, in that target. `gh-app-outbox.c` gets the value through `GhAppOutboxConfig.encrypted_groups` from the same file, so every compilation unit agrees.
- **How it could leak.** A reused build directory keeps the cached option. A packager who configures on top of a dev tree would ship the preview with no visible sign.

**Fix (cheap).** `FATAL_ERROR` when the option is ON with `CMAKE_BUILD_TYPE` in `Release`/`RelWithDebInfo/MinSizeRel`, unless a second explicit override is set. Optionally, also mark the About dialog's version, e.g. "(encrypted groups preview)".

### Low / nits

1. **Wrong send-error copy.** `gh-mls-ui.c` `delegate_send()` maps every `G_IO_ERROR_INVALID_ARGUMENT` from `gh_mls_service_send()` to "You left this group…". The service also returns that code for empty or invalid-UTF-8 text. The composer prevents both today, but the mapping should check `gh_mls_group_get_active()` rather than the error code. Likewise, `GH_MLS_SERVICE_ERROR_NO_RELAYS` gets the generic "couldn't be sent".
2. **Misleading Create reason.** `sync_create()` gives "Approve this device in Nostr Signer first." for every non-ready identity state. For `GH_MLS_IDENTITY_NONE` the row above says "Encrypted groups start once you're online", so the two disagree. Use the identity copy's own title for that state.
3. **NO_RELAYS mapping too broad.** `gh_mls_invitee_classify()` maps any `G_IO_ERROR_INVALID_ARGUMENT` to NO_RELAYS ("add a discovery relay in Preferences"). That is right for the lookup's no-usable-source case, but it would also cover a malformed pubkey. The pickers only list validated hex, so this is theoretical.
4. **Dead Info button.** With the MLS UI attached but no running service (a locked or ephemeral store), Conversation Info on an MLS room calls `gh_mls_ui_show_info()`, which returns FALSE, and nothing happens. Either fall through to the generic dialog or disable `win.conversation-info` for that room.
5. **Stale invitations count.** `on_watched_gone()` doesn't call `sync_invitations()`. The count stays until the next account-store "changed", which does come on a switch. Harmless today.
6. **New Group depends on NIP-29.** `win.new-group` is enabled only when the NIP-29 service exists (`gh-group-ui.c:93`), so the encrypted page also depends on NIP-29. Both services are created beside the same outbox, so this can't diverge today. Note it for when relay groups can be turned off.
7. **HIG.** The row title "On this device only" is sentence case; the neighbouring row titles use header caps ("Only Members Can Read", "Before You Create It"). Also, "_Accept"/"_Decline" repeat the same mnemonic on every invitation row. GTK cycles through them, but consider dropping the underline on per-row buttons.
8. **a11y.** "Unable to decrypt N messages yet" is not announced when it first appears. The locked-messages row sets the precedent of not announcing, so this is optional. Everything else is announced: KeyPackage check results, enrollment changes, and create status and errors (`gtk_accessible_announce`, assertive for errors).
9. **libmarmot test gap.** `test_key_package_account_proof_check` covers proven, legacy (absent) and invalid-event, but not *present-but-invalid*, which should return `MARMOT_ERR_KEY_PACKAGE_IDENTITY`. The path is `marmot_validate_key_package_event()` at `credentials.c:1131`, which W20's tests already cover through select. A one-line tamper case would pin the documented return value.
10. **Bead hygiene.** nostrc-kdxe is still OPEN, although its fix 9062dd2b is in this branch's base. Close it or say what remains.

## What was checked and is correct

- **libmarmot `marmot_key_package_event_has_account_proof()`.**
  - **What it checks.** It runs `marmot_parse_key_package_event()`, then `marmot_leaf_proof_status()`. The parse covers the id, signature, kind 30443, tags, KeyPackage validation, author binding, the KeyPackageRef, and rejects an INVALID proof. `marmot_leaf_proof_status()` calls `marmot_account_proof_verify()` against the leaf's credential identity and signature key. So `true` means a *verified* proof, `false` means an absent proof on an otherwise valid KeyPackage, and an invalid proof is an error. It verifies; it does not merely detect.
  - **Why it's enough for the UI.** It is exactly the service's Add policy (`groups.c` `parse_key_packages()`: INVALID, or ABSENT without `allow_unproven_members`, becomes `MARMOT_ERR_KEY_PACKAGE_IDENTITY`), and Groundhog never sets `allow_unproven_members`. Under `GH_MLS_SERVICE_ACCOUNT_PROOF` 0 (libmarmot < 0.10.0), every valid KeyPackage is READY, as the header documents. The check is advisory: create and add look everyone up again, and their errors carry their own copy (`GH_MLS_SERVICE_ERROR_NEEDS_UPDATE`).
  - It is additive, the KeyPackage is cleared on every path, and nothing is stored.
- **Privacy.**
  - Contacts come from `gh_mls_contacts_dup()`: NIP-17, non-request rooms, lowercased 64-hex, never the account. This is the service's own default consent, and requests and strangers are never listed (tested).
  - A lookup starts only from `on_toggled()` (ticked), `on_retry()` (ticked) or an account-generation change (ticked rows only).
  - Unticking cancels the lookup. A cancelled lookup's result is dropped before the weak row is touched: `check_done()` tests `error` first.
  - Group Info's add picker excludes current members and has the same rules.
  - Invitations are listed from local state, with the group name and member count from the Welcome.
  - Names come from `gh_contact_directory_get_display_name()`, which is cache-only and for accepted contacts only. A stranger gets "From npub…, not in your contacts".
  - Every row that shows peer-controlled text sets `use-markup: false` (invite, member, invitee and relay rows, and the entries).
  - The toast for a new invitation names nobody.
- **Honest copy** (compared with the charter):
  - the chooser rows match §7.9 word for word;
  - "Encrypted group · N members", with no "0 members" (plain "Encrypted group" when unknown);
  - the lock glyph's tooltip and accessible label "Encrypted group";
  - Leave: "…The other members keep counting you as a member until an admin removes you." This is true: libmarmot has no self-remove.
  - Remove: "can't read anything sent to the group after this". This is true now that nostrc-jnfp and nostrc-lz4f are closed, which the charter requires before any removal UI. The one gap is M2.
  - Group relays: "…not who is in it". This is true: kind 445 is sent under fresh keys and group relays get ephemeral AUTH only (`GH_AUTH_PURPOSE_MLS_ROUTING`).
  - The D7 "On this device only" note appears on both the create page and Group Info.
  - Create is insensitive and shows the first reason why (§7.1).
- **Owner/Admin.** The GroupData admin order is read from libmarmot, and the first admin is the Owner. Groundhog creates groups with the creator as the only admin, so that holds for Groundhog-made groups. The sorted `gh_mls_group_dup_admins()` fallback (used if the marmot read fails) would pick the wrong Owner, and nostrc-c7tk already tracks moving the order into the service. Add, remove and rename are shown and enabled only for an active admin, and a member gets no admin action even by activating the action directly (tested). Remove and Leave go through `AdwAlertDialog` with destructive responses.
- **Flag.**
  - `gh_mls_ui_attach()` runs only under `if (GH_FEATURE_ENCRYPTED_GROUPS)` in `gh-app-services.c`. Without it there is no chooser, no invitations action or sidebar entry, and no MLS delegate. An MLS room, which can't exist at 0, would show "Encrypted groups aren't available in this version yet."
  - `GROUNDHOG_HAVE_MLS_UI` compiles the attach code only where the send and group UIs exist.
- **Lifetimes.**
  - The dialogs weak-watch the service. `g_object_run_dispose()` notifies weak refs, so Group Info and Invitations close on an account switch even though the add picker holds a strong reference.
  - The New Group page cancels its create on dispose and ignores a late result (`gtk_widget_in_destruction()`).
  - `change_done()` holds a reference to the dialog.
  - `mls_ui_free()` disconnects from the watched service, the shown group and the view.
- **Sidebar.** "Group Invitations" is a flat button beside Message Requests. It is hidden in the requests view and at 0, and its accessible label is "Group Invitations, N invitations". Invitations alone keep the list page from showing the empty state.
- **Tests.** Mutations show that the display-free suite pins the copy, classification and view model (NEEDS_UPDATE, NO_RELAYS, UNREACHABLE, roles), and the three-account GUI suite pins PT-8. The GUI suite runs end to end:
  - chooser, create, open;
  - send and receive through the delegate;
  - invitation, accept, open;
  - badges, admin add, remove and rename, member without admin actions;
  - the withheld-Commit count;
  - leave, with its copy;
  - declined enrollment, then Try Again.

## Required before the flag flip (not blocking this merge)

1. M1: add the flag-off New Group assertion. This is a small test change and is best done before merging.
2. M2 and M3: file the removal-state bead, and make oya4, that bead and nostrc-5rfp blockers of the flip, both as bead dependencies and in the `gh-features.h` comment.
3. M4, M5 and the lows as follow-up beads, at the owner's discretion.
