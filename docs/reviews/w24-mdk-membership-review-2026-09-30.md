# Review: `marmot/w24-mdk-membership` (W24 slice A, nostrc-6ukh / nostrc-prrl)

- **Reviewer:** independent peer review (AGENTS.md), read-only worktree `/tmp/rv-w24-mdk-membership` at `74d9067f`
- **Commits:** `86e8ab02` feat(marmot,groundhog): MDK-compatible membership in legacy groups; `74d9067f` docs(analysis) addendum
- **Base:** master `82a615e4`
- **Brief:** `/tmp/w24/A.md`

## Verdict: **CHANGES-REQUIRED**

Most of the branch is well built:

- The profile classifier reads the authenticated GroupContext.
- Adopted and unknown profiles fail closed at every admission point.
- A failing proof is refused in every profile and mode.
- `allow_unproven_self` is off by default and only tests set it.
- The KeyPackage-evidence check binds event author, KeyPackage credential, member account and leaf signature key, and resists confusion attacks.
- The prrl "change-refused" path is honest in the tested flows.
- The tests kill every mutant I tried.

There is one blocker. The branch makes proof-less members the libmarmot default. That turns an existing gap in `marmot_commit_authorize()` into a live attack: **any non-admin member can swap a proof-less member's device for one they control, in a single Commit.** Every libmarmot peer accepts that Commit, with or without the "verified only" preference. That is the we6g/7vyi impersonation class, reachable for exactly the members this slice admits (B1, confirmed end-to-end).

Second, the automatic KeyPackage lookups, which the brief prescribes, go to the group's own relays and repeat every day. That leaks group membership in a way that conflicts with the privacy charter (H1, policy below).

Third, KeyPackage-based "verified" decays to "Identity not verified — Added by <themselves>" as soon as an MDK member does the self-update that the spec and MDK expect. Together with owkh, that makes the badge noise (M1, confirmed against real MDK 0.8).

## Gates run (macOS 27, `source /tmp/nostrc-macos27-env.sh`)

| Gate | Result |
|---|---|
| `git submodule update --init --recursive`; `cmake -G Ninja -DBUILD_GROUNDHOG=ON`; `ninja` | PASS (2468/2468). No warnings in touched files |
| `ctest -R "marmot\|groundhog\|gnostr-test-mls" -j6` | **97/97 passed**. Skipped (environment, as on master): groundhog-launch, -store-key-keyring, -background-gui, -notifier-gui |
| `python3 scripts/check-unsequenced-args.py` | PASS |
| `scripts/linux-gate.sh --sanitizers` (ASAN+UBSAN+LSAN, arm64) | **PASS, 49 tests**. Includes groundhog-mls-service, -mls-ui, -group-ui, -privacy-mls, -store-* |
| libmarmot unit tests under ASAN+UBSAN (macOS, `build-asan`; not in the Linux job) | **22/22 passed** (instrumentation verified: `libclang_rt.asan` linked) |
| MDK 0.8 harness (`-DBUILD_MDK_INTEROP=ON`, `mdk-group-unproven-member-default`, with a temporary probe) | PASS. Probe result in M1 |
| Docker | No named or anonymous volumes created. Temporary image tag removed |

### Mutation spot-checks (each reverted after the run)

| Mutation | Caught by |
|---|---|
| Drop `if (!legacy) return KEY_PACKAGE_IDENTITY` in `leaf_binding_check` | `test_non_legacy_profile_fails_closed` ("unknown profile, unproven Add") |
| Drop the profile gate in `marmot_tree_members_bound` | same test ("unknown profile, tree") |
| `allow_unproven_self = true` by default | `test_create_group_needs_enrollment` ("unsigned KeyPackage before enrollment") |
| Drop `author == member->account_pubkey` in `marmot_key_package_event_matches_member` | `test_protocol` ("a KeyPackage of another account does not match") |
| `device_matches()` always TRUE | `groundhog-mls-service /unproven-member-identity` |

## Findings

### B1 — Blocker (security): a non-admin can replace a proof-less member's device with their own; accepted in default and strict mode
`libmarmot/src/commits.c:258` (membership change judged by credential identity only) and `libmarmot/src/commits.c:176` (the "same member's new leaf" exemption).

**What goes wrong.** `marmot_commit_authorize()` decides whether a Commit is privileged (admin-only) with `members_changed = (!a != !b) || (a && !same_identity(a, b))`, slot by slot. Consider a Commit with `Remove(Y)` + `Add(KeyPackage claiming Y, with the committer's own fresh keys)`. RFC 9420 applies Removes before Adds, and the Add takes the leftmost blank leaf, which is Y's old slot. The MLS layer only checks that keys are unique. So the slot holds the same identity as before, and the Commit is judged **non-privileged**: no admin check. `leaf_binding_check()` then treats the new leaf as "the same member's new leaf" (`before && same_identity`). It accepts it whenever the old leaf was proof-less, **whatever `allow_unproven` says**.

**Failure scenario (confirmed end-to-end with a temporary test, not committed).**
1. Trio Alice/Bob (admins) and Charlie (no admin), default config.
2. Alice adds Mdk, an MDK 0.8 KeyPackage with no proof (this slice's default).
3. Charlie builds one real Commit with `[Remove(Mdk), Add(leaf claiming Mdk, Charlie's keys)]` plus an UpdatePath, and publishes it as a kind 445.
4. Bob's `marmot_process_message()` returns `MARMOT_OK` (COMMIT, committer Charlie).
5. Bob's `marmot_get_group_member_identities()` lists Mdk at leaf 3, UNPROVEN, **with Charlie's signature key**.
6. Alice processes the same Commit with `allow_unproven_members = false` (the "verified only" preference): also `MARMOT_OK`.

Charlie now holds the private keys of the leaf every libmarmot member attributes to Mdk. MLS lets any member derive any leaf's application ratchet, so Charlie can post messages that libmarmot authenticates and Groundhog displays as Mdk's. MDK 0.8 peers admin-gate Remove/Add, so they reject the Commit and the group forks. The impersonation lands on the libmarmot side. This violates the adopted spec too: removing or adding another member is admin-gated (`protocol-core/member-departure.md`, `app-components/admin-policy-v1.md`).

**Why it blocks this branch.** The root cause predates the branch. Before it, though, proof-less leaves existed by default only as the exempt Welcome sender. This slice admits every MDK/Amethyst member proof-less by default, and each one becomes a takeover target for any member. Groundhog's only mitigation is a Group Info badge ("Identity not verified — Added by Charlie") that M1 makes routine for legitimate members too. The "Only join groups where every member's identity is verified" preference doesn't help.

**Fix (in scope: libmarmot authorization).**
- Any slot filled by an **Add** is a membership change (privileged) and a new identity claim: it needs a valid proof, or `allow_unproven` in a legacy group.
- The same-identity exemption applies only to the committer's own leaf (UpdatePath) or a leaf replaced by its own Update proposal (by reference).
- `process_commit_impl()` already tracks `added_leaves[]` and the Remove proposals. Expose them to `marmot_commit_authorize()`; comparing signature keys alone is not enough, because MDK's self-update rotates the signature key legitimately.
- Add the regression test: a non-admin same-slot swap refused with `COMMIT_FROM_NON_ADMIN`; an admin swap of a proof-less leaf judged as a new claim (refused with proofs required).

### H1 — High (privacy, policy): automatic evidence lookups leak group membership to the group's relays, repeatedly
`gnome/groundhog/src/mls/gh-mls-service.c:1171` (group relays first in the lookup set), `:1400` (`verify_group()` on every `group_refresh()`), `:1036` (re-check after `GH_MLS_MEMBER_RECHECK_S`).

**What happens today.** For every proof-less device not yet confirmed, `verify_group()` starts a lookup without any user action:
- **When:** at join, after each Commit, and at every start once a NOT_FOUND verdict is older than 24 h.
- **Phase 1:** `REQ {authors:[member], kinds:[10002, 30443, 443]}` to the group relays plus the discovery relays.
- **Phase 2:** the member's NIP-65 write relays.

What protects it:
- AUTH is ephemeral (`GH_AUTH_PURPOSE_CONTACT_DIRECTORY`).
- Each phase is its own `GhRelayScope` with a random Tor isolation label, on the normal transports, so Tor and network mode are respected. That part is done right.

**Failure scenarios.**
- **The group relay learns who is in the group.** It already sees the group's kind 445 traffic under its `h` tag. Marmot signs those events with ephemeral keys precisely so the relay doesn't learn who the members are. Now, just after a Commit is delivered (or just after a client subscribes to `h=G` at startup), the same relay receives a burst of KeyPackage REQs naming the members' accounts. Separate Tor circuits don't hide that timing, and in direct mode it all comes from one IP. Group relays are often also popular discovery relays (one operator), so the separate phases aren't separate either.
- **Discovery and write relays learn the user's co-membership graph:** the set of accounts looked up together.
- **It repeats.** Because of M1 and owkh, most MDK members can *never* be matched. So every start after 24 h re-queries them for the life of the group: a periodic beacon that buys nothing.

The brief asked for group relays, so this is a specification problem rather than an implementation slip. nostrc-dujv tracks it at P2. It should block the `GH_FEATURE_ENCRYPTED_GROUPS` flip.

**Recommended policy.**
1. **No network by default.** Verify automatically only from evidence already in hand:
   - the KeyPackage the account used to add the device (done);
   - the Welcome sender's leaf, bound by the NIP-59 seal (owkh);
   - a chain of custody across the member's own key rotations (M1);
   - KeyPackage events already fetched for an invitation.
2. **Network lookups on demand only.** A "Verify identity" action per unverified member in Group Info, with honest copy such as "Groundhog will ask relays for this person's published keys. Those relays can see who you look up."
   - Ask the discovery relays and the person's NIP-65 write relays only. **Never the group relays**: they are the one place a lookup can be tied to a specific group.
   - Use one isolated scope per person, and never fire a burst for many people at once.
3. **No automatic periodic re-checks.** If an "automatic verification" option is wanted, make it opt-in with the same relay restrictions and randomized spacing (minutes, not milliseconds).
4. Raise nostrc-dujv to P1. Add a `test_privacy_mls` check that no evidence REQ reaches a group relay and none runs without user action. Update charter §2.2 and the privacy summary.

### M1 — Medium (UX honesty and privacy): a member's own self-update turns "verified" into "Identity not verified — Added by <themselves>"
`gnome/groundhog/src/mls/gh-mls-service.c:1015-1046` (`device_new()`: devices keyed by account:signature key; `added_by = last_committer`).

**What goes wrong.** MDK 0.8's `self_update()` rotates the leaf **signature key** (`groups.rs:1716-1760`, `self_update_with_new_signer`). MDK marks every joined group `SelfUpdateState::Required`, and the spec says a joiner SHOULD self-update promptly (`protocol-core/joining.md` step 14). Groundhog keys devices by signature key, so the rotated leaf is a brand-new device:
- it goes CHECKING, which fires a lookup (H1);
- no published KeyPackage can ever contain the new key, so it ends UNVERIFIED;
- `added_by` is the committer, which is the member themselves.

**Confirmed against MDK v0.8.0 `575ae29d`.** I added a temporary step to the harness case `mdk-group-unproven-member-default`: Dave self-updates after joining. Before: Dave `VERIFIED` (1). After: `identity 3 (UNVERIFIED), added_by = Dave's own pubkey`, and new KeyPackage subscriptions appeared in the log.

So Group Info reads "Identity not verified. Added by Dave. Groundhog couldn't confirm this account owns this device." for an honest Dave. With owkh (the creator is always unverified), nearly every member of an MDK group ends up flagged within a day. Users learn to ignore the badge, and that is exactly when B1-style takeovers would need it. The analysis addendum mentions only the creator case.

**Fix.**
- libmarmot reports each leaf rotated by its own Update or UpdatePath (old key → new key, signed by the predecessor) as distinct from Remove+Add (B1).
- Groundhog carries VERIFIED (or "invited you", per owkh) across such rotations, and does no lookup for them.
- Until then, never show "Added by X" where X is the member itself.

### L1 — Low (correctness, prrl edge): a refused competitor is called stale before the ordering is checked
`libmarmot/src/commits.c:2199`. `KEY_PACKAGE_IDENTITY` from `stage_inbound()` on the retained parent is mapped to `WRONG_EPOCH` *before* `commit_key_cmp()` decides whether that competitor beats the applied Commit.

**Scenario.** A forged-proof Commit (or, with the preference on, an unproven Add) races an honest Commit at the same epoch and wins the ordering. MDK peers don't check the proof and apply it. Groundhog silently stays on the losing branch, and the later-epoch messages it then holds show "Some messages in this group can't be read yet" for 15 minutes each. That is the false-wait symptom prrl is about. `authorize()` fills `key.committer`/`key.privileged` even on failure.

**Fix:** `memcpy(key.digest, digest, 32)` and remap only if `commit_key_cmp(&key, &rp.key) >= 0`; otherwise return `KEY_PACKAGE_IDENTITY` (the group really stops). Also correct the README sentence "a refused competitor ... cannot win".

### L2 — Low (prrl persistence): "change-refused" lives only in memory and doesn't hold the cursor
`gnome/groundhog/src/mls/gh-mls-service.c:1236` and `:1774`. `refused_json` is not persisted, and the refused event returns `EVENT_OTHER` without pinning the cursor. Held events pin it, but an accepted message of the old epoch can move the cursor more than `GH_MLS_SERVICE_CURSOR_OVERLAP` (600 s) past the refused Commit.

**Scenario.** Strict-mode user, an admin adds an MDK member, and another strict member keeps chatting at the old epoch for an hour. After a restart the refused Commit is never fetched again:
- "change-refused" is lost;
- the new epoch's messages show the false wait again;
- turning the preference off can no longer apply the change, because `on_verified_only_changed()` finds no `refused_json`.

**Fix:** pin the cursor behind the refused Commit (as `hold_event()` does), or persist its id/JSON in the store.

### L3 — Low (latent, adopted hook): the profile is re-derived every epoch, not pinned at join; an adopted→legacy downgrade isn't refused
`libmarmot/src/commits.c:233`, `libmarmot/src/profile.c:24`.

To the brief's question, "Is the profile bound at join and persisted?": **no.** `marmot_mls_group_profile()` reads the current GroupContext each time. That is authenticated, and you can't strip `app_data_dictionary` from a Welcome without forking yourself off. But a GroupContextExtensions Commit can rewrite the set. `authorize()` judges only that one Commit as non-legacy and does not refuse it. In the downgraded group, the next Commit admits proof-less leaves.

For a group with no 0xF2EE, `group_data_of(pre)` is NULL, so `gde_is_admin()` returns true and anyone may make that change. Today this is unreachable: libmarmot refuses Welcomes without 0xF2EE (`welcome.c:610-626`). The adopted-admission work must close it by refusing any Commit where `profile(pre) == ADOPTED && profile(post) != ADOPTED`, or by persisting the join-time profile. File it against nostrc-qp24.5.1.

### L4 — Low (copy honesty)
- `gh-mls-copy.c:277`: the default-mode refused copy always says "An admin added someone with a forged account proof". `KEY_PACKAGE_IDENTITY` also comes from a member's own UpdatePath that drops or garbles its proof (any member, not an admin, adds nobody) and from a profile change.
- With the preference on, `gh-mls-ui.c:104-111` and `gh-mls-group-info-dialog.c:224-232` choose the copy by the *current* preference, not the actual cause. A forged proof refused while strict reads "added someone whose identity can't be verified".
- `gh-mls-copy.c:244-258`: "Checking identity…" carries "Groundhog couldn't confirm this account owns this device" while the check is still running.
- `gh-preferences-dialog.blp:102`: "Only join groups where every member's identity is verified" uses "verified" to mean "proven". A member Groundhog itself shows as verified (KeyPackage match, no badge) is refused when the preference is on. Suggest "...where every member's app proves their account".

### Nits
- **N1.** The same `requires_proofs()` logic is copied four times: `gh-mls-service.c:4289`, `gh-mls-invitee.c:39`, `gh-mls-ui.c:107`, `gh-mls-group-info-dialog.c:228`. Expose one helper.
- **N2.** `gh_mls_group_get_member_identity()` returns PROVEN for any account with no Device (`gh-mls-service.c:683`). That includes when `marmot_get_group_member_identities()` failed in `refresh_devices()`, so a storage error shows no badges (fail-open in the UI). Return CHECKING/unknown when the devices were never loaded.
- **N3.** `mls-member:` records are never pruned (`gh-store-mls-identity.c`). One row per group, account and device key stays forever, including after the account leaves the group. The rows are hashed and encrypted, but they keep membership history after the user leaves. Delete them with the group.
- **N4.** `marmot_key_package_event_matches_member()` accepts expired KeyPackages with no age bound. That's reasonable (the account did sign that key), but note it in the threat model: a long-lost device key that later leaks stays "verified".

## Focus-area answers (brief)

1. **Security regression.**
   - A failing proof is refused everywhere: Welcome trees, own Adds, Public/PrivateMessage Commits, `parse_key_packages`.
   - Adopted and unknown profiles fail closed, with no sender exemption and no unproven leaf kept.
   - Our own leaves still carry proofs (`allow_unproven_self` is off by default and set only in tests).
   - Classification uses only authenticated GroupContext extensions, and stripping the dictionary from a Welcome forks the joiner.
   - **But:** B1 (non-admin same-slot device swap) and L3 (profile not pinned).
   - marmot-gobject keeps proofs required. No other direct libmarmot consumer exists.
2. **Verification logic.**
   - Sound: event id and signature, the KeyPackage and leaf signatures, credential == author, no invalid proof, author == member account, leaf signature key == member key. Confusion (X's KeyPackage for a leaf claiming Y) is refused and tested.
   - Expired KeyPackages are accepted by design (N4).
   - The cache is keyed by (group, account, signature key), so a leaf update with a new key naturally misses. That is correct for safety but causes M1.
3. **Privacy.** H1. Transports, Tor isolation and network mode are respected, but the lookups are automatic, recurring and include group relays. Policy above: on demand, never group relays, no periodic re-check.
4. **UI honesty / prrl / owkh.**
   - The main prrl path is honest and tested; edge gaps are L1 and L2.
   - owkh is worse than reported. Together with M1, essentially every MDK member is flagged within a day of joining.
   - Better sources:
     - the NIP-59 seal binding of the Welcome sender (owkh);
     - chain of custody across the member's own key rotations (needs a libmarmot rotation report);
     - KeyPackages already in hand.
5. **Memory safety.** The sanitizer gate (49 tests, LSAN on) and the libmarmot ASAN/UBSAN run are clean. New code zeroes the MLS state blob it loads (`profile.c`). `committer_pubkey_hex` is freed in `marmot_message_result_free()`. Groundhog's `Verify` and `Device` lifetimes are balanced.

## Suggested beads
- **B1:** libmarmot authorization: Add-filled slots are privileged and are new identity claims; regression test (P0/P1, blocks this merge).
- **H1:** raise nostrc-dujv to P1 with the policy above. Blocks the `GH_FEATURE_ENCRYPTED_GROUPS` flip.
- **M1:** libmarmot rotation report plus Groundhog chain of custody. Fold into owkh or link it.
- **L3:** pin or refuse profile downgrades, against nostrc-qp24.5.1.
