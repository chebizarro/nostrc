# Review: W24 slice B — standalone Proposals + SelfRemove (nostrc-2um6)

- Branch: `marmot/w24-selfremove` at `00eb9d49` (commits `bc809f75`, `e70d1930`, `00eb9d49`, on master `82a615e4`)
- Reviewer: independent peer review (AGENTS.md), 2026-09-30
- References: Marmot adopted spec `07da8ffb` (`protocol-core/member-departure.md`, `group-messaging.md`, `group-setup.md`), MDK v0.8.0 `575ae29d` (`messages/proposal.rs`, `groups.rs`), MDK v0.11.0 `946e0547`, OpenMLS 0.8.1 (`public_group/validation.rs`)

## Verdict: CHANGES-REQUIRED

The libmarmot core is careful and correct where it matters most. Proposals are authenticated: the membership tag and the signature for a PublicMessage, sender data plus the signature for a PrivateMessage. Each is bound to its group and epoch and sent by an occupied member leaf. Each ProposalRef is computed over the AuthenticatedContent and matches OpenMLS's own ref for an MDK-captured vector. A SelfRemove can only remove its sender. A Commit that mixes a SelfRemove with anything else becomes privileged. An admin's SelfRemove is refused at send, at receipt and at Commit authorization. The cache is epoch-pruned and forgotten when a removal becomes final.

The blocker is in convergence. A Commit that arrives before the proposal it references is dropped by Groundhog and never retried when the proposal lands. Groundhog's jitter then makes that member publish a competing Commit for an epoch the rest of the group has already left (H1, reproduced). There is also an interop divergence from MDK 0.8 on *when* to send SelfRemove (M1), and an unbounded-per-sender proposal cache (M2).

## Gates run

| Gate | Result |
| --- | --- |
| macOS build (`cmake -G Ninja -DBUILD_GROUNDHOG=ON`, `ninja`) | OK |
| `ctest -R "marmot\|mls\|groundhog\|interop\|kp_profile\|store"` | 106/106 passed (4 GUI tests skipped) |
| `scripts/check-unsequenced-args.py` | clean |
| `scripts/linux-gate.sh --sanitizers` | passed, 49 tests (includes groundhog-mls-service, -store-marmot, -mls-ui) |
| libmarmot `test_commits`, `test_marmot_interop`, `test_mls_key_package`, `test_kp_profile` under ASAN+LSan+UBSan (Linux CI image, `--rm` container, bind mount) | 0 sanitizer reports; these tests are not in the CI sanitizer list, so this covers the new proposal paths and the 1542-proposal OpenMLS passive-client replay |
| MDK 0.8 harness (`-DBUILD_MDK_INTEROP=ON`, `ctest -R mdk`) | 8/8, including `mdk-member-leaves` and `groundhog-leaves` |
| Revert spot-checks | removing the admin-SelfRemove refusal in `self_removers_not_admins()` makes `test_self_remove_admin_and_authorization` fail (test_commits.c:4945); forcing `sr_only = false` makes `test_self_remove_leaves_for_everyone` fail (non-admin committer refused, :4869) |

Version bumps: libmarmot 0.11.0 → 0.12.0 (MINOR) in CMake, meson and VERSION_MANIFEST, with compatibility notes. Groundhog is not bumped, which is justified because everything sits behind `GH_FEATURE_ENCRYPTED_GROUPS=0`. Both are consistent with AGENTS.md.

## Findings

### H1 (High) — A Commit that arrives before its proposal is dropped, then raced by our own competing Commit

`gnome/groundhog/src/mls/gh-mls-service.c:1411-1412`, `:1443-1444`, `:1452-1455`; `libmarmot/src/mls/mls_group.c:3671-3676`

1. A by-reference Commit whose ProposalRef is not in the local store fails with `MARMOT_ERR_MLS_PROCESS_MESSAGE` (mls_group.c:3675, "a missing referent is a genuine framing error").
2. `process_event()` treats that as "skipped" (`g_debug`, `EVENT_OTHER`). It is not held.
3. When the SelfRemove proposal arrives afterwards, `process_event()` only calls `departures_schedule()`. There is no `retry_held()`, and nothing re-offers the dropped Commit.
4. After the 1–4 s jitter, this member finds the leave still committable (its group is still at epoch E) and publishes a competing SelfRemove Commit for E.

Reproduced at the libmarmot level with a temporary test (Quad; Charlie leaves, Dave commits, Bob gets Dave's Commit first):

```
RV: Bob, Commit before proposal: type=0 err=-116 (MLS: failed to process message)
RV: Bob after proposal: pending=1 committable=1
RV: Bob, Commit redelivered: type=1 err=0
```

So libmarmot recovers if the Commit is offered again, but Groundhog does not offer it again until the next subscription (the 600 s `CURSOR_OVERLAP` REQ after a reconnect or restart).

**Failure scenario.** MDK 0.8 auto-commits a SelfRemove immediately (no jitter), so the proposal and the Commit often share a `created_at` second. Delivery order across the group relays, or within a backfill page, is then arbitrary. The same happens with any clock skew larger than 1 s against Groundhog's committers.

A Groundhog member that receives the Commit first:
- drops it;
- holds every later E+1 message as undecryptable;
- publishes its own Commit for E.

If its key sorts after the winner (`commit_key_cmp`: committer pubkey, then digest), it sits on a dead branch until it re-subscribes. If its key sorts first, every other member rolls back from the branch they already applied. Either way this is the non-convergence nostrc-w1m0 is about.

**Fix.**
- Have libmarmot return a distinct error for an unresolved reference (for example `MARMOT_ERR_PROPOSAL_UNKNOWN`) and have Groundhog hold that event like a later-epoch one.
- Call `retry_held()` when a proposal is accepted.
- Do not schedule a departures Commit while a held Commit for the current epoch exists.
- Add a service test for Commit-before-proposal.

### M1 (Medium) — SelfRemove is sent whenever every leaf advertises it; MDK 0.8 sends it only when the group *requires* it

`libmarmot/src/proposals.c:685`, `libmarmot/src/mls/mls_group.c` (`mls_group_self_remove_proposal`, `mls_group_members_support_proposal`); compare MDK 0.8 `groups.rs:1870-1900` (`try_self_remove`)

MDK 0.8 gates on `RequiredCapabilities.proposal_types ∋ SelfRemove` and otherwise falls back to a Remove of itself. Its comment gives the reasons:
- legacy groups and members can be `PURE_CIPHERTEXT`, so the peer rejects a PublicMessage handshake;
- "a mixed-composition group's non-admin-committable SelfRemove path would orphan on at least one peer".

libmarmot gates on leaf capabilities only. In a group whose RequiredCapabilities lacks 0x000a but whose leaves all advertise it, the problems compound:
- examples are a group created when one invitee was ≤0.11, or a group of MDK origin where a member's stored config is still PURE_CIPHERTEXT;
- Groundhog sends a SelfRemove PublicMessage that such an MDK peer drops;
- other members commit it by reference;
- that MDK peer cannot resolve the reference and stalls at E;
- `gh_mls_leave_copy(GH_MLS_LEAVE_EVERYONE)` meanwhile tells the user "The other members are told that you left".

**Fix.** Match MDK. Send SelfRemove only when the GroupContext requires it; otherwise use the device-only path, or a Remove-of-self proposal for an admin to commit as MDK does. The receive side can stay leaf-based, since it matches OpenMLS's commit-time check.

### M2 (Medium) — Proposal store: one 256-record cap shared by every sender, rewritten whole on each insert

`libmarmot/src/proposals.c:475-478`, `:504`, `:540-544`; `MARMOT_PROPOSALS_MAX` in `proposals.h`

Any member can fill a group's store. In legacy groups a non-admin can store any of Add, Update, Remove (of anyone) and GCE, which libmarmot never commits itself. Once 256 records exist, every further proposal is refused with `MARMOT_ERR_STORAGE_CONSTRAINT`, legitimate SelfRemoves included, until a Commit prunes. The parent epoch's records are kept as well, so one insider can refill the store each epoch.

Every accepted proposal also loads and re-serializes the whole blob: up to 256 AuthenticatedContents, each up to an event's size. That is write amplification an insider controls.

**Failure scenario.** Mallory, a non-admin, posts 256 Update proposals in epoch E. Carol's SelfRemove in E is refused everywhere. No member can commit Carol's leave until an unrelated Commit lands, and Mallory repeats the flood in E+1.

**Fix.**
- Set a per-sender quota per epoch (for example one departure and a few others).
- Evict non-departure records first.
- Or keep only the types this profile can act on.

Also consider keeping records per epoch instead of in one blob.

### L1 (Low) — A group created with no invitees requires SelfRemove forever, unlike MDK

`libmarmot/src/groups.c:557`, `:1340`

MDK 0.8 deliberately leaves an empty-invitee group's required proposals empty (`groups.rs` LCD comment: "empty stays empty", so later legacy adds still work). libmarmot sets `[0x000a]`. RequiredCapabilities is never rewritten afterwards (`replace_group_data_extension` keeps the existing one), so such a group can never admit a libmarmot ≤0.11 KeyPackage (its leaf lacks 0x000a). The code comment's justification, "every libmarmot and MDK 0.8 KeyPackage advertises it", is false for ≤0.11. VERSION_MANIFEST does note that 0.11 cannot join groups requiring SelfRemove.

**Failure scenario.** A Groundhog 0.12 user creates a group alone, then invites a friend still on 0.11. The Add fails with UNSUPPORTED.

**Fix.** Follow MDK (empty for no invitees), or fix the comment and surface a clear "needs update" error on Add.

### L2 (Low) — The SelfRemove capability check counts leaves removed by the same Commit; OpenMLS does not

`libmarmot/src/mls/mls_group.c:3395` (`commit_departures_check`); compare OpenMLS 0.8.1 `public_group/validation.rs:139-155`

OpenMLS intersects capabilities over the leaves that remain after the Commit's Removes. Consider an MDK admin Commit that removes the last member lacking SelfRemove and also references pending SelfRemoves (OpenMLS's default commit builder includes queued proposals). OpenMLS peers accept it, libmarmot rejects it, and the libmarmot member forks. This is an edge case. Exclude the Commit's departures from the check.

### L3 (Low) — "Leaving" can wedge silently

`gnome/groundhog/src/mls/gh-mls-service.c:2207-2218`

If a Commit that keeps us moves the group on and `marmot_self_remove()` then fails, the failure is only `g_debug`. Examples are `UNSUPPORTED` after an admin adds a member whose leaf lacks 0x000a in a group that does not require it, or `OWN_COMMIT_PENDING`. The group stays Leaving with every send gated, and the UI keeps saying "Waiting for another member to confirm it", which is now untrue. Surface the state and offer the device-only leave, which `gh_mls_service_leave()` already provides as "give up".

### L4 (Low) — "<name> left the group" only for leaves this client saw proposed, and only as a toast

`gnome/groundhog/src/mls/gh-mls-service.c:926-945`, `:1398-1399`, `departures_committable()`

The `leavers` set only fills from proposals processed locally. A member whose SelfRemove was committed while we were offline, or whose proposal we never received (see H1), silently disappears from the member list. The Commit summary already knows the self-removed leaves (`MlsCommitSummary.self_removed`). Expose them in `MarmotMessageResult.commit` and drive the notice from the Commit. Consider a persisted conversation row rather than a transient toast, since the brief asks to "show" it.

### L5 (Low) — Jitter herd and metadata

`gh-mls-service.h` (`GH_MLS_SERVICE_DEPARTURE_JITTER_MIN_MS/MAX_MS` = 1000/4000); `departures_fired()`

With N online Groundhog members, propagation latency L and a fixed 3 s window, about 1 + (N−1)·L/3 members fire before seeing the first Commit. That is roughly 6 competing Commits for N=30 and L=0.5 s, each one a kind:445 and a rollback for some members. Relays cannot read the contents (NIP-44 under the exporter secret). They do see a proposal followed by a burst of k Commits, which marks a departure and hints at the online-member count. The spec allows this; consider widening the window with group size. Ordering (`commit_key_cmp`: privileged, then committer pubkey, then digest) is deterministic and independent of the jitter, as the spec requires.

### Nit N1 — Leave copy for an admin with a pending change

`libmarmot/src/proposals.c:661-669`, `gh-mls-service.c:3563`

`self_remove()` checks for a pending Commit before checking admin status. An admin with an outstanding change therefore gets `OWN_COMMIT_PENDING`, which `gh_mls_service_leave_kind()` maps to `GH_MLS_LEAVE_EVERYONE` ("The other members are told that you left"). The action then returns BUSY, and later the answer turns into the admin copy. Check admin status first.

### Nit N2 — PrivateMessage proposals put the handshake ratchet back

`libmarmot/src/mls/mls_group.c:3117` (`private_handshake_open`, now shared with proposals)

For Commits the epoch ends anyway. For proposals the sender's generation key is never deleted within the epoch, which weakens RFC 9420 §9.2 forward secrecy for the rest of the epoch, and a replayed ciphertext decrypts again (deduped by ref and event id, so harmless). This is acceptable for interop with MDK 0.8's PrivateMessage Remove-of-self, but document it.

### Nit N3 — At most 64 departures per Commit

`mls_group.c:3386`, `MLS_COMMIT_SUMMARY_MAX`

A valid Commit with more than 64 SelfRemoves, or self-Removes by reference, is rejected while OpenMLS accepts it. This matters only for very large groups. A comment noting the deliberate cap would do.

## Focus answers

1. **Proposal authentication.**
   - Covered: sender is a member leaf (`leaf_occupied`, external and new-member senders refused), group_id and epoch bound, membership tag (PublicMessage), signature over the AuthenticatedContent under the arrival wire format, ProposalRef = RefHash("MLS 1.0 Proposal Reference", AuthenticatedContent), re-verified when the Commit resolves it.
   - A non-member cannot inject a proposal. A member cannot forge another's leaf, because the signature is checked against that leaf's key; for a PrivateMessage, knowing the secret tree is not enough.
   - Replay is deduplicated by event id and by ref, and is epoch-bound.
   - The cache is pruned at each Commit (the parent epoch is kept for a competing Commit) and forgotten on a final removal.
   - The bound exists (256) but is not per sender: M2.
2. **SelfRemove semantics.**
   - The removed leaf is always the sender's (`departure_leaf`). Inline SelfRemove and the committer's own SelfRemove are refused.
   - A Commit with a SelfRemove plus anything else is privileged, so admin-only. This matches spec `member-departure.md` "Validation" and `group-messaging.md`, and MDK 0.8's SelfRemove-only auto-commit filter.
   - An admin cannot self-remove, refused at send, receipt and authorization (MIP-03 and the spec). Any member may commit (MDK 0.8, MDK 0.11 and the spec agree).
   - Remove-of-self stays admin-committed, as in MDK 0.8.
3. **Capabilities.** The LCD rule matches MDK 0.8 byte for byte when invitees exist. It diverges for no invitees (L1). The *sending* gate diverges from MDK 0.8 (M1). MDK 0.8 KeyPackages advertise 0x000a, and the harness cases pass both ways.
4. **Groundhog.**
   - The leaving state is durable in libmarmot (`mls_group_leaving`), republished byte-identically within an epoch, re-proposed in each new epoch, and resumed by `resume_all()`.
   - Losing Commits use the normal pending and deferral machinery.
   - Convergence breaks on Commit-before-proposal (H1).
   - The metadata exposure is limited to timing (L5).
   - The copy is honest except as noted in M1, L3 and N1.
5. **Memory safety.** The CI sanitizer gate passed, and the libmarmot tests ran clean under ASAN, LSan and UBSan. Ownership in `mls_group_commit_by_ref`, `keep_opened`, `marmot_proposals_load` and the pending-record trailer checks out. The timer and publish callbacks in Groundhog are cancelled in `stop_generation()`, before the group can be finalized.

## Required before merge

H1. M1 and M2 are strongly recommended in this slice; if not, file them as beads blocking enabling `GH_FEATURE_ENCRYPTED_GROUPS`.

---

## Addendum: re-review of the fixes (`aef3aa1c`, `e5b16eb7`, `6df721dd`; tip `6df721dd`)

### Final verdict: CHANGES-REQUIRED (one new, narrow item: R1)

Every original finding is resolved, and each fix I spot-checked fails its test when reverted. But the M1 fix, together with Groundhog's "re-propose every epoch" rule, introduces a reproducible unbounded loop with an MDK 0.8 admin (R1). The fix is small. Once R1 is addressed this is an APPROVE.

### Gates (re-run on `6df721dd`)

| Gate | Result |
| --- | --- |
| macOS build, `ctest -R "marmot\|mls\|groundhog\|interop\|kp_profile\|store"` (with `-DBUILD_MDK_INTEROP=ON`) | 108/108 passed (4 GUI tests skipped); `groundhog-mdk-interop` ran against the real MDK 0.8 driver |
| `scripts/linux-gate.sh --sanitizers` | passed, 49 tests |
| libmarmot `test_commits`, `test_marmot_interop`, `test_mls_group`, `test_mls_key_package`, `test_kp_profile` under ASAN+LSan+UBSan (CI image, `--rm`, ASAN flags confirmed in `build.ninja`) | 0 reports |
| Reordered-delivery repro (my libmarmot probe from the first round) | Commit before proposal now returns `-69 MARMOT_ERR_PROPOSAL_UNKNOWN`, repeatably, nothing marked processed; once the proposal arrives the redelivered Commit applies (`type=1 err=0`) |
| Revert spot-checks | H1 Groundhog: dropping `retry_held()` on an accepted proposal makes `commit-before-proposal` fail (gh test :2305); mapping an unresolved ref back to `MLS_PROCESS_MESSAGE` makes it fail (:2299). M2: one shared slot for every sender makes test_commits :4880 fail. L2: counting removed leaves makes :5401 fail. All pass restored |

### Finding by finding

- **H1: resolved.**
  - Error and authentication:
    - An unresolved reference is now `MARMOT_ERR_PROPOSAL_UNKNOWN`. A reference to a proposal an earlier reference of the same Commit already consumed stays a processing error (`proposal_store_resolve` -2).
    - The Commit is authenticated first (`commit_authenticate()` runs before references are resolved, mls_group.c `process_commit_impl`), so only a member can make Groundhog hold one.
    - A Commit that both cites an unknown proposal and removes us is still recognised as a removal.
  - Groundhog hold and retry:
    - It holds the event (`awaits_proposal`), retries on every accepted proposal (`retry_held()` before `departures_schedule()`) and on each Commit pass.
    - It gives up after 16 tries or 600 s, on monotonic time. The overall held-queue cap still applies.
    - The junk mark only prevents re-holding: a later redelivery that now applies is accepted.
  - No competing Commit:
    - `departures_schedule()` and `departures_fired()` refuse while any held Commit awaits a proposal, and while a retry pass is running.
    - The author's `commit-before-proposal` test withholds the proposal on G and asserts that Bob holds the Commit, applies it on release, and stores nothing more after his longest jitter.
  - Residual (acceptable): a member can burn the 16 tries by re-wrapping its own valid proposals in fresh events, which are reported again as duplicates. That only matters for a Commit whose proposal never arrives, and the outcome is the pre-fix behaviour, healed by the 600 s overlap on the next subscription.
- **M1: resolved as asked; it causes R1.**
  - Send rule and wire format:
    - SelfRemove is sent only when `required_capabilities` lists 0x000a (`mls_group_requires_proposal`), as MDK 0.8 `try_self_remove()` does.
    - Otherwise a Remove of our own leaf goes out as a PrivateMessage under our handshake ratchet. The ratchet step is stored in the same transaction as the proposal record and the leave request.
    - Our own echo is recognised from the sender data before any decryption attempt (`MARMOT_ERR_OWN_MESSAGE`), so the spent key is never needed.
    - The bytes are MDK-compatible: in my probe, MDK 0.8 decrypted and processed the request.
  - Leave kind and copy:
    - `MarmotLeaveKind` and `GH_MLS_LEAVE_ADMINS` make the confirmation say "The group's admins are asked to remove you…", and the status says "Waiting for an admin to remove you."
    - That copy is honest when the admin runs libmarmot (Groundhog commits it after its jitter; `mdk-member-leaves` exercises the reverse direction). It is not honest with an MDK 0.8 admin; see R1.
- **M2: resolved.**
  - Only leave proposals are kept: a non-admin's SelfRemove, and in legacy groups a non-admin's Remove of itself. Everything else is `UNSUPPORTED` and not stored.
  - Storage is one slot per (epoch, authenticated sender leaf), at most 4 records, behind an index capped at 4096. The slot key comes from the signed sender, so a flooder can only fill its own slot. Its records are its own leave requests, so a successful flood just gets the flooder committed out.
  - An insert rewrites one slot (and the small index only for a new slot). Pruning keeps the current and parent epochs, so occupancy is bounded by members × 2 × 4.
  - I found no remaining flood path.
- **L1: resolved.** No invitees means `[]`, as in MDK, and the false comment is fixed. The consequence is discussed below.
- **L2: resolved.** Leaves the same Commit removes are excluded from the SelfRemove capability check, as OpenMLS does.
- **L3: resolved.** `marmot_cancel_leave()` exists. Groundhog drops a leave it cannot continue and raises "leave-failed" with honest copy, and sending works again. Nit N4 below.
- **L4: resolved.**
  - `MarmotMessageResult.commit.departed_pubkey_hexes` lists self-requested departures from the Commit itself, so "member-left" no longer depends on having seen the proposal. It is freed in `marmot_message_result_free()`.
  - The durable conversation row is tracked as nostrc-b25u.
- **L5: resolved.** The window is 1 s plus 250 ms per member, capped at 15 s, and the relay-visible burst is documented in `gh-mls-service.h`.
- **N1: resolved** (admin checked before a pending Commit). **N2 and N3: documented** in `mls_group.h`.

### R1 (High, new): leaving by Remove request livelocks with an MDK 0.8 admin

`libmarmot/src/proposals.c:809-814`, `:887-889`; `gnome/groundhog/src/mls/gh-mls-service.c` `after_commit()` → `leave_continue()`; MDK 0.8 `messages/proposal.rs:301-331`

When an MDK 0.8 admin receives a Remove-of-self, it calls `auto_commit_proposal()`. That builds a Commit with `consume_proposal_store(true)` and the filter `|queued| matches!(queued.proposal(), Proposal::SelfRemove)`. The Remove is filtered out, so MDK publishes an **empty** path-only Commit. The interop doc already records this ("MDK's own admin auto-commit … commits an empty Commit").

Groundhog applies that Commit, is still a member and still Leaving, and `leave_continue()` makes a fresh Remove request for the new epoch, which MDK answers with another empty Commit, without end.

Reproduced with a temporary MDK 0.8 harness case. Groundhog (Alice) creates the group with Carol (MDK), makes Carol the only admin, then Alice leaves:

```
RV leave kind=1 (ADMINS=1)
RV round 0: alice epoch 2, carol commits 1, carol still counts alice=1
RV round 0: alice now epoch 3, active=1 leaving=1, G events=4
RV round 1: alice epoch 3, carol commits 1, carol still counts alice=1
RV round 1: alice now epoch 4, active=1 leaving=1, G events=6
RV round 2: alice epoch 4, carol commits 1, carol still counts alice=1
RV round 2: alice now epoch 5, active=1 leaving=1, G events=8
```

**Failure scenario.** Every Groundhog-made group now leaves by Remove request (see below), and Groundhog supports promoting a member to admin. In any such group where the admin online is an MDK 0.8 client and no libmarmot admin commits first:
- the leaver and the MDK admin exchange a request and an empty Commit every round, for as long as both are online;
- the group's epoch churns, every member processes two events per round, and relays store them;
- the leaver never leaves, cannot send, and is told "Waiting for an admin to remove you."

A Groundhog admin online at the same time masks it: its privileged Commit outranks MDK's ordinary empty one in `commit_key_cmp`. So the existing tests never see it.

**Fix (any one is sufficient):**
- In Remove-request mode, do not re-propose automatically after a Commit by an admin that left a pending request of ours unconsumed. Stop instead, using the existing "leave-failed" path with copy along the lines of "an admin's app didn't act on it", or back off (for example a fixed number of re-requests per leave, with exponential delay).
- Keep the harness case above as a regression test, asserting that the loop is bounded.
- File an upstream MDK issue: `auto_commit_proposal` should admit `Remove` where the sender equals the removed leaf when the committer is an admin.

### Nit N4 (new): any `marmot_self_remove()` failure drops the durable leave

`gh-mls-service.c` `leave_continue()`

Every error except `OWN_COMMIT_PENDING` cancels the leave, including transient ones such as `MARMOT_ERR_STORAGE`. Cancel only on the definitive answers: `ADMIN_CANNOT_LEAVE`, `UNSUPPORTED`, `USE_AFTER_EVICTION`. Retry the rest on the next pass.

### The flagged consequence: Groundhog groups do not require SelfRemove (nostrc-8ndz)

**Acceptable for now, once R1 is fixed.**
- Groundhog creates every group alone and then Adds, so under MDK's empty-invitee rule its groups require nothing and non-admins leave by a Remove request an admin commits.
- That matches MDK 0.8 exactly and keeps 0.11 and older peers addable. A Groundhog admin commits such requests automatically, after its jitter.
- The cost is that a leave waits for an online admin and sends are blocked meanwhile. The copy says so, and leaving again gives up and leaves on this device only.
- Everything stays behind `GH_FEATURE_ENCRYPTED_GROUPS=0`.
- nostrc-8ndz (admin GroupContextExtensions adding 0x000a once every leaf supports it, or alongside the first Add) correctly describes the way back to any-member SelfRemove. It should be done before the flag is turned on, because with it R1-style admin dependence disappears for all-modern groups.
- R1 must not wait for nostrc-8ndz: groups with a ≤0.11 or legacy member will always stay on Remove requests.

---

## Final addendum: R1 and N4 (`777589a1`; tip `777589a1`)

### Final verdict: APPROVE-WITH-NITS

R1 and N4 are resolved. The loop is bounded and the bound is tested against the real MDK 0.8 client. The new committer field comes from the authenticated MLS sender. Definitive errors are told apart from transient ones. The nits below need no further review round.

### Gates (on `777589a1`)

| Gate | Result |
| --- | --- |
| macOS build, `ctest -R "marmot\|mls\|groundhog\|interop\|kp_profile\|store"` (`-DBUILD_MDK_INTEROP=ON`) | 108/108 passed (4 GUI tests skipped); `groundhog-mdk-interop` against the real MDK 0.8 driver |
| New harness case `groundhog-leaves-mdk-admin` (Docker free) | passes: exactly 2 rounds (`round 0: … leaving 1`, `round 1: … leaving 0`), then `NOT_PROCESSED`; a further MDK sync yields 0 proposals and 0 Commits; G stores nothing more; Alice sends again and MDK reads it |
| Revert spot-check | disabling the admin-Commit count in `leave_continue()` makes the case fail at test_mdk_interop.c:960 (`rounds < GH_MLS_SERVICE_LEAVE_REQUESTS`: 2 < 2) after round 1 still leaving; restored it passes |
| `scripts/linux-gate.sh --sanitizers` | passed, 49 tests |
| libmarmot `test_commits`, `test_marmot_interop`, `test_mls_group` under ASAN+LSan+UBSan (CI image, `--rm`) | 0 reports; `committer_pubkey_hex` is freed in `marmot_message_result_free()` and on the error path |

### The questions asked

- **Is the 2-request bound plus cancel safe? Yes.**
  - Each applied Commit whose authenticated committer is in the group's admin list, arriving while our Remove request waits, counts one miss.
  - The second miss calls `leave_give_up()` inside `leave_continue()`, *before* `marmot_self_remove()` would make the next epoch's request.
  - Proposals are epoch-bound, so when the leave is dropped no live request of ours remains for an admin to commit later. The "still a member, can send" state is therefore true.
  - `marmot_cancel_leave()` failing leaves the group Leaving, to be retried on the next pass (fail closed).
  - The bound is exactly 2 requests and 2 empty MDK Commits per leave attempt, as the harness case shows.
- **Is it honest? Yes, with one wording nit (F2).** "Your leave request wasn't processed by the group admin" is accurate: the admin's Commits came and kept the account.
- **Is `committer_pubkey_hex` sound? Yes.**
  - It is `key.committer`, set by `marmot_commit_authorize_ex()` from the credential identity of `committer_leaf` in the pre-Commit tree.
  - `committer_leaf` is the MLS sender: the PublicMessage framing, or the decrypted sender data for a PrivateMessage.
  - `commit_authenticate()` checks that sender against the framing (`from != sender_leaf`, `leaf_index != sender_leaf` → fail), verifies the signature with that leaf's signature key, and checks the membership tag.
  - The outer kind:445 pubkey (ephemeral) never enters it.
  - It is set only when a Commit is applied (linear, or the retained-parent winner). It is NULL for our own merge, so our own Commits never count as admin misses.
  - Groundhog compares it against `group->admins`, the pre-Commit admins (refresh runs after the check).
- **Does N4 distinguish definitive from transient errors? Yes.**
  - Cancel only on `ADMIN_CANNOT_LEAVE`, `UNSUPPORTED` and `USE_AFTER_EVICTION`.
  - Everything else, including `OWN_COMMIT_PENDING` and storage errors, keeps the durable leave and calls `schedule_retry()`, which backs off exponentially with jitter up to `RETRY_MAX_S`.
  - See F4 for the persistent-but-unlisted case.

### Nits (no re-review needed)

- **F1 (Low): any admin Commit counts as a miss, not just one that dropped our request.**
  - Consider a group with a libmarmot admin who would commit the request after its jitter (up to 15 s). If other admin activity (a rename, an Add) lands twice inside that window, the leave stops with "not processed" although it would have succeeded.
  - The outcome is safe (still a member, can retry) but spurious.
  - Refinement: count only Commits that carry no proposals (MDK's empty auto-commit) or that consume none of ours.
- **F2 (Nit): the "not processed" status suggests a choice that one press does not give.**
  - The status says "You can leave on this device only…". But after it, `gh_mls_service_leave_kind()` returns `ADMINS` again (the group is no longer Leaving), so the next Leave re-sends a request, and device-only takes a second Leave ("give up waiting").
  - Either return `GH_MLS_LEAVE_DEVICE` while `leave_failure == NOT_PROCESSED`, or say "Leave again to ask once more, or …".
- **F3 (Nit): the miss counter lives in memory.** `leave_misses` resets on restart, so each app start allows 2 more requests. That is bounded per session; persisting it with the leave request would make it bounded per leave.
- **F4 (Nit): some persistent errors retry forever.** Unlisted errors such as `OWN_LEAF_NOT_FOUND`, `GROUP_NOT_FOUND`, or `DESERIALIZATION` of a damaged leave record retry indefinitely (backed off) with sends gated. That is fail-closed, which is defensible, but the first two are definitive in practice and could join the cancel list.
- **Test coupling (Nit):** the harness case asserts against `GH_MLS_SERVICE_LEAVE_REQUESTS` itself, so it guards the counting logic, not the constant's value. That is fine, and the revert check above confirms it catches the loop.

### Status of every finding

| Finding | Status |
| --- | --- |
| H1 Commit before proposal | resolved (round 2) |
| M1 SelfRemove only where required | resolved (round 2) |
| M2 proposal flooding | resolved (round 2) |
| L1-L5, N1-N3 | resolved or documented (round 2) |
| R1 Remove request livelock with MDK 0.8 admin | **resolved** (bounded at 2, harness-tested); upstream MDK issue nostrc-laxu |
| N4 transient errors drop the leave | **resolved** |
| nostrc-8ndz (Groundhog groups leave via admins) | acceptable for now, as judged in round 2; do before enabling `GH_FEATURE_ENCRYPTED_GROUPS` |
| F1-F4 | nits, may be filed as follow-ups |

---

## Rebase addendum: onto master `aae023f5` (slices C and G); tip `edac0960`

### Verdict: APPROVE-WITH-NITS

The rebase lost no behaviour, and the new integration fix is sound. It cannot be used to push a non-departure Commit. One new low-severity nit (G1).

### Gates (on `edac0960`)

| Gate | Result |
| --- | --- |
| macOS build, `ctest -R "marmot\|mls\|groundhog\|interop\|kp_profile\|store"` (`-DBUILD_MDK_INTEROP=ON`) | 109/109 passed (4 GUI tests skipped); `groundhog-mdk-interop` against MDK 0.8, including the new `groundhog-member-commits-leave` (3d) |
| `scripts/linux-gate.sh --sanitizers` | passed, 50 tests |
| Revert spot-checks | retrying departures with the admin-only `check_change()` makes 3d fail (test_mdk_interop.c:1047, "Bob's leave committed by Alice did not happen"); dating the `marmot_commit_pending_proposals()` Commit with `marmot_now()` instead of the floor makes `test_departure_events_follow_the_floor` fail (test_commits.c:6697, "refused at the bound"). Both pass restored |

### (a) Conflict resolutions against C's created_at floor

- **Range-diff.** `git range-diff 777589a1~7..777589a1 aae023f5..edac0960` matches all seven commits:
  - `00eb9d49` and `6df721dd` are unchanged (`=`).
  - The other five differ only in:
    - VERSION_MANIFEST and README reconciliation (libmarmot and groundhog 0.12.0 already on master, so B folds in with no further bump);
    - the slice-C signatures `group_data_of(pre, pre, &gde)` and `marmot_commit_build_event(…, created_at)`;
    - the `gh-store-marmot.c` label comment;
    - hunk realignment in `test_commits.c`.
- **Source interdiff.** I diffed B's own source patch before and after the rebase (`82a615e4..777589a1` vs `aae023f5..edac0960~1` over `libmarmot/src`, `libmarmot/include`, `gnome/groundhog/src`, ignoring offsets). The *only* differences are the three expected resolutions:
  1. `removal_key()`: `group_data_of(pre, pre, &gde)`.
  2. `commit_pending_proposals_impl()`: `marmot_next_group_event_time(m, nostr_group_id, true, …)`, a Commit, so it can return `MARMOT_ERR_EVENT_RATE`. It runs after `mls_group_commit_by_ref()` on the clone and before anything is staged, so a refusal stages nothing. The new libmarmot test asserts this.
  3. `self_remove()`: `marmot_next_group_event_time(…, false, …)`, a non-Commit that is never rate-refused. This covers both leave kinds (SelfRemove and the PrivateMessage Remove request share this producer). It runs only on the non-dry path, inside the call's transaction. A same-epoch re-publish returns the stored bytes with their original created_at, as the spec requires.

  There are no Groundhog source differences at all.
- **Tests.** Comparing the `RUN(...)` and `g_test_add_func` sets of every touched test file at `777589a1` and `edac0960~1`, the changes are only additions (master's C and G tests). No slice-B test was dropped. The range-diff hunk around `test_private_remove_self_committed_by_admin` is realignment: that test was already renamed in `aef3aa1c`.

### (b) `edac0960`: departure retry after `EVENT_RATE`

- **Is the rule sound? Yes.**
  - The relaxed re-check (`check_departures()`: running, our active group, not leaving, `departures_committable()`) applies only when `op->kind == OP_DEPARTURES`.
  - That kind is created at a single internal site, `departures_fired()` (gh-mls-service.c:2282), together with the fixed producer `produce_departures`. The retry re-stages `retry->producer`, which is the same `produce_departures`.
  - No public API creates an `OP_DEPARTURES` op or supplies its producer.
- **Can a non-admin use it to push a non-departure Commit? No.** Three independent checks stand in the way:
  1. The only producer reachable is `marmot_commit_pending_proposals()`. It commits nothing but the *selected* departure proposals: a non-admin's SelfRemove, and a Remove-of-self only when `may_commit_privileged()` holds for us.
  2. libmarmot authorizes before staging (`marmot_commit_stage_pending_ex()` → `marmot_commit_authorize_ex()` with the departure summary). The Commit is ordinary only if it is SelfRemove-only; anything else needs an admin.
  3. Every receiver applies the same authorization.

  The Groundhog check is a scheduling gate, not the security boundary, and relaxing it widens nothing.

### Nit G1 (Low, new): the retry re-check lacks the H1 guard

`gh-mls-service.c` `check_departures()`

The commit says the retry is "re-checked as `departures_fired()` does". But `departures_fired()` also refuses while `held_awaits_proposal(group)` (H1) and while `round` or `pending_commit` is set. `check_departures()` checks neither.

`round` and `pending_commit` are covered anyway, because libmarmot refuses with `OWN_COMMIT_PENDING`. The held-Commit guard is not covered. A Commit citing a proposal we lack can arrive during the rate-retry window (1 s, up to 5 times). The retry would then publish our own SelfRemove Commit, competing with one the group may already have applied.

Ordering still converges (both are ordinary; `commit_key_cmp`), so the cost is the churn H1 removed, in a narrow window. Fix: share one predicate between `departures_fired()` and `check_departures()`, including `held_awaits_proposal()`.

### Carried nits

F1-F4 from the previous addendum are unchanged. None blocks the merge.
