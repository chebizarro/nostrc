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
