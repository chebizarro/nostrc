# W24b review: slice H, Commits in adopted groups (nostrc-qp24.5.1.3, nostrc-u9kv, nostrc-lbgu)

- **Reviewer:** independent peer reviewer (AGENTS.md "Peer Review")
- **Branch reviewed:** `marmot/w24b-adopted-commits` at `5799f390`, two commits on `7f73738f` (master)
- **Review branch:** `review/w24b-adopted-commits` (this document only)
- **Date:** 2026-10-01 (file named for the W24 review series)
- **Verdict:** **CHANGES-REQUIRED.**
  - **What holds.** Most of the slice is sound:
    - Non-admin authorization is exactly MDK's allowlist; a 2.5M-iteration property fuzz found no way around it.
    - AppDataUpdate application is byte-identical to the pinned OpenMLS on 7.4M differential inputs.
    - Partial writes roll back.
    - The secret-tree width fix (lbgu) is correct RFC 9420 tree math.
    - The live MDK 0.11 case is a real bidirectional run.
  - **M1 (required):** libmarmot follows admin Commits that MDK v0.11.0 and the lifecycle spec refuse. Examples: un-requiring 0x800c, or a redundant 0x800c update. Any admin can therefore fork every libmarmot member off the MDK members of a group. This contradicts the slice's own contract ("judged as MDK v0.11.0's cgka-engine judges a staged Commit ... fail closed on anything libmarmot cannot judge"). The fix is small.
  - **MC (required at rebase):** the merge condition from slice I's review (3eac257b M1).
  - Everything else is Low or Nit.

| Commit | Beads | Change |
|---|---|---|
| `54971451` | nostrc-qp24.5.1.3, nostrc-u9kv, nostrc-lbgu | libmarmot 0.12.0 (unreleased): adopted Commits judged and applied (0x8003 against the candidate parent, AppDataUpdate by value and by reference, resulting-epoch check), our adopted producers, routing rotation (`routing_changed`, `marmot_get_group_routing()`, aliases), created_at precheck (u9kv), canonical secret-tree width (lbgu, legacy too). `test_adopted_commits` (13 cases) on MDK v0.11.0 captures and pinned-OpenMLS forgeries |
| `5799f390` | nostrc-qp24.5.1.3 | Groundhog test hook `gh_mls_service_test_create_adopted_group_async()` (`GH_MLS_TEST_HOOKS` only), driver-0.11 `peer_new` `config: engine-default`, harness case `groundhog-mdk011-interop-adopted-commits` |

No code or beads were changed. Scratch work:

| Location | Contents |
|---|---|
| `/tmp/rv-h-scratch` | the probes (`probe_lifecycle.c`, `probe_removal.c`, `probe_rotation_rollback.c`), the fuzzers (`fuzz_adu.c`, `fuzz_authz.c`), `mutate.py`, build scripts |
| `/tmp/rv-h-mut` | detached at `5799f390`, with an ASAN+UBSAN build; mutations were applied one at a time and reverted |
| `/tmp/rv-h-merge` | the trial merge with slice I |
| `/tmp/rv-h-gate` | the clean checkout for the sanitizer gate |
| `/tmp/rv-h-mdk` | a private MDK v0.11.0 clone with the committed `commits-emitter`, built in its own target directory |

All of these but the first were removed afterwards.

Docker hygiene:
- The live run used a private driver tag (`nostrc-mdk-interop:rv-w24b-h`, removed afterwards), never the shared `:0.11.0`.
- The volume list before and after is identical (268 volumes). The gate reused its shared volumes.

## Summary by focus area

### 1. Authorization: complete for non-admins; one admin-side gap (M1)

`adopted_commit_authorize()` (`libmarmot/src/commits.c:488-587`) was checked against MDK v0.11.0 `cgka-engine` (`app_components.rs`) and the spec at marmot 07da8ffb.

**The non-admin allowlist is MDK's.** It matches `is_allowed_non_admin_commit` (`app_components.rs:1452-1479`):

| Shape | libmarmot | MDK |
|---|---|---|
| self-update | `proposal_count == 0 && has_path` | the same |
| SelfRemove-only | every proposal is a SelfRemove (at least one) | the same |
| the two combined | refused (XOR) | refused (XOR) |
| anything else, including a no-op inline AppDataUpdate | privileged | privileged |

Equivalence of the summary:
- **Proposal counts.** `proposal_count` counts inline and by-reference proposals, as `staged.queued_proposals()` does.
- **By-reference detection.** Inline proposals are deserialized with `sender_leaf = UINT32_MAX` (`mls_group.c:5307-5312`), so `commit_shape_fill()` (`:3763`) tells them apart from by-reference ones correctly.

**Each attack path asked about:**
- **Inline proposals from a non-admin.** Any proposal makes the Commit privileged, so it is refused.
- **By-reference proposals from an old epoch.**
  - The MLS proposal store admits only proposals of the parent epoch, with signature and membership tag verified (`mls_group.c:2459-2476`).
  - `stage_inbound()` loads only the parent epoch's kept proposals.
  - Every non-SelfRemove by-reference sender must be an admin of that epoch (`commits.c:561-573`; MDK `authorize_proposal`, `:416-507`).
  - The ingest refuses a non-admin's standalone AppDataUpdate (`MARMOT_ERR_ADMIN_ONLY`) and a lifecycle update sent standalone.
  - Tested, including the demoted-admin case (`test_openmls_by_reference`).
- **SelfRemove mixed with anything.** Not SelfRemove-only, so privileged.
- **SelfRemove senders** must be non-admins of the source epoch, as MDK's `reject_admin_self_remove_proposals` requires.
- **An UpdatePath altering other leaves.** MLS cannot do this. Every slot but the committer's must be `same_leaf` (signature, key, identity, extensions), and the committer keeps its identity.
- **An AppDataUpdate disguised inside an allowed Commit.** Any AppDataUpdate makes the Commit privileged. Update, GCE and PSK proposals fail closed in adopted groups, both at the MLS layer (`proposal_type_apply_supported()`) and in authorization (`update_count`/`gce_count`/`other_count`).

**Admin-side rules:**
- **Can an admin remove the last admin, or leave 0x8003 naming a non-member?** No. `mls_group_profile_check_entered()` (`mls_app_components.c:749-780`, `admins_are_members`) refuses both a non-member admin and an empty list. The admin-removal coupling holds in the resulting epoch, as in MDK `validate_admin_leaf_coupling_for_staged_commit` (`:594-674`).
- **Are new leaves' 0x8009 proofs bound to the right group and leaf?**
  - They are bound to the leaf, not to the group, by design: the v2 proof is a KeyPackage-level proof.
  - `marmot_leaf_proof_status()` (`kp_profile.c:455`) checks the same bindings as MDK `validate_proof_bindings`: credential identity, the leaf's own signature key, ciphersuite and scheme. A proof copied from another leaf fails on the signature key.
  - Every changed or added leaf needs a VALID proof (`commits.c:575-585`), and the committer's renewed leaf keeps its identity.
  - **What fails closed (documented, tracked in nostrc-tba2).** GCE (MDK uses it only in `upgrade_group_capabilities`, a public API whose only in-tree caller is MDK's conformance simulator), an admin's Update proposals, and admins' standalone Add/Remove proposals.

**Property fuzz.** `fuzz_authz.c` started from two real adopted states: the OpenMLS forgery group, and a libmarmot-created trio. It produced random post states (AppDataUpdates of real, malformed and non-member admin lists; blanked, copied and proof-flipped leaves; foreign committers) and random summaries. Results:
- 2.53M iterations, 395,616 accepted Commits, 127,406 of them with a non-admin committer.
- **0 violations** of these invariants:
  - a non-admin changed only its own leaf or the leaves of the SelfRemoves it commits, left the GroupContext unchanged, and got an ordinary key;
  - no admin SelfRemove and no non-admin by-reference sender was accepted;
  - every changed leaf carries a valid proof;
  - every accepted epoch passes the entered-epoch check.
- Run under ASAN+UBSAN on macOS and under LSan on Linux.

**The gap is on the admin side.** MDK's lifecycle transition rules (`validate_group_lifecycle_transition`, `app_components.rs:859-1019`) are not ported. See M1.

### 2. AppDataUpdate semantics: byte-identical to the pinned OpenMLS

`mls_app_data_update_apply()` (`mls_app_data_update.c:270`) was compared with OpenMLS `59e7d3b` (MDK's pin), using the same semantics in both directions:
- **Application** (`apply_app_data_update_proposals`, `apply_proposals.rs:241-300`): an update sets the entry and a removal deletes it.
- **Dictionary order:** entries ascending by id (a BTreeMap).
- **Extension order:** the dictionary is re-appended last via `Extensions::add_or_replace` (`extensions/mod.rs:517-525`: remove, then push). Other extensions keep their bytes and order.
- **Duplicates:** MDK refuses more than one operation per component id (`validate_app_data_update_batch`, `:1698-1746`).
- **GCE after an AppDataUpdate:** refused, as OpenMLS `IncorrectOrder` does.

**Differential.** `fuzz_adu.c` is an independent reference model against libmarmot:
- 7.4M structured inputs: random extension lists with 0–2 dictionaries, minimal and non-minimal varints, unsorted entries, bit flips and truncations, and 0–19 operations.
- Under ASAN+UBSAN, and LSan on Linux.
- **0 disagreements.** Both accepted the same 1,966,452 inputs, every one with byte-identical output.

**Removal of a component with no state** is the only semantic choice:
- libmarmot refuses it, following draft-ietf-mls-extensions §4.7.
- MDK and its OpenMLS pin accept it as a no-op. OpenMLS checks presence only when the Commit also carries a GCE (`validation.rs:761-781`).
- See L3.

**The resulting epoch** is checked twice: by the MLS layer before install, and by `adopted_commit_authorize()` again.
- `mls_group_profile_check_entered()` covers required components present and valid, a canonical dictionary, every admin a member, routing, every leaf capable, and the required-capabilities floor.
- The changed entries' bytes pass `adopted_component_valid()`. On this base it is the **only** validator of 0x8002, 0x8005 and 0x8007 (see L4 and MC).

**Partial failure leaves nothing behind:**
- **Rename:** the author's `test_partial_write_rollback` fails each of the 4 writes in turn.
- **Routing rotation** (7 writes, including alias, history and the carried floor): `probe_rotation_rollback.c` fails every one of them in turn, on macOS and under LSan. Each time the MLS state, record, address and epoch are unchanged; a retry applies, reports the rotation, leaves exactly one history entry, and the next MDK message decrypts.
- **Our own producers** stage on clones and install through `group_install_checked()`.

### 3. Routing rotation: old ids retained; collisions refused; hijack contained

- **What a Commit that changes 0x8004 does:**
  - it is reported with the address it left (`routing_changed`, `previous_nostr_group_id`);
  - the old id becomes an alias (`nostr_group_id_alias`) and is recorded in the history (at most 16, oldest dropped);
  - events at old addresses still route to the group (`messages.c:845-853`);
  - the created_at floor is carried to the new id (`marmot_carry_group_event_time`).
  
  This meets `nostr-routing-v1.md` "Routing rotation" on the library side.
- **On a competing Commit,** `routing_changed` is computed against the epoch being left (`commits.c:3090`), which is correct (see N5 for a cosmetic leftover).
- **The collision check** (`adopted_record_apply()`, `commits.c:1234-1288`) refuses an address that is another held group's current id **or alias** (`MARMOT_ERR_PROTOCOL_GROUP_MISMATCH`, tested).
- **Can an admin hijack another group's h tag?** An admin of group X can rotate X onto group Y's public h tag. What follows:
  - A member who holds Y refuses the rotation: X stops for it (L2), but Y is never misrouted.
  - Members who don't hold Y follow the rotation and publish undecryptable noise on Y's tag, which anyone can already do on a public relay.
- **The remaining vector is nostrc-scki** (a Welcome reusing an id, still accepted).
  - Since this slice adds aliases, the scki fix must check the alias table too. Otherwise a Welcome naming an old address of X steals X's backfill at that address.
  - The routing wiring in Groundhog is out of scope and filed as nostrc-ms4d. Until it lands, a rotated group goes silent in Groundhog. That is latent, since Groundhog offers no adopted groups.

### 4. lbgu: the width is RFC-correct; no deployed users affected

**The tree math.** `mls_tree_canonical_leaves()` (`mls_tree.c:57-66`):
1. trims trailing blank nodes;
2. pads to the smallest full tree;
3. returns its leaf count.

That is exactly the rule `mls_group_tree_hash()` already used to make the canonical tree (serialize-trim plus `mls_ratchet_tree_deserialize` padding), and it equals RFC 9420 truncation: 2^d leaves, d the smallest with 2^d > the rightmost non-blank leaf.
- Checked on 2.0M random trees (live widths 1–128, random occupancy and parents): 0 mismatches.
- Nodes beyond the canonical width are always blank. The Remove blanks the direct path, and the committer re-blanks its direct path, then sets only filtered nodes, whose blank right copath excludes everything above the canonical root.

**Memory safety.** The secret tree may now be narrower than the live tree. Every accessor bounds-checks `leaf_index >= st->n_leaves` (`mls_key_schedule.c:386,466,550,602,625,639`), so a sender beyond the canonical width (necessarily blank) is refused.

**Persistence.** A persisted state keeps the width its epoch started with, and its loader accepts up to the live width.

**The Welcome path.** The change in `mls_welcome.c` is a no-op: a joiner's tree comes from the canonical ratchet_tree extension. Mutation M21 confirms it is equivalent.

**Regression test.** `test_mdk_commit_sequence` (the message after Dave, the rightmost leaf, leaves) fails when the fix is reverted (mutation M8: `MARMOT_ERR_MLS`).

**Legacy impact.** libmarmot members with and without the fix read nothing of each other's while the live tree has a blank right edge. Because libmarmot never truncates its live tree, that lasts for every epoch until the edge refills, not just one (N4). But no deployed member is affected:
- libmarmot has never been released; there is no 0.11.0 tag, and `VERSION_MANIFEST.md` lists it as unreleased.
- The only libmarmot ever shipped is 0.1.0 in `gnostr-v0.1.0-preview` (2026-08-10), long since wire-incompatible.
- Groundhog's encrypted groups are a development-only flag (`GH_FEATURE_ENCRYPTED_GROUPS`, default OFF).

**No Groundhog migration or upgrade note is needed.** Unfixed members were already unable to read OpenMLS/MDK members in those epochs; that is the bug fixed.

### 5. Our own producers: checked, floored, coupled

- **Install path.** `mls_group_commit_adopted()`, `mls_group_commit_by_ref()` and both self-update producers install through `group_install_checked()`. Only the Commit processor uses `group_install_staged()`, after its own entered-epoch check (`mls_group.c:4797`).
- **The created_at floor (u9kv).** Every Commit producer runs `local_commit_precheck()` (the receivers' policy) before `marmot_next_group_event_time()`: `groups.c:1177,1476,1874,2054`. The remaining draws date a new group's id or non-Commit events.
  - Mutation M11 (precheck removed) is caught by `test_refused_commit_draws_no_time`, which reports `MARMOT_ERR_EVENT_RATE` after the drawn times.
- **Removing an admin drops its 0x8003 key** in the same Commit (`remove_members_adopted()`, `groups.c:1593-1644`). It is multi-device aware: `account_stays()` keeps the key while another of the account's leaves remains. It is tested in-process and live against MDK; mutation M12 is caught.
- **Other producers:**
  - Add accepts only proof-verified adopted KeyPackages that satisfy the group's leaf check, and then requires every resulting leaf to be proven.
  - Metadata requires every listed admin to be a member, sorts and deduplicates the list, and keeps the routing id on relay changes.
  - Self-update keeps the leaf's signature key, and its proof renewal replaces only the 0x8009 entry.
  - SelfRemove is refused to admins and requires every leaf's support.

### 6. Harness: a genuine bidirectional live run; the hook is test-only

I rebuilt the driver from this commit under a private tag and ran the matrix live (`mdk09-probe` excluded: it rebuilds a shared tag). Results:
- `control` **passed**.
- `groundhog-invites-mdk`, `mdk-invites-groundhog` and `adopted-welcome` were XFAIL-skipped as documented.
- `adopted-commits` **passed**. MDK reported rev `946e0547`, OpenMLS `59e7d3b`, profile `marmot-adopted`. The run:
  1. Groundhog created the group, and MDK joined it.
  2. kind 9 went both ways.
  3. MDK processed Groundhog's rename and admin change.
  4. Groundhog followed MDK's rename and self-update.
  5. kind 9 went both ways again.
  6. Groundhog removed MDK, an admin, and MDK recorded `removed`, with both sides at epoch 6.
- Only the Add that admitted MDK is tolerated as `TransportDeferred`.
- A missing driver is a skip (exit 77), never a silent pass.

**The test hook** is guarded by `#ifdef GH_MLS_TEST_HOOKS` in both the .c and .h files.
- The define is set only on three test targets, which compile the service sources themselves (`gnome/groundhog/CMakeLists.txt:1608,1671,1738`).
- The built `groundhog` binary has no `gh_mls_service_test_*` symbols (`nm`).

**Vector provenance.**
- I rebuilt the committed `commits-emitter` in my own MDK v0.11.0 clone. `Cargo.lock` gained only the emitter's own package, as its README claims.
- A **fresh capture** (new keys, new group) passed all 13 `test_adopted_commits` cases, so the tests are not tied to one capture.
- The emitter drives MDK's real engine APIs (`SendIntent::UpdateGroupData`, `SelfUpdate`, `Invite`, `UpdateAppComponents`, `Leave`, `RemoveMembers`).
- The committed fixture header regenerates byte-identically from the JSON.

### 7. Memory safety

No sanitizer finding in libmarmot:

| Run | Result |
|---|---|
| ASAN+UBSAN (macOS), all 24 `marmot_test_*` | pass |
| ASAN+UBSAN+LSan (`nostrc-linux-ci:arm64`), all 24 `marmot_test_*` plus every fuzzer and probe | clean, no leak |
| `scripts/linux-gate.sh --sanitizers` | 50 tests passed |

The only reports came from my own harnesses (a 2-byte read of a 1-byte buffer, and a `memcpy(NULL, 0)` in the reference model), and both were fixed.

The `add_members_staged()` error paths clear every proposal they allocated, including the AppDataUpdate copies.

## Findings

### M1 (Medium; required): lifecycle (0x800c) transition rules are not enforced, so libmarmot follows admin Commits MDK v0.11.0 and the spec refuse

- **Where:**
  - `libmarmot/src/commits.c:445-485`, `adopted_dictionary_changes_valid()`. An entry whose bytes did not change is skipped (`:459`), and a 0x0001 requirement-list change is checked only as a well-formed list (`adopted_component_valid()`, `:356-361`).
  - `:384-388`: only 0x800c's value is checked.
  - There is no port of MDK `validate_group_lifecycle_transition()` (`cgka-engine/src/app_components.rs:859-1019`), which MDK runs on every staged Commit through `validate_current_profile_invariants_for_staged_commit()` (`:814-829`; `message_processor/ingest.rs:1674-1683`).
- **Evidence:** `probe_lifecycle.c`. An admin's Commits were made with libmarmot's own `mls_group_commit_adopted()` and delivered to a libmarmot member, with the same result on macOS and Linux:

  | Probe | Admin's Commit | libmarmot | MDK v0.11.0 |
  |---|---|---|---|
  | P1 | AppDataUpdate of 0x0001 dropping 0x800c (its state kept) | **accepted** | refused ("lifecycle-v1 cannot be un-required") |
  | P2 | inline 0x800c = active while active | **accepted** | refused ("redundant lifecycle update") |
  | P3 | P1 plus a rename | **accepted** | refused |
  | P4 (control) | un-require 0x8001 | accepted | accepted |

  `group-lifecycle-v1.md`: "Once required, this component MUST remain present and required for the remainder of the group's lifetime."

  By reading, these are also accepted:
  - an enablement Commit that carries unrelated or by-reference proposals (MDK: inline only, only 0x0001/0x800c updates);
  - a lifecycle state added without being required.
  
  Today's admitted groups always require 0x800c, so those two need P1 first.
- **Failure scenario:**
  1. Admin A uses a modified or non-MDK client. MDK's own send path runs the same check and cannot produce these Commits.
  2. In a mixed group, A publishes P1 at epoch N. MDK members refuse it and stay at N; every libmarmot member applies it and enters N+1.
  3. A publishes a valid Commit C2 at N, and the MDK members follow it.
  4. To libmarmot, C2 competes with P1. Both are A's privileged Commits, so the digest decides, and A can grind P1 to win.
  5. libmarmot members stay on the fork for good. In Groundhog they hold every later MDK message as waiting (L2).

  So any admin can silently cut all libmarmot members of a group off from its MDK members. A buggy third-party client does the same by accident.
- **Fix:** port `validate_group_lifecycle_transition()` into `adopted_commit_authorize()`:
  1. Refuse a Commit whose resulting requirement list drops 0x800c once required.
  2. Refuse any 0x800c AppDataUpdate, same-bytes ones included, unless it is the enablement: not required → required, state `active`, every proposal inline, only 0x0001/0x800c updates. The disband stays `MARMOT_ERR_UNSUPPORTED`.
  3. Refuse a lifecycle state change while the requirement is unchanged.

  Because `adopted_dictionary_changes_valid()` skips unchanged entries, rule 2 needs the AppDataUpdate component ids of **inline** proposals in `MlsCommitSummary`; today `commit_shape_fill()` records them only for by-reference ones.

  Tests to add:
  - P1 and P2 as pinned-OpenMLS forgeries in `commits-emitter`;
  - an MDK `EnableDisbanding` capture as a positive, if a pre-lifecycle group can be made, or else as a unit test on `marmot_commit_authorize_ex()`.

### MC (merge condition; required when H rebases onto slice I, per 3eac257b M1)

Slice I now exports `mls_adopted_component_state_valid()` (`a562b94b`), and its header says H must call it. Status of the trial merge of `marmot/w24b-wn-components` (`b991e6c3`) into `5799f390`:

- **Conflicts** in `VERSION_MANIFEST.md`, `libmarmot/README.md`, `tests/interop/mdk/README.md`, `gnome/groundhog/CMakeLists.txt` (the case list) and `test_mdk011_interop.c`. All are additive: both slices add a case at the same place.
- **libmarmot tests:** 22/24 pass, with the two expected flips:
  - `test_adopted.c:2396` (merged tree): `marmot_can_self_remove()` now succeeds, because I's leaves advertise SelfRemove.
  - `test_adopted_commits` `unsupported_component`: I's entered-epoch parse refuses the malformed 0x800b first (`MARMOT_ERR_MLS_PROCESS_MESSAGE`).
- **Functional:**
  - Replace `adopted_component_valid()` with I's validator. Keep H's removal rules and the disband fail-closed. This also covers the untested 0x8002/0x8005/0x8007 path (L4).
  - Add positive 0x8006/0x800b update tests.
- **Groundhog:** I's `white-noise-welcome` asserts that Carol's rename is **not** followed, and treats any epoch advance as a hard XPASS error. With H, libmarmot follows that rename, so the case must flip to a real PASS. The brief requires this: "Any case that now passes must flip from XFAIL to a real PASS."

### L1 (Low): a Commit that removes our own leaf is judged on authority only

- **Where:**
  - `libmarmot/src/commits.c:2290-2337` (`removal_key()`);
  - `libmarmot/src/mls/mls_group.c:3793-3854` (`commit_removes_self_impl()`), which authenticates and parses but applies nothing: no resulting-epoch check, no dictionary check, no proof check.

  By contrast, MDK runs its full staged-Commit validation before it acts on any self-removal (`message_processor/ingest.rs:1602-1683`): authorization, admin coupling, component integrity, account proofs and profile invariants.
- **Evidence:** `probe_removal.c`. Alice, the sole admin, commits Remove(Carol) plus an AppDataUpdate setting 0x8002 to malformed bytes; the MLS-layer producer check passes on this base.
  - Bob refuses it (`MARMOT_ERR_EXTENSION_FORMAT`).
  - Carol gets a Commit result: removed by Alice, `final = 1`, group inactive, keys deleted.
- **Failure scenario:**
  - An admin can make one libmarmot member believe it was removed, and delete its keys, with a Commit every other member (MDK or libmarmot) refuses. The removal is final at once when no other admin could out-rank it.
  - The group keeps listing the victim and encrypting to it.
  - The admin could remove the victim openly, so this is a stealthy exclusion, not an escalation.
  - The behaviour pre-exists for legacy groups (the nostrc-xrya design); this slice extends it to adopted ones.
- **Fix:** before accepting a removal in an adopted group:
  1. Apply its proposals to a public copy of the parent. Removed members can do this: the UpdatePath's public keys are in the clear, and the GroupContext is produced by `mls_app_data_update_apply()`.
  2. Run the entered-epoch check, the dictionary check and the added leaves' proof checks.

  File a bead.

### L2 (Low, latent): Groundhog shows an adopted Commit libmarmot refuses for good as a silent stall

- **Where:** `gnome/groundhog/src/mls/gh-mls-service.c:1977`. Only `MARMOT_ERR_KEY_PACKAGE_IDENTITY` marks a group `change-refused`; every other refusal is `g_debug` (`:1999`). The next epoch's events then fail as `MARMOT_ERR_NIP44` and are held.
- **Failure scenario:** an admin's Commit that MDK members follow but libmarmot refuses. Groundhog then holds every later message as "waiting for the Commit that opens it", with no banner. Examples:
  - a disband (`UNSUPPORTED`);
  - an admin's Update or GCE (`UNSUPPORTED`, nostrc-tba2);
  - a rotation onto another held group's id (`PROTOCOL_GROUP_MISMATCH`);
  - a no-op removal (L3);
  - a role-less leaf (slice I's L1).

  Slice I's review L1 asked for change-refused when H lands. This is latent: Groundhog offers no adopted groups.
- **Fix:** have libmarmot report "an authenticated admin's Commit refused for good" distinctly (an error or a result flag), and map it to `set_refused()`. A non-admin's junk Commit, which every member refuses, must not mark the group. Do this before the adopted KeyPackage producer is enabled.

### L3 (Low): fail-closed asymmetries with MDK let an admin freeze libmarmot members

- **Where:**
  - `libmarmot/src/mls/mls_app_data_update.c:338-358`: a removal of absent state is refused.
  - `mls_app_data_update.h:48` with `process_commit_impl()`: a 17th AppDataUpdate is refused (`MLS_APP_DATA_UPDATE_MAX` = 16).

  MDK v0.11.0 accepts both:
  - `validate_app_component_remove_against()` (`app_components.rs:1748-1773`) has no presence check;
  - OpenMLS `59e7d3b` checks presence only alongside a GCE (`validation.rs:761-781`) and then removes nothing;
  - nothing bounds the count beyond one operation per id.
- **Failure scenario:** an admin's Commit removing an absent optional component, or carrying 17 operations on distinct unknown ids, is followed by MDK and refused by libmarmot. libmarmot members fall behind for good, and silently in Groundhog (L2).
  - libmarmot's removal rule is the draft's ("invalid if it specifies the removal of state ... that has no state present"), so this is spec-right but MDK-divergent.
  - Honest MDK never sends removals: `update_group_data.rs:220` builds only updates.
- **Fix:** either list both cases under "Commits libmarmot cannot judge as MDK does" in the README and raise the presence rule upstream, or accept an absent removal as a no-op as MDK does. Raise `MLS_APP_DATA_UPDATE_MAX`, or bind it to the dictionary bound with a comment.

### L4 (Low): test gaps found by mutation

- **0x8002, 0x8005 and 0x8007 value validation** on the Commit path is the only check on this base: the MLS layer validates them only once slice I lands. No test covers it. Mutations M6 (0x8002 unvalidated), M6b (0x8007) and M6c (0x8005 length) all **survived**.
- **Second-layer checks.** The admin-SelfRemove-sender check inside adopted authorization (M4) and the capture of by-reference senders in `commit_shape_fill()` (M14) survive because only the ingest refusals in front of them are tested.
- **Fix:**
  - Add pinned-OpenMLS forgeries: an admin's AppDataUpdate with malformed 0x8002, a non-URL 0x8007, and a 7-byte 0x8005.
  - Add a unit test on `marmot_commit_authorize_ex()` with an admin in `self_removed`.
  - Add a processor-level test that a by-reference AppDataUpdate's sender reaches the summary.

### N1 (Nit): wrong test count

`libmarmot/README.md:2035` lists `test_adopted_commits` with 11 tests. There are 13 `RUN()` cases.

### N2 (Nit): `validate_proposal_ordering()` comment over-claims

`libmarmot/src/mls/mls_group.c:2316-2319` says "none follows an AppDataUpdate". The code, like OpenMLS `IncorrectOrder`, refuses only a GroupContextExtensions proposal after one.

### N3 (Nit): misplaced doc comment

`libmarmot/src/mls/mls_tree.h:331-341`: the `mls_tree_canonical_leaves()` comment and declaration were inserted between `mls_tree_root_hash()`'s doc comment ("Compute the tree hash of the root") and its declaration. There is also a stray blank line at `:39`.

### N4 (Nit): lbgu compatibility wording

- `VERSION_MANIFEST.md:170` says 0.11.0 and 0.12.0 "both now match RFC 9420 and MDK". Only 0.12.0 does.
- `libmarmot/README.md:470-474` ("in such an epoch") understates the effect. The mismatch lasts for every epoch until the blank right edge refills, since the live tree never shrinks.
- Add that libmarmot 0.11.0 was never released, so no deployed member or Groundhog user is affected (§4).

### N5 (Nit): the routing history can list the current id

When a competing Commit undoes a rotation (the retained-parent path into `adopted_record_apply()`, `commits.c:1234-1288`), the abandoned id is remembered and the restored id stays in the history. `marmot_get_group_routing()` then returns the current id among `previous`. This is cosmetic. Skip remembering an id equal to the new current one, or drop it from the list.

## Verification

macOS 27, with `/tmp/nostrc-macos27-env.sh`, `cmake -G Ninja -DBUILD_GROUNDHOG=ON`:

| Check | Result |
|---|---|
| `ninja` (full tree) | OK; no warning in touched code |
| `ctest -R "marmot\|mdk\|mls"` | 34/34 passed |
| Full `ctest` (minus the MDK matrices) | 452/452 passed (5 skipped: environment-gated) |
| `python3 scripts/check-unsequenced-args.py` | clean |
| ASAN+UBSAN (macOS): all 24 `marmot_test_*` | pass |
| ASAN+UBSAN+LSan (`nostrc-linux-ci:arm64`): all 24 `marmot_test_*` plus fuzzers and probes | pass, no leak |
| `scripts/linux-gate.sh --sanitizers` (clean checkout of `5799f390`) | 50 tests passed |
| Live MDK 0.11 matrix, driver rebuilt from this commit under a private tag (`-DBUILD_MDK011_INTEROP=ON`) | `control` and `adopted-commits` pass; 3 XFAIL-skipped as documented |
| Fresh MDK v0.11.0 capture by the committed emitter (own clone and target dir) | all 13 `test_adopted_commits` cases pass on it |
| Differential fuzz: `mls_app_data_update_apply()` vs reference | 7.4M inputs, 0 disagreements (1,966,452 accepted, byte-identical) |
| `mls_tree_canonical_leaves()` vs RFC 9420 truncation | 2.0M trees, 0 mismatches |
| Authorization property fuzz | 2.53M iterations, 395,616 accepted (127,406 non-admin), 0 violations |
| Rollback probe: routing rotation, each of 7 writes failed | state unchanged each time; retry applies |
| Trial merge with slice I (`b991e6c3`) | 5 additive conflicts; libmarmot 22/24 (the two expected flips) |

**Mutation spot-checks.** Each was applied alone, rebuilt under ASAN, run against the four affected test executables, and reverted. 14 of 23 were caught:

| # | Mutation | Result |
|---|---|---|
| M1 | a non-admin's privileged Commit accepted | caught (`nonadmin_rename`; `test_adopted:1858`) |
| M2 | by-reference non-SelfRemove sender not checked | caught (`test_adopted_commits:848`) |
| M3 | new leaves' 0x8009 proofs not verified | caught (`add_bad_proof`) |
| M4 | admin SelfRemove sender check dropped (authorization) | survived: ingest refuses first (L4) |
| M5 | never-removable components removable (Marmot layer) | survived: the MLS entered-epoch check refuses first |
| M6, M6b, M6c | 0x8002, 0x8007, 0x8005 Commit-path validation removed | **survived** (L4) |
| M7 | 0x8006/0x800b/0x8008 changes accepted blind | caught (`unsupported_component`) |
| M8 | lbgu reverted | caught (`message_after_self_remove_commit`) |
| M9 | rotation onto another group's id allowed | caught |
| M10 | old address no longer routes | caught |
| M11 | u9kv precheck removed (adopted producers) | caught |
| M12 | our Remove of an admin keeps its key | caught |
| M13 | routing change not reported | caught |
| M14 | by-reference senders never recorded in the shape | survived: ingest refuses first (L4) |
| M15 | Update/PSK applied in adopted groups | survived: authorization refuses (`update_count`/`other_count`) |
| M16 | a non-admin's standalone AppDataUpdate kept | caught |
| M17 | removal of our leaf by a non-admin accepted | caught |
| M18 | required_capabilities change accepted | survived: unreachable (GCE refused) |
| M19 | removal of absent state allowed | caught (`test_app_data_update:320`) |
| M20 | lifecycle by reference allowed | caught |
| M21 | Welcome joiner's secret tree by live width | survived: equivalent (a joiner's tree is canonical) |

## Follow-ups to file (author)

- **M1:** the lifecycle transition port and its tests. Required before merge.
- **MC:** switch to slice I's validator, flip the two tests, flip `white-noise-welcome` to PASS, and resolve the five conflicts. Required at rebase.
- **L1:** full validation of a Commit that removes our leaf in adopted groups. Consider legacy as well (nostrc-xrya).
- **L2:** surface admin Commits that are refused for good as change-refused in Groundhog, before the adopted KeyPackage producer is enabled (with nostrc-tba2).
- **L3:** document the absent-removal and >16-update asymmetries, or align them with MDK; raise the presence rule with MDK upstream.
- **L4:** the missing forgeries and unit tests.
- **nostrc-scki:** the fix must check the routing alias table as well as current ids.
- **nostrc-ms4d:** stays the Groundhog routing wiring (resubscribe, old-address backfill, relay refresh).

---

## Addendum: re-review of the fixes (2026-10-01)

- **Branch re-reviewed:** `marmot/w24b-adopted-commits` at `d0700c8a`, rebased on master `64f765e3` (which includes slice I):
  - the reviewed commits, rebased: `056fc300` (was `54971451`) and `5bcb7434` (was `5799f390`);
  - the review fixes: `22282581` (libmarmot) and `d0700c8a` (Groundhog and the harness flip).
- **Review branch:** rebased onto `d0700c8a`; this addendum is appended.
- **Verdict: APPROVE-WITH-NITS.**
  - **Fixed:** M1, MC, L1–L4 and N1–N5 are all fixed and tested, and every repro from the first review now behaves as MDK v0.11.0 does.
  - **What remains (Low or Nit):**
    - R1: a legacy strict-mode removal is shown as "refused" rather than "removed";
    - R2: a pre-authorization proposal sort that is quadratic, bounded in Groundhog;
    - R3: two untested branches;
    - R4: change-refused copy and cursor;
    - R5: a public-result parent-hash check.
  - None of them blocks the merge.

### The first review's findings

| Finding | Status | Evidence |
|---|---|---|
| **M1** lifecycle rules | **Fixed** | See below |
| **MC** slice I validator | **Fixed** | See below |
| **L1** removal judged whole | **Fixed** (adopted and legacy) | See below; new Low **R1** and Nit **R5** |
| **L2** `MARMOT_ERR_COMMIT_REFUSED` | **Fixed** | See below; test gap **R3**, Nit **R4** |
| **L3** absent removal, update count | **Fixed as designed** | See below; the pre-authorization sort is **R2** |
| **L4** test gaps | **Fixed** | Pinned-OpenMLS forgeries for valid and malformed 0x8002/0x8005/0x8007, a direct test of the admin-SelfRemove sender, a direct by-reference-capture test. My earlier survivors now fail: R6a (old M4), R6b (old M14), R6c (0x8005 length) |
| **N1–N5** | **Fixed** | Count 16 (`README.md:2180`); the ordering comment; `mls_tree.h` placement; lbgu wording (0.11.0 never released, `VERSION_MANIFEST.md` no longer says "both now match"); the routing history drops an address the group returns to (`test_rotation_back_to_old_address`, mutation R7) |

**M1** (`adopted_lifecycle_transition()`, `commits.c`) is a case-by-case port of MDK `validate_group_lifecycle_transition`:
- **The rules ported:**
  - 0x800c cannot be un-required;
  - no redundant or by-reference lifecycle update;
  - the enablement is inline, alone and active;
  - otherwise the state never changes;
  - a disband stays `MARMOT_ERR_UNSUPPORTED`.
- **My repros, rerun on `d0700c8a`:**

  | Repro | Result |
  |---|---|
  | P1, P2, P3 | **refused**, as `MARMOT_ERR_COMMIT_REFUSED` (judged `MARMOT_ERR_VALIDATION`) |
  | P4 (control) | accepted, as MDK accepts it |
  | P5 (control) | refused |

- New pinned-OpenMLS forgeries cover the same cases and the enablement.
- Mutations R1a–R1e (each rule removed) are caught.
- The property fuzz now checks MDK's lifecycle invariants as well, on three bases including a lifecycle-less group: 3.74M iterations, 326,404 generated lifecycle changes, 1 accepted (a valid enablement), **0 violations**.

**MC:**
- `adopted_component_valid()` is now slice I's `mls_adopted_component_state_valid()` plus the disband refusal.
- Valid 0x800b and receive-only 0x8006 updates are followed (`ok_media_v2`, `ok_agent_stream`).
- The two test flips I predicted are handled.
- The five rebase conflicts are resolved: both harness cases are registered, and slice I's manifest rows are kept.
- The Marmot-layer copy of the check is now redundant (mutation R2 survives because the MLS-layer entered-epoch check, which uses the same validator, refuses first). That is expected.

**L1** (`removal_key()`): a Commit that removes our leaf now goes through `mls_group_commit_public_result_by_ref()` and the full `marmot_commit_authorize_ex()`. The public result is the processor up to the path-secret decryption, then the resulting-epoch check.
- **My probe** (an admin's Remove(Carol) plus an invalid AppDataUpdate): Carol now refuses it exactly as Bob does (`MARMOT_ERR_COMMIT_REFUSED`), and stays active and not removed.
- **Legacy, legitimate removals:** in default mode a legitimate removal of us is still recognized. The evidence:
  - `probe_legacy_strict.c`;
  - the live MDK 0.8 matrix: 11/11 subtests, including both cases where an MDK 0.8 admin removes a Groundhog member (`mdk-invites-groundhog`, `groundhog-invites-mdk-default`, `GH_MLS_GROUP_END_REMOVED` asserted);
  - all 13 nostrc-xrya removal tests in `test_commits` (learns it, losing and winning races, finality and key deletion, pending interplay, rival removals), plus the new forged-Add case, under ASAN on macOS and LSan on Linux.
- **The one legacy behaviour change** is strict proof mode (R1).
- Mutations: R3a (verdict ignored) and R3b (public result skips the entered-epoch check) are caught.

**L2:**
- **When libmarmot uses the new code.** `MARMOT_ERR_COMMIT_REFUSED` is returned only when all of these hold:
  - the group is adopted;
  - the Commit is of the current epoch;
  - it is authenticated (`mls_group_commit_authentic()`: signature and membership tag);
  - it comes from an admin of that epoch;
  - it is refused for its content.
- **Mutations:** R4a (no mapping) and R4b (a non-admin's junk mapped too) are caught.
- **A tampered admin Commit** keeps `MARMOT_ERR_MLS_PROCESS_MESSAGE` (`probe_refused_auth.c`); see R3 for the missing test.
- **Groundhog** maps the error to "change refused" with the new cause `UNFOLLOWABLE`:
  - it is stored as cause 2, which older builds ignore;
  - it is never retried on the proof preference, and is cleared by the next Commit;
  - its test (`mls-service/adopted-change-refused`) passes and fails when the mapping is removed.

**L3:**
- **What changed:**
  - a removal of absent state is a no-op, as in MDK and its OpenMLS pin;
  - the operations are sorted once (`qsort`, O(n log n)) and merged linearly;
  - duplicates are adjacent;
  - every operation array is heap-sized by the Commit.
- **The differential fuzzer**, with its reference model updated to the new semantics: 7.4M inputs, 0 disagreements, 4,492,786 accepted with byte-identical output, under ASAN+UBSAN and LSan.
- **Is the 65,536 bound a DoS vector?** No: nothing on the apply path is worse than O(n log n). The bound is one operation per u16 id. But the bound is checked after a pre-existing O(n²) step, which is R2.

### New findings (re-review)

#### R1 (Low): legacy, proofs required — a legitimate removal of us that also admits an unproven member is no longer recognized

- **Where:** `commits.c` `removal_key()` passes `m->config.allow_unproven_members` to `marmot_commit_authorize_ex()` for the public result.
- **Evidence:** `probe_legacy_strict.c`. Alice, an admin, commits Remove(Charlie) plus Add(Victor, no proof). Bob (default mode) and MDK 0.8 follow it.

  | Victor | Charlie's mode | Charlie's result |
  |---|---|---|
  | unproven | default | removed |
  | unproven | **proofs required** | **`MARMOT_ERR_KEY_PACKAGE_IDENTITY`**, stays active, not removed |
  | proven | either | removed |
  | forged proof | (everyone) | refused, as intended |

- **Failure scenario:** a Groundhog user with `only-join-verified-mls-groups` on, in a legacy group, is removed by an admin who also adds a member without a proof.
  - Before this slice they saw "removed" and their keys were deleted.
  - Now they see "change refused (unproven)" and keep a dead group's keys.
  - They are told the truth only if they turn the preference off, which retries the Commit.
- **Severity:** no security impact: the group stops for them either way. The preference is off by default.
- **Fix:** when the Commit removes our leaf, judge it with `allow_unproven = true`. An *absent* proof then never blocks our removal; an *invalid* one still does, so L1's stealth-exclusion protection is kept. Add the probe's strict case as a test.

#### R2 (Low; hardening): a member's Commit can make every receiver spend O(updates × adds) moves of 512-byte structs before authorization

- **Where:**
  - `mls_group.c` `sort_proposals_for_application()`, an insertion sort of `MlsProposal` (512 bytes each). It runs in `process_commit_impl()` before any operation-count check and before Marmot authorization.
  - Nothing bounds a Commit's proposal count before it:
    - the departures check limits Removes to distinct occupied leaves;
    - Adds are only parsed structurally until the apply loop;
    - an AppDataUpdate is 7 bytes on the wire and sorts after both.
  - `marmot_process_message()` has no event-size cap (it parses with `nostr_event_deserialize_compact`, which applies none).
- **Evidence:** `probe_adu_dos.c` (release build, `-O2`). Bob, a non-admin, frames, signs and tags a Commit with K AppDataUpdates first, then A Adds; the confirmation tag is junk, since it is checked only after the sort. Alice's processing time before refusal:

  | Event size | K | A | Sorted order | Adversarial order |
  |---|---|---|---|---|
  | 242 KB | 13,000 | 200 | 0.015 s | 0.043 s |
  | 879 KB | 30,000 | 1,000 | 0.050 s | 0.379 s |
  | 1.76 MB | 60,000 | 2,000 | 0.138 s | **1.459 s** |

  The cost grows with the square of the event size. Groundhog processes each kind:445 synchronously in the MLS service's relay callback (`on_group_update` → `process_event`), on the context its scopes run on.
- **Bound in practice:**
  - Groundhog's relay path (libnostr `nostr_event_deserialize_signed`) drops events over 256 KB (`NOSTR_MAX_EVENT_SIZE_BYTES`), which caps the cost at about 43 ms per event.
  - An embedder calling `marmot_process_message()` with larger events has no such cap.
- **Since when:** the original slice H. AppDataUpdates reached the sort before the old 16-update cap too.
- **Fix:**
  - replace the insertion sort with a stable O(n) bucket pass over the five application-order classes, or sort an index array;
  - optionally refuse oversized events or proposal counts in `marmot_process_message()` before parsing.

#### R3 (Nit; test gaps found by mutation)

- **R4c survives:** dropping the authenticity requirement from `refused_for_good()`. The code is right (my probe shows a tampered admin Commit is not mapped), but no test pins it. Without that check, anyone holding the exporter secret could raise every member's "change refused" banner with a forged admin-sender Commit.
- **R1f survives:** counting by-reference AppDataUpdates as inline for the enablement in `commit_shape_fill()`. The by-reference enablement case is tested only with a hand-built summary.
- **Fix:** add a forgery each, one with a tampered tag and one an enablement referencing its 0x0001 update.

#### R4 (Nit): "change refused" for an admin Commit everyone refuses

- An admin's Commit that every member refuses (malformed, say) is also `MARMOT_ERR_COMMIT_REFUSED`: libmarmot cannot tell it apart.
- Groundhog's copy then says "New messages here can't be read until the group moves past that change". In that case the group never moved, and same-epoch messages stay readable.
- The read cursor stays held at that Commit until a later Commit (`gh-mls-service.c:1717-1719`), so each restart refetches from there.
- **Fix:** soften the copy ("may not be readable"). Consider clearing the state once a later message of the same epoch is read.

#### R5 (Nit): the public result skips the UpdatePath parent-hash check

- `mls_group_commit_public_result_by_ref()` returns before `mls_treekem_apply_update_path()` (`mls_group.c:4659`).
- That function checks the committer's parent-hash chain (RFC 9420 §7.9.2) on public data, and every non-removed member runs it.
- So an admin's removal with a bad parent hash ends the group for the removed member only.
- **Fix:** run it on the public result; it is cheap.
- **What no fix can close:** a removed member can never check the confirmation tag (it lacks the new epoch's secrets; MDK has the same limit). Say in the README that a determined admin can still make a removed member alone see its removal.

### Re-review verification

macOS 27, `/tmp/nostrc-macos27-env.sh`, `cmake -G Ninja -DBUILD_GROUNDHOG=ON`, on `d0700c8a`:

| Check | Result |
|---|---|
| `ninja` (full tree) | OK, no warning in touched code |
| Full `ctest` (minus the MDK matrices) | 452/452 passed (5 skipped: environment-gated) |
| `python3 scripts/check-unsequenced-args.py` | clean |
| ASAN+UBSAN (macOS): all 24 `marmot_test_*` | pass |
| ASAN+UBSAN+LSan (`nostrc-linux-ci:arm64`): all 24 `marmot_test_*`, both fuzzers, all five probes | pass, 0 sanitizer reports |
| `scripts/linux-gate.sh --sanitizers` (clean checkout of `d0700c8a`) | 50 tests passed |
| Live MDK 0.11 matrix (private tag built from `d0700c8a`) | `control`, **`white-noise-welcome`** and `adopted-commits` pass; 3 XFAIL-skipped as documented |
| Live MDK 0.8 matrix (private tag) | 11/11, including both MDK-admin removals of a Groundhog member |
| Differential fuzz `mls_app_data_update_apply()` vs reference (new semantics) | 7.4M inputs, 0 disagreements (4,492,786 accepted, byte-identical); canonical width 2.0M trees, 0 mismatches |
| Authorization property fuzz (lifecycle invariants added; three bases) | 3.74M iterations (527,604 accepted, 157,768 by non-admins; 326,404 lifecycle changes generated, 1 accepted), 0 violations |
| Repro probes | P1–P3 refused, P4 accepted; removal judged whole; rotation rollback holds at all 7 writes; R1, R2, R4c probes as above |

**`white-noise-welcome` is a genuine live PASS:**
1. MDK 0.11 (`946e0547`, OpenMLS `59e7d3b`) parses libmarmot's adopted KeyPackage (Current profile, `0xf2d1`, `0x000a`, 0x8006/0x800b).
2. Carol creates a White Noise group with components 0x0001, 0x8001, 0x8003, 0x8004, 0x8006, 0x800b and 0x800c.
3. Groundhog joins, and kind 9 goes both ways.
4. Groundhog follows Carol's rename: `wait_name`, `assert_gh_converged`, her next message read, not change-refused.
5. Groundhog renames the group, and MDK goes from epoch 2 to 3 with previous name "Renamed by White Noise".
6. kind 9 goes both ways after.

**Revert spot-checks of the fixes.** Each mutation was applied alone, rebuilt under ASAN, and reverted; the Groundhog one was rebuilt in the review tree and reverted. 16 of 19 were caught:

| # | Mutation | Result |
|---|---|---|
| R1a–R1e | each M1 rule removed, or the check not called | caught (`test_adopted_commits:716`, `:901`) |
| R1f | by-reference enablement AppDataUpdate counted as inline | survived (R3) |
| R2 | Marmot-layer component check accepts anything | survived: redundant (MLS layer refuses first) |
| R3a | removal ignores the whole-Commit verdict | caught (`test_adopted_commits`, `test_adopted:2324`, `test_commits:308`) |
| R3b | public result skips the entered-epoch check | caught (`remove_observer_malformed`) |
| R4a | no `COMMIT_REFUSED` mapping | caught |
| R4b | a non-admin's junk mapped too | caught |
| R4c | authenticity not required for the mapping | survived (R3) |
| R5a | duplicate component ids allowed | caught (`test_app_data_update:327`) |
| R5b | operations not sorted | caught |
| R6a | admin SelfRemove sender unchecked | caught (`test_adopted_commits:1193`) |
| R6b | by-reference senders never recorded | caught (`:1076`) |
| R6c | 0x8005 length unchecked | caught (`retention_7_bytes`) |
| R7 | returned-to address stays in the history | caught (`:1669`) |
| G1 | Groundhog: `COMMIT_REFUSED` not mapped to change-refused | caught (`mls-service/adopted-change-refused`) |

### Re-review scratch and cleanup

- **Scratch:** `/tmp/rv-h2-mut` (detached at `d0700c8a`, ASAN build and mutations) and `/tmp/rv-h2-gate` (clean checkout for the gate). Both are removed.
- **Kept:** probes and fuzzers in `/tmp/rv-h-scratch`: `probe_legacy_strict.c`, `probe_adu_dos.c`, `probe_refused_auth.c`, `probe_removal2.c`, `fuzz_adu2.c`, `fuzz_authz2.c`.
- **Docker:** the private tags (`nostrc-mdk-interop:rv-w24b-h2`, `:rv-w24b-h-08`) were removed. The volume list before and after is identical.

### Follow-ups (author)

- **R1:** judge a Commit that removes us with absent proofs allowed, keeping invalid proofs refused, and test strict mode.
- **R2:** a stable O(n) proposal-order pass, and optionally an event-size or proposal-count cap in `marmot_process_message()`.
- **R3:** the two missing tests (tampered-tag forgery; by-reference enablement).
- **R4:** the change-refused copy and the held cursor for an everyone-refused admin Commit.
- **R5:** the parent-hash check in the public result, and a README note on the confirmation-tag limit.
