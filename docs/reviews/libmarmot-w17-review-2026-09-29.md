# libmarmot W17 review: Welcome path secrets (nostrc-il4i) and Commit publication/ingestion (nostrc-9ata)

- **Reviewer:** independent peer reviewer (AGENTS.md "Pre-Push Requirements → Peer Review")
- **Branch:** `marmot/w17-welcome-commits` at `7e9f8808`, on `74ced159`
- **Commits:** `2f487f8a` (nostrc-il4i), `7e9f8808` (nostrc-9ata)
- **Date:** 2026-09-29
- **Verdict:** **REQUEST CHANGES**. Blocking findings: **B1, B2, B3**.

**Specs checked**
- RFC 9420 §12.4.3.1.
- Marmot at `26fa6a6`, the adopted spec: `protocol-core/convergence.md`, `transports/nostr.md`, `app-components/admin-policy-v1.md`, `mip-coverage.md`.
- The legacy MIP-01 and MIP-03 texts at `cc73aa8`. That is the last commit before they were removed; it is the profile libmarmot's 0xF2EE / NIP-44 wire format follows.

No code or beads were changed. Repros and mutations ran in throwaway worktrees (`/tmp/rr17`, `/tmp/rr17cf`), which were removed afterwards.

## Summary

**nostrc-il4i is correct and well tested.**
- The committer puts the lowest common ancestor's path secret in `GroupSecrets`.
- The joiner derives that node and every node above it on the committer's filtered path, compares each public key with `sodium_memcmp`, and installs the keys only after the whole Welcome verifies. On failure it removes them again.
- Parsing is strict, and secrets are wiped.

**nostrc-9ata closes the biggest functional gap.** Other members can finally apply a Commit.
- Authorization runs on a staged copy.
- Rejections store nothing.
- The version bumps (libmarmot 0.5.0, marmot-gobject 1.2.0) are the right size.

**Blocking**

1. **B1: the new Gnostr rename strands the admin.** Gnostr publishes the rename Commit *unsigned*, *after* libmarmot has already merged it. Relays cannot accept it, so every Gnostr rename moves the admin into an epoch no member can reach.
2. **B2: multi-KeyPackage producers now fork the group every time.**
   - `marmot_add_members` and `marmot_remove_members` with more than one entry, and `marmot_create_group` with three or more invitees, still make N local Commits and publish only the last (nostrc-wc6v).
   - Now that members process Commits, each such call permanently splits the committer from the group. I verified this.
3. **B3: the documentation does not disclose permanent splits.**
   - The same-epoch rule is implemented only for one-Commit branches, without the higher spec criteria. Two honest members can diverge permanently; I verified this, and it isolated the only admin.
   - The README calls this "a bounded subset" but does not say a group can split for good, or that the tie-break deliberately departs from the legacy MIP-03 rule that libmarmot's own wire profile follows.

I am not asking for full convergence in this branch. It is tracked as nostrc-w1m0; see B3 for what should change.

## Answers to the review questions

**1. Is the tie-break exactly what the spec says?**

Only its last three steps. Adopted `convergence.md` ("Branch selection", "Same-epoch races") orders eligible branches by:
1. `effective_commit_depth`
2. witness quorum
3. `app_witness_score`
4. privileged before ordinary
5. lower committer (x-only key)
6. lower SHA-256 digest

`commit_key_cmp()` (`commits.c:115-122`) implements steps 4–6 exactly: 32-byte x-only memcmp, and SHA-256 over the Commit `MLSMessage` bytes, the same digest producers compute (`groups.c:689`). It compares only one-Commit branches from the retained parent, and it skips steps 1–3.

The legacy MIP-03 that this wire profile follows (`03.md` "Commit Message Race Conditions") says something different: earliest outer `created_at`, then the smallest event id. The adopted spec forbids exactly that transport metadata, so choosing the adopted suffix is defensible. But it is a deliberate departure, and a group mixing libmarmot with a peer on the legacy rule can pick different winners. The README should say so (B3).

**"Privileged"** is inferred from the state difference: members changed, or extensions changed (`commits.c:144-164`). The spec instead defines it as "the authorization rule requires an admin". The two differ only for Commits outside the legacy MIP-03 non-admin allowance (N4).

**Can two honest members diverge? Yes (verified; B3).**

The repro uses the `test_commits` helpers, unchanged code at `7e9f8808`:
1. Alice (the only admin) renames. Her Commit is privileged and wins at depth 1.
2. Concurrently, Bob self-updates.
3. Charlie receives Bob's Commit first, applies it, and self-updates on top of it.
4. Bob applies Charlie's Commit (epoch 4).
5. Alice rejects Bob's Commit (`WRONG_EPOCH`) and cannot decrypt Charlie's (`MARMOT_ERR_NIP44`, -22).
6. Bob and Charlie reject Alice's Commit (`WRONG_EPOCH`, -64).

**Final state:** Alice is at epoch 3 on the rename branch. Bob and Charlie are at epoch 4 on the other branch, with a different exporter secret.

The adopted spec selects the depth-2 branch, so Alice should switch, but libmarmot keeps only one retained parent. The split is permanent, and because only admins may add or remove members, the members cannot repair it. The one-level race itself does converge in either arrival order; `test_same_epoch_race_converges` pins that.

**2. Is replacing an applied Commit safe?**

Mostly, with the gaps the README admits. `marmot_commit_persist(rp.parent → post)` overwrites these records:
- the epoch N+1 exporter secret, so the losing branch's key is no longer stored;
- the MLS state;
- the retained parent, now holding the new winner's key;
- the group record.

I found no key-retention defect. The parent epoch's full state is kept for exactly one epoch and overwritten on the next transition, which is inside the spec's `max_rewind_commits = 5`.

What is missing (N1, tracked in w1m0):
- **Delivered messages stay delivered.** App messages already decrypted under the losing branch remain delivered and marked processed. The spec requires them to be invalidated and withdrawn.
- **Winning-branch messages are dropped.** Messages sent on the winning branch that arrived before the switch failed NIP-44 and were not retained. The spec requires `transport_deferred` retention and a retry after the epoch changes, so after a switch these are lost unless a relay re-sends them.
- **Losing-branch Welcomes strand the joiner.** A joiner admitted by the losing Commit's Welcome ends up on a dead branch.
- **Own outgoing messages vanish.** The member's own messages sent on the losing branch are silently unreadable to everyone else.

**3. Is the storage rollback correct under partial failure?**

For write-error returns, yes. Writes go exporter → parent → state → record, and compensation runs newest first (`commits.c:336-360`). The caller's in-memory `MarmotGroup` is mutated before the writes, but every caller frees it on failure (`messages.c`, `finish_local_commit`).

Caveats:
- **N2: process crashes are not covered.** A crash between the `mls_group` write and `save_group` leaves the record at epoch N and the MLS state at N+1. Trial decryption uses the record's epoch (`messages.c:643-646`), so every later kind:445 fails NIP-44 and the group is stuck for good.
- **N3: undo failures are ignored.** Undo return values are discarded.
- **N3: a read error counts as "absent".** In that case the compensation *deletes* a record that exists, the parent or the exporter secret.
- **No test.** The undo path has no fault-injection test, and removing it passes every test.

**4. Is the wire change versioned and documented?**

Versioned, yes:
- libmarmot 0.4.1 (PATCH, il4i) → 0.5.0 (MINOR: new required `out_commit_json` plus the Commit wire change).
- marmot-gobject 1.2.0 (MINOR, new async API and signal).
- gnostr and groundhog unchanged because they are unreleased.
- `VERSION_MANIFEST.md` records every decision.

The README's 0.5.0 entry covers the API migration, the NIP-44 Commit envelope and the epoch rules. It is missing the known permanent-split modes and the spec basis for the tie-break (B3), and wc6v (B2).

**5. Do the new tests fail without the fixes?**

Mostly.

| Mutation | Result |
|---|---|
| Skip Welcome path-key install | `test_mls_group` and interop abort |
| Skip public-key verification | `test_mls_group` aborts |
| Sender omits `path_secret` | `test_mls_group` aborts |
| Disable Commit routing | `test_commits` aborts |
| Never replace / always replace | `test_commits` aborts |
| Digest before committer | `test_commits` aborts |
| No admin check | `test_commits` aborts |
| No `nostr_group_id` check | `test_commits` aborts |
| **Drop the privileged step** | fails only **7 of 10** runs: the test's keys are random, so it depends on which of Alice or Bob sorts lower |
| **Committer-identity check** | survives |
| **GroupData-removal check** | survives |
| **Outer-epoch match (`epoch != outer_epoch`)** | survives |
| **Own-sender guard** | survives |
| **Persistence undo** | survives |

The survivors are recorded as N5.

## Findings

### B1 (High, blocking): the Gnostr rename publishes an unsigned Commit after it has been merged locally

**Where**
- `apps/gnostr/plugins/mls-groups/ui/gn-group-settings-view.c`, `on_rename_committed` → `gnostr_plugin_context_publish_event_to_relays_async(commit_json, …)`.
- `marmot_commit_build_event()` returns an *unsigned* event (`commits.h:71-79`: "the caller signs with an ephemeral key").
- `marmot_gobject_client_update_group_metadata_finish` returns it unchanged (`marmot-gobject-client.c:1127-1149`).
- The plugin publish API requires a signed event (`gnostr-plugin-api.h:800-821`) and sends it verbatim (`publish_to_relays_thread_func`).
- Nothing on this path signs anything: no signing call exists in marmot-gobject or the mls-groups plugin.

**What happens.** libmarmot has already persisted the new epoch. The unsigned kind:445 cannot be accepted by a relay (`transports/nostr.md`: a fresh ephemeral key MUST sign each kind:445, and receivers MUST verify id and signature). Publish failure is only a `g_warning` (`:440`). Every rename therefore moves the admin to an epoch no member can reach, so the admin can no longer read the group or be read by it.

The same unsigned publish affects kind:445 app messages and the add-member Commit (`:383-391`). That predates this branch and should get its own bead, but the rename is new here.

**Fix**
- Sign every kind:445 that marmot-gobject returns with a fresh ephemeral key, per event, never the account key (MIP-03).
- Do not merge a Commit before a relay accepts it. Legacy MIP-03 says producers "MUST NOT apply the Commit locally until at least one relay confirms receipt"; nostrc-w1m0 and qp24.7 cover the staged-publish work.
- Until both are in place, hide the rename row.

### B2 (High, blocking): multi-KeyPackage / multi-member producers now guarantee a permanent fork

**Where**
- `groups.c:789-831` (`marmot_add_members`) and `:878-900` (`marmot_remove_members`): one Commit per entry, only the last published, and the final state persisted.
- `marmot_create_group` behaves the same way. It is tracked as nostrc-wc6v (P1).

**Verified.** In a Trio group at epoch 2, Alice calls `marmot_add_members` with Dave's and Eve's KeyPackages. Alice ends at epoch 4. Bob and Charlie get `MARMOT_ERR_NIP44` (-22) for the published Commit and stay at epoch 2, permanently, with the admin isolated.

**Reach.** Gnostr's create-group dialog passes one KeyPackage per selected invitee (`gn-create-group-dialog.c:390-415`). With three or more invitees, the early invitees are left at stale epochs. Gnostr's add-member path passes exactly one.

**Before vs now.** Before this branch no member processed Commits, so this is not a new failure. But 0.5.0 advertises these producers' Commits as "published and processed".

**Fix.** Either fix wc6v (one Commit with N proposals and one Welcome), or fail closed with `MARMOT_ERR_UNSUPPORTED` when there is more than one entry (`create_group`: more than two invitees) and document it in the README.

### B3 (High, blocking as documentation): permanent splits are possible but not disclosed; the tie-break's spec basis is not stated

The depth-2 split above is a safety violation of `convergence.md` ("clients … MUST select the same canonical branch"). It is reachable whenever someone commits on top of a same-epoch loser before the winner arrives. Full convergence (nostrc-w1m0) can follow in a later branch, but the 0.5.0 README should say, in plain terms:
- that concurrent Commits can permanently split a group, including isolating an admin;
- that `CommitOrderingSuffix` was chosen over legacy MIP-03's `created_at`/event-id rule, and what that means for mixed-implementation groups.

Given the reach, nostrc-w1m0 (P2) should be raised to P1.

### N1 (Medium): replacement does not invalidate the losing branch, and winning-branch input is not retained

See answer 2. Tracked in nostrc-w1m0. The README mentions the lack of invalidation; it should also mention dropped input and stranded joiners.

### N2 (Medium): a crash between writes can make a group permanently undecryptable

See answer 3. The fix is small: when loading, reconcile `group->epoch` with the stored MLS state's epoch (the MLS state and exporter secret are written first), or key trial decryption on the MLS state's epoch. Related to nostrc-qp24.7.

### N3 (Low): the rollback's compensation is best effort

- Undo results are ignored.
- `mls_load` and `get_exporter_secret` errors are treated as "absent", so the undo can delete an existing parent or exporter record.
- There is no fault-injection test.

Distinguish not-found from error, and add a storage wrapper that fails the Nth write.

### N4 (Low): authorization and "privileged" come from the state difference, not the proposal type

`marmot_commit_authorize` treats any Commit that changes neither members nor extensions as ordinary. For example, a non-admin Commit carrying PSK proposals with a path is accepted. Legacy MIP-03 ("Commit Messages") allows non-admins only self-update and SelfRemove-only Commits. The adopted spec defines privileged by the authorization rule. Libmarmot peers agree with one another, but an MDK peer would reject such a Commit. Classify by proposal set.

### N5 (Low): test gaps

- Make privileged-vs-ordinary deterministic: generate keys until Alice's key sorts above Bob's.
- Add cases for the committer-identity change, GroupData removal, an outer epoch that differs from the header epoch, a different Commit from our own leaf, and persistence undo.

## il4i against RFC 9420 §12.4.3.1: conforms

- **Committer side:** `generate_update_path(..., secret_node = LCA, ...)` returns the path secret at the LCA's position on the filtered path. The LCA is always on it, because the joiner is a non-blank leaf in its copath; the code still fails closed if not.
- **Wire format:** `serialize_group_secrets` writes `optional<PathSecret>`.
- **Joiner side:** `mls_group_welcome_install_path_secret` (`mls_group.c`):
  - finds the lowest common ancestor with `GroupInfo.signer`;
  - derives along the committer's *filtered* path, as OpenMLS does, which is the behaviour the 8 passive-client Welcomes now assert;
  - compares every public key with `sodium_memcmp` before installing any, and on failure removes whatever it installed;
  - runs only after the full Welcome has verified (`mls_welcome.c`).
- **Parser:** presence byte must be 0 or 1; the secret must be exactly `Hash.length` bytes; trailing bytes are rejected; PSK ids are freed on the failure path.
- **Secrets:** GroupSecrets plaintext, joiner secret and path-secret copies are wiped on every path.

## Verification

- Build: `cmake -S . -B /tmp/w17mr -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w17mr` ok.
- `ctest --test-dir /tmp/w17mr -R 'marmot|mls|gnostr' -j6`: **80/80 passed**. Includes `marmot_test_commits`, `marmot_test_mls_welcome`, `marmot_test_interop`, `marmot_gobject_test` and `gnostr-test-mls-group-error`.
- ASAN+UBSAN (runtime confirmed): `test_commits`, `test_mls_welcome`, `test_mls_group` and `test_marmot_interop` all pass with no reports.
- `leaks --atExit`: `test_commits`, `test_mls_welcome` and `test_mls_group` each report **0 leaks**.

## Recommendation

**REQUEST CHANGES.**

**Must change before push**
- **B1:** sign kind:445 events with a fresh ephemeral key, and don't expose rename until publish-before-merge exists (or hide the row).
- **B2:** fix nostrc-wc6v, or fail closed for multi-entry producers and document it.
- **B3:** README disclosure of permanent-split modes and of the tie-break's spec basis; raise w1m0's priority.

**Should fix in the same pass:** N2 (a small load-time reconcile) and N5 (tests).

**Tracked; document:** N1 (w1m0). **Can follow:** N3 and N4.

The il4i commit alone is ready to approve.
