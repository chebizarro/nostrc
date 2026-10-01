# W25 slice N review: Marmot convergence (nostrc-w1m0)

- **Branch:** `marmot/w25-convergence`, tip `f2d0cf2c` on master `08d35b4e`
  (commits 8627ef96, 590c74ff, 38a0f24e, 5afe4152, f2d0cf2c)
- **Reviewer:** Claude (independent; worktree `/tmp/rv-w25-convergence`, read-only)
- **References:**
  - marmot-protocol/marmot `07da8ffb` (`protocol-core/convergence.md`,
    `retained-history.md`, `inbound-processing.md`, `transports/nostr.md`,
    `foundation/errors.md`)
  - MDK v0.11.0 `946e0547`, cgka-engine (`convergence.rs`,
    `canonicalization.rs`, `openmls_projection.rs`)

## Verdict: CHANGES-REQUIRED

The selection engine is correct. It matches MDK 0.11 on every delivery order of
the captured fork vectors, when held events are retried properly. Five epochs
of retention is exactly what the spec mandates, and it is wiped correctly.

Two High findings block the merge:

- **H1 (a regression):** the new candidate bounds refuse honest Commits that
  advance the canonical tip. The member then forks for good, and Groundhog
  drops the refused event.
- **H2:** libmarmot gives the application no usable signal that its decryption
  context changed when a Commit is retained. Under Groundhog's retry policy,
  40 of 72 delivery orders of MDK's own fork vectors end on a different branch
  from MDK.

H2 is tracked as nostrc-xrza (P1). The libmarmot half of it, the API signal,
belongs to this slice.

## What I ran

| Check | Result |
| --- | --- |
| Build (`cmake -G Ninja -DBUILD_GROUNDHOG=ON`, `ninja`) | OK |
| `ctest -R "marmot\|gnostr-test-mls\|groundhog-store-marmot\|groundhog-mls\|groundhog-privacy-mls"` | 37/37 pass |
| macOS ASan+UBSan: `test_commits`, `test_adopted_commits`, `test_protocol`, `test_storage` | All pass, no reports |
| `scripts/linux-gate.sh --sanitizers` | Pass, 52 tests |
| `python3 scripts/check-unsequenced-args.py` | Clean |
| MDK 0.11 matrix (own image `nostrc-mdk-interop:0.11.0-rv-w25n`, built from this tree's `driver-0.11`): control, white-noise-welcome, adopted-commits, **concurrent-commits** | Pass. Three cases report Skipped by design (they assert documented refusals; unchanged by this branch). |
| `concurrent-commits` with the witness-score rule mutated out of `conv_branch_cmp` | **Fails** in round 2: Groundhog keeps "MDK's second" while MDK converges on "Groundhog's witnessed". The live case depends on the new rules. |
| Probe: MDK fork-vector delivery-order fuzzer (all 2×6×6 = 72 orders) | See "Selection equivalence" and H2 |
| Probes: linear Commit under the caps | See H1 |
| Probe: per-message cost | See M1 |
| Docker volumes | None created (the shared `nostrc-linux-gate-*` volumes pre-existed) |

The probes are not committed. Their diffs are kept outside the tree as
`/tmp/rv-w25-conv-probes.diff` and `/tmp/rv-w25-conv-probe-cost.diff`.

### Mutation spot-checks

Each mutation was applied, rebuilt and tested, then reverted.

| Mutation | Result | Caught by |
| --- | --- | --- |
| Privileged order inverted | Caught | `test_three_concurrent_commits`, `test_same_epoch_race_converges` |
| Witness boost removed | Caught | `test_witness_quorum_beats_longer_branch` |
| Witness score removed | Caught | `test_same_epoch_witness_decides`, `test_mdk_forks_converge` |
| Per-committer cap removed | Caught | `test_branch_flood_is_bounded` |
| Unconfirmed own candidate made selectable | Caught | `test_commits` |
| Demotion of superseded Commits removed | Caught | both suites |
| Invalidation removed | Caught | `test_depth2_branch_child_first` |
| Witness app-payload window removed (`conv_score`) | **Survived** | no test (L2) |
| Deleting exporter secrets above a lowered tip removed (`conv_install`) | **Survived** | no test (L2) |
| Witnesses at the root epoch counted | Survived | none needed: they apply equally to every branch |

## Selection equivalence with MDK v0.11.0 and the spec

Re-derived from the sources:

- **Branch set.** MDK (`historical_replay_start_epoch` and the BFS in
  `build_stored_openmls_candidate_paths`) rewinds to
  `replay_start = min(source_epoch of unresolved Commits)`. Its candidates are
  the *maximal* BFS paths only, and every path's `fork_epoch` is that replay
  start (`materialize_openmls_candidate_paths_budgeted` takes the min source
  epoch per path). libmarmot's `conv_build` and `conv_select` (commits.c:3171,
  3252) do the same: one root, leaves only, `fork_epoch = root_epoch`.
- **Comparator.** `conv_branch_cmp` (convergence.c:384) equals MDK
  `compare_scores` and spec "Branch selection" steps 1–6. Effective depth
  (raw + 1 with quorum), then quorum, then score, then privileged before
  ordinary, then lower committer, then lower digest. `privileged` comes from
  the Commit's shape, not the committer's role (commits.c:571, 685), as MDK's
  `CommitOrderingPriority` does.
- **Witnesses.** Counted at epochs strictly after the fork, inside
  `tip − e ≤ 5` per candidate tip, by distinct account per (epoch, state),
  capped at 2. This matches MDK `attach_app_witnesses`, `app_witness_score`
  and `witness_quorum_met`. Several devices of one account share one identity
  (`marmot_mls_sender_identity`), so they count once. A witness is counted only
  after `check_inner_author` (messages.c:1188, 1452), so decryption alone is
  never a witness.
- **Eligibility and expiry.** `source + 5 < tip` is stale (`conv_prune_stale`,
  `conv_admit`), matching MDK `is_branch_eligible` and spec "Eligibility".
- **Fuzzing.** All 72 delivery orders of the three captured MDK forks (race,
  depth2, witnessed) were run, with held events retried after every delivery.
  **72/72** reach MDK's verdict and read MDK's converged-epoch message, and
  re-delivering every event afterwards moves nothing.
- **Reorg authorization.** On every pass, each candidate Commit is re-staged
  through `stage_inbound` (commits.c:2202). That runs full MLS processing plus
  `marmot_commit_authorize_ex`: H's rules, A's slot and leaf binding, and I's
  component validation. Own candidates use the kept post-state and priority.
  Canonical nodes were validated when applied.

Remaining differences from MDK are in H1 (bounds), L1 (parent identification)
and the note on withdrawn witnesses, plus the documented absence of pass timers.

## Findings

### H1 (High): the candidate bounds refuse honest Commits that advance the tip, forking the member for good

**Where:**
- `libmarmot/src/commits.c:4515`, `:4506`: any Commit while candidates are
  retained becomes `linear = false`.
- `commits.c:3985`: the 32-candidate cap is checked before staging.
- `commits.c:4013-4018`: the per-committer cap counts the new Commit itself,
  whether or not it would be selected.
- Groundhog `gnome/groundhog/src/mls/gh-mls-service.c:2046-2047`:
  `MARMOT_ERR_RESOURCE_REFUSED` falls into "skipped". It is not held, and later
  accepted events move the cursor past it.

**Mechanism.** Once a group has retained candidates, every inbound Commit goes
through `conv_admit`, linear advances included. The caps are applied to it
before it is known to be the selected branch.

**Probe P1 (confirmed).** Trio; Alice renames, and Bob applies it. Charlie's
4 self-updates from the old epoch lose at Bob and are retained. Charlie applies
Alice's rename, then makes an ordinary, honest self-update **on the tip**.
- Bob returns `-54 MARMOT_ERR_RESOURCE_REFUSED`.
- Alice, who has no candidates, applies the same Commit: `MARMOT_OK`.
- Bob is now one epoch behind. Everything after it is from an epoch he lacks.
  Charlie's further Commits keep being refused, and the losers never age out,
  because the tip cannot move.

**Probe P2 (confirmed).** With 32 retained candidates, an honest linear Commit
is refused with `-54` as well. The candidates can come from 8 colluding
accounts at 4 each (e.g. sock puppets in a large group), or from honest
churn: one epoch with more than 32 concurrent committers.

**Effect.** All libmarmot members of the group are frozen, while MDK members
continue.

This is a regression: master applied every valid linear Commit. It also breaks
the spec:
- `foundation/errors.md` defines `resource_refused` as a *pre-convergence*
  category for unclassified transport objects. Here it is applied to a
  validated, selectable candidate edge.
- `transports/nostr.md` (resource-refused events): the client MUST give the
  event another delivery opportunity. Groundhog does not.

**Fix.**
1. Admit first, then bound. Never refuse a Commit whose branch would be
   selected. A linear advance at the tip always attaches and wins its depth.
2. Apply the bounds only to *losing* candidates. When over the bound, evict
   the lowest-ranked retained loser (its `could_win` is lowest) rather than the
   newcomer.
3. Count per-committer only among losers.
4. In Groundhog, hold `RESOURCE_REFUSED` like `NIP44` and pin the cursor.
5. Add tests P1 and P2 (an honest linear Commit applies with a full store and
   with a committer at its cap).

### H2 (High): no retry signal when a Commit is retained, so a Groundhog member ends on a different branch from MDK in 40/72 orders

**Where:**
- `libmarmot/src/commits.c:4026-4028`: a newly retained candidate returns
  `MARMOT_ERR_WRONG_EPOCH`, the same code as stale or duplicate input.
- `libmarmot/src/messages.c:1419-1463` and `commits.h`: the doc tells the app
  to offer `NIP44` events again "after the next Commit".
- `gnome/groundhog/src/mls/gh-mls-service.c:1836` (`retry_held`): runs only
  after an applied Commit or `branch_recovered`.

**Mechanism.** Retaining a candidate adds its state's exporter secret, which
changes the transport decryption context. `inbound-processing.md` and
`nostr.md` require retrying held events then ("A retained object MUST be
retried whenever the transport decryption context changes"). The app cannot
tell this outcome apart from a stale Commit.

**Fuzz evidence.** Same 72 orders, retrying only after an applied Commit or
branch change (Groundhog's policy). **40/72 diverge from MDK's verdict.**
- depth2: `rename, second, first`. `second` is held; `first` is retained with
  `WRONG_EPOCH`; no retry, so Carol's deeper branch never wins here.
- witnessed: `witness_message` before `witnessed_high`. The witness is held; the
  high Commit is retained; the witness is never counted.

**Effect.** Relays serve backfill newest-first, so a child or witness arriving
before its retained parent is the *common* case, not an edge case. nostrc-xrza
(P1) covers the Groundhog side. The libmarmot side is in this slice and is not
filed.

**Fix.** Distinguish "retained as a candidate (context changed)" from stale.
For example, return `MARMOT_OK` with a result type or flag
(`result.convergence.candidates_changed`), or a dedicated error code. Document
it, retry held events on it in Groundhog, and add the fuzzer above as a test.

### M1 (Medium): held candidate-branch messages are an amplification vector

**Where:** `commits.c:4163` (`marmot_commit_branch_decrypt` runs a full
`conv_build`, re-staging every candidate, for each message);
`messages.c:1424`.

**Mechanism.** A message sealed under a candidate state's exporter whose state
is below quorum (`wants`) costs a history decode plus re-staging of every
retained candidate. It is then returned as `NIP44`, kept by the app, and
retried after every Commit.

**Measured** (3-member group, 4 candidates):

| Case | Cost per message |
| --- | --- |
| Normal message | ~0.1 ms |
| Held branch message | 2.2 ms |
| Same message, re-offered | 6 ms |

**Scenario.** One member, alone, makes one losing self-update and sends
messages on it. Being a single sender, they never reach quorum, so the
expensive path never short-circuits. That keeps Groundhog's 256-slot held queue
full: about 1.5 s per retry pass in a trio. The cost grows with the candidate
count (up to 32 re-stagings, each with an UpdatePath HPKE decap) and with group
size.

**Fix.** Cache the rebuilt candidate states per pass or per record version.
Skip `branch_decrypt` when the sender already witnesses that (epoch, tag): the
sender is in the cleartext-authenticated sender data after the first
decryption. Or bound held branch messages per sender.

nostrc-7e0k covers only the record decode. Junk kind:445 from non-members
measured 0.06 ms, which is fine.

### M2 (Medium): a reorg loads the group's whole message history into memory

**Where:** `commits.c:3570-3619` (`invalidate_messages`).

**Mechanism.** It pages through *all* stored messages of the group (no epoch
filter) and keeps every page alive until the reorg ends (for the undo
pointers), including messages it does not touch.

**Scenario.** Any member can force a reorg cheaply: a 2-Commit self-update
branch beats a depth-1 branch. With 10^5 messages, each reorg holds every
message's JSON in memory at once.

**Fix.** Query by epoch range, or free pages whose messages are not modified,
keeping only the undo records.

### M3 (Medium, tracked nostrc-xrza): withdrawn messages and superseded changes stay visible in Groundhog

**Where:** `gh-mls-service.c:2049-2053` (only a refresh on `branch_recovered`).

**Mechanism.** libmarmot marks the losing branch's messages
`EPOCH_INVALIDATED` and lists them in `MarmotMessageResult.convergence`.
Groundhog ignores both lists. Its conversation store keeps showing those
messages as delivered, and the superseded renames or admin changes as done.

**What a user sees.** On reorg, messages do *not* vanish; they stay, without
explanation. Other members never saw them. This violates convergence.md
("a group-state change that lost branch selection MUST NOT remain visible to
the application as a completed change").

**Withdrawal is final in both implementations.** After a flip-back (A→B→A),
withdrawn messages are not re-delivered: processed markers stay
(nostrc-su0v). MDK also withdraws already-delivered messages for good (#965),
so the two agree on that.

**Recommendation.** When xrza lands, render withdrawals explicitly ("message
withdrawn: the group's history was rewritten by a competing change") rather
than deleting rows silently.

### L1 (Low): the parent is identified by the transport exporter, not by MLS authentication

**Where:**
- `commits.c:3209-3213`: the parent is matched by `parent_tag`, the state whose
  exporter peeled the kind:445.
- `commits.c:3109-3112`, `:3122-3142`: a failure there is terminal DEAD.

**Spec.** convergence.md: the parent is "the retained group state ... against
which the Commit's parent-dependent MLS authentication succeeds; transport
metadata never identifies it". Failure against a non-matching state "cannot
reject the Commit". MDK's BFS probes a Commit against every path at its source
epoch.

**Scenario.** A member re-seals another member's valid Commit under a
different same-epoch state's exporter. libmarmot refuses that copy; MDK
accepts it.

**Impact found:** none lasting. The refused copy is not retained or marked,
and the honestly sealed original is then processed normally. So the risk is a
transient difference, plus a refusal error surfaced to the app.

**Fix.** On an MLS-authentication failure, try the other same-epoch nodes
before declaring DEAD.

### L2 (Low): test gaps (mutations survived)

- Nothing pins the witness app-payload window: the `tip − e > 5` check in
  `conv_score` (commits.c:3239).
- Nothing pins the deletion of exporter secrets above a lowered tip
  (commits.c:3733-3739). `test_witness_quorum_beats_longer_branch` lowers
  Charlie's tip but does not check that the E+2 exporter is gone.
- Neither probe P1 nor P2 exists as a test (see H1).

### L3 (Low, spec-level, informational): a single member can rewrite recent history or evade a removal

Under the spec's longest-branch rule, which MDK shares, any member can:
- publish a deeper chain of ordinary self-updates from up to 5 epochs back,
  withdrawing every other member's messages in those epochs;
- defeat a depth-1 admin removal with two self-updates.

libmarmot's per-committer cap (4) incidentally limits one account to rewriting
about 3 epochs. A self-witness (score 1) also beats an unwitnessed privileged
Commit in a same-epoch race (seen while measuring M1).

None of this is a libmarmot defect. It is worth raising upstream (removal
finality, which the bead lists as remaining item 6) and stating in Groundhog's
threat model.

### Nits

- `CONV_MAX_ORPHANS` (convergence.h:45) is defined but unused: orphans are
  never retained. Remove it, or document that orphans are transport-deferred.
- **Witnesses of withdrawn messages.** libmarmot keeps the witness records of
  a superseded branch (`conv_retained` keeps witnesses of tree nodes). MDK
  stops re-admitting `EpochInvalidated` messages as witnesses on later passes.
  So, after a reorg, a later contest of the old branch can score differently.
  The spec arguably favors libmarmot, since a witness is retained
  authenticated input. Not exercised by any vector; document the difference.

## Forward secrecy

**Is 5 epochs justified?** Yes; it is the minimum the spec allows.
- `retained-history.md` ("Retained anchor") requires a MUST-retain state for
  "each epoch inside `max_rewind_commits` from the current tip".
- Its material table needs, for that whole horizon, the membership key and
  sender-data secret (to authenticate PublicMessage Commits) and the private
  path and init material (to process them). The app-payload window is the
  same 5 epochs.
- MDK keeps full OpenMLS snapshots per retained anchor
  (`retain_current_group_epoch_snapshot`, `prune_retained_anchor_snapshots`)
  for the same horizon.
- nostrc-yuj2's early reduction is unsafe under the depth rules: any member
  can extend a competing branch later. Removing it is right, and the tradeoff
  is documented (commits.c:735-748, README).
- No smaller retention is conformant. The only lever is
  `max_rewind_commits`, a protocol constant.

**Are retired secrets really wiped?** Yes.
- **In memory:** `conv_entry_clear` → `mls_group_free` zeroizes the epoch
  secrets, own keys and path keys; the secret tree is zeroized; candidates'
  `own_post` is wiped; the `ConvHistory` (branch exporters) gets
  `sodium_memzero`.
- **In storage:** rows of exporter secrets that leave the window are deleted
  (`marmot_commit_persist_ex`, `conv_install`). The retained record is
  rewritten without the retired entries.
- **On disk (SQLCipher):** Groundhog's store sets `cipher_memory_security=ON`,
  and SQLCipher 4.17's default `secure_delete=1` zeroes freed pages. Only
  WAL-frame residue persists until checkpoint; consider a periodic
  `wal_checkpoint(TRUNCATE)`.

**Replay through rebuilt states.** Rebuilding a branch's states from Commit
bytes regenerates fresh secret trees (A→B→A). AEAD nonce reuse is prevented
by the random `reuse_guard` (mls_group.c:5025), and old envelopes are
duplicates by inner id.

## Resource bounds

- The 32/4 caps bound memory: the record is about 15.7 KB with 5 entries in a
  trio; `CONV_MAX_WITNESSES` = 76; branch secrets ≤ 32. But the caps are
  applied to the wrong inputs (H1).
- Per-message decode cost: about 0.1 ms in a trio, the same as without
  history. It grows linearly with group size (5 full states per decode); the
  measurement is in M1, and it is fine to track it in nostrc-7e0k.
- Storage write amplification: every late message and every new witness
  rewrites the whole record (5 states). Same remedy as nostrc-7e0k.

## Harness case `concurrent-commits` (MDK 0.11)

It is a genuine live race. A real MDK 0.11 process and Groundhog each commit
from the same epoch without the other's Commit; round 3 uses relay withholding.
Both sides switch at least once. Mutating the witness score makes it fail, so
it exercises the new rules.

It uses deterministic, favorable orders. It does not cover:
- a depth-2 branch or two-sender quorum against live MDK (covered only by the
  captured vectors);
- the "parent retained after its child" orders that fail in H2.

## Version and manifest

libmarmot 0.12.0 (unreleased), Groundhog 0.12.0 and marmot-gobject 1.5.0 have
manifest rows. CMake and meson versions agree, and the record-format change is
called out. No further bump is needed. The `save_message` update-by-id
requirement holds for every backend (memory, sqlite `INSERT OR REPLACE`,
nostrdb `mdb_put`, Groundhog `ON CONFLICT DO UPDATE`).

## Required before approval

1. **H1:** bounds never refuse a selectable or linear Commit; Groundhog holds
   `RESOURCE_REFUSED`; tests P1 and P2.
2. **H2:** a distinct "candidate retained" outcome from libmarmot, Groundhog
   retrying held events on it, and the 72-order fuzzer as a test (both retry
   policies).
3. File M1, M2 and L1 as beads, or fix them. Add the L2 tests.

---

## Addendum (2026-10-01): re-review of the fixes

- **Fix commits:**
  - `9be480f6`: libmarmot
  - `1b688b28`: Groundhog, nostrc-xrza
  - `013f4484`: docs
- **Branch:** rebased onto master `543ca0b6`. The range-diff shows the five
  original commits unchanged; only the manifest context moved. This review
  branch is rebased onto `013f4484`.

### Verdict: APPROVE-WITH-NITS

Every finding from the first review is fixed. Each fix is backed by my own
probes, rerun unchanged from the first review, and by mutations: every new
test fails with its fix reverted. Two Medium follow-ups remain (N1 and N2
below). Neither is a regression in libmarmot's convergence. Encrypted groups
still ship behind `GH_FEATURE_ENCRYPTED_GROUPS=0`. Both should be filed and
fixed before that flag is turned on.

### What I ran

| Check | Result |
| --- | --- |
| Build | OK |
| `ctest` (libmarmot, Groundhog store, MLS and conversation tests) | 49/49 pass. One reports Skipped: `groundhog-store-key-keyring`, which needs a keyring. |
| macOS ASan+UBSan: `test_commits`, `test_adopted_commits`, `test_protocol`, `test_storage`, `test_storage_contract` | All pass, no reports |
| `scripts/linux-gate.sh --sanitizers` | Pass, 52 tests |
| `check-unsequenced-args.py` | Clean |
| MDK 0.11 matrix (own driver image, unchanged driver) | Pass. `concurrent-commits` passes in 11 s, including its new withdrawn-witness assertion. |
| Docker volumes | None created |

The probes are not committed. Their diff is kept outside the tree as
`/tmp/rv-w25-conv2-probes.diff`.

**Mutation checks.** Each reverted fix makes its test fail:

| Reverted fix | Failing test |
| --- | --- |
| H1: refuse before selection | `test_linear_commit_with_store_full` |
| H2: `WRONG_EPOCH` for a retained Commit | `test_mdk_forks_every_order`: "40 of 72 delivery orders end off MDK's verdict" |
| L1: parent by seal only | `test_parent_found_by_authentication` |
| M1: no leaf shortcut | `test_held_branch_message_cost_bounded` |
| M2: no epoch query | `test_reorg_reads_only_affected_epochs` |
| Groundhog: no retry on `COMMIT_RETAINED` | `retained-commit-retries-held` |
| Groundhog: capacity refusal not held | `capacity-refusal-held-not-junk` |
| Groundhog: withdrawn messages not marked | `conflict-withdraws-messages` |

### Status of each finding

**H1: fixed.** The bounds now apply after selection, and only to losers
(`conv_retained`, commits.c:3402). Eviction removes the least likely loser:
unattached ones first, then by the best branch through each, a tip before
what it builds on, then epoch and digest.

My first-review probes, rerun unchanged:
- **P1.** Charlie's 4 losers return `-55 COMMIT_RETAINED`. A fifth is also
  `-55`, retained by evicting a lower-ranked earlier one; Bob keeps 4. Charlie's
  honest linear Commit then applies at Bob (`MARMOT_OK`, a Commit result), and
  all three converge.
- **P2.** With the store holding 32, the linear Commit applies (`MARMOT_OK`),
  all three converge, and Bob still holds 32.

Groundhog now holds `RESOURCE_REFUSED` events as `capacity`. They are never
junked, and once dropped the cursor stays pinned behind them, as
`transports/nostr.md` requires.

**H2: fixed.** `MARMOT_ERR_COMMIT_RETAINED` (-55) is distinct from stale input
(commits.c:4194). Groundhog retries held events on it: immediately inside a
retry pass, coalesced to one pass per 250 ms for fresh deliveries.

All of the following were run in my own fuzzer, which models Groundhog's real
policy *including* its junk aging (an event still held after 3 fresh Commits
is junk):

| Orders tried | Result |
| --- | --- |
| All 72 per-fork orders of the MDK 0.11 capture | 72/72 reach MDK's verdict and read every converged-epoch message (was 40/72 diverging) |
| Whole 12-event capture, newest-first (relay backfill order) | Converges, all 4 converged-path messages delivered and not withdrawn |
| 3000 random shuffles of the whole capture, aging disabled | 3000/3000 converge with every converged-path message delivered |
| The same 3000 shuffles, Groundhog's real aging | Residual failures: see N1 |

**M1: fixed.** Measured as in the first review (3-member group, 4 retained
candidates, 3 runs):

| Case | Before | After |
| --- | --- | --- |
| Held candidate-branch message | 2.2 ms | 0.155–0.159 ms |
| Same message, re-offered | 6 ms | 0.117–0.120 ms |
| Normal message (reference) | about 0.08 ms | about 0.08 ms |

The cost is now under 2× a normal message, and a retry pass is cheap.

**M2: fixed.** A reorg reads only the superseded epochs
(`messages_in_epochs`, implemented in the memory, SQLite and Groundhog
backends). Without that query, only matching messages are kept in memory.
Measured on a 200k-row `mls_messages` in SQLCipher 4.17 (286 MB): the
Groundhog query takes 95–143 ms and loads only the 251 matching rows.

**M3: fixed.** Withdrawn messages are marked in the same transaction (seen
namespace 6) and survive a restart. The row, its accessible summary and the
list text say "This message was withdrawn when the group resolved a
conflict", and the group emits `conflict-resolved` with a toast naming what
was undone. The tests cover the reader and the sender. The MDK case asserts
that Groundhog's own witness on the branch it left is marked withdrawn.

**L1: fixed.** A candidate's parent is the retained state its MLS
authentication succeeds against (`conv_parent_of`, commits.c:3088). The
sealing state is only tried first. A Commit sealed under the tip that does
not authenticate there goes to the retained branches. A Commit no state
authenticates is `MLS_PROCESS_MESSAGE`, as MDK does.
`test_parent_found_by_authentication` covers my scenario: a re-sealed child
finds its parent.

**L2: fixed.** New tests pin the witness window
(`test_witness_window_bounds_score`) and the exporter deletion above a lowered
tip.

**L3: filed upstream** as nostrc-sq85 (P2) and stated in the README.

**Nits: fixed.** `CONV_MAX_ORPHANS` is gone, and the libmarmot-vs-MDK
difference on witnesses of withdrawn messages is documented in convergence.h.

### The question on `marmot_save_created_message`: does it change the privacy or forward-secrecy posture?

**MLS forward secrecy: no change.** The stored row holds the kind:445
ciphertext and the inner event, which Groundhog already keeps in its
conversation store and outbox. No key material is added. The new
`ConvBranchSecret.sender_data` (trailer 0xC2) holds candidate states'
sender-data secrets. Those states can already be rebuilt from retained state
plus Commit bytes, so this adds nothing either.

**At-rest privacy: yes, it gets worse for the account's own messages.** See N2.

### New findings

**N1 (Medium, Groundhog; pre-existing rule, new interaction): junk aging drops held convergence input before its parent arrives**

- **Where:** `gnome/groundhog/src/mls/gh-mls-service.c:1910-1915`
  (`held->misses >= GH_MLS_SERVICE_JUNK_AFTER_COMMITS`, which is 3, then
  `remember_junk`).
- **Evidence.** In 3000 random interleavings of the whole capture, with
  Groundhog's aging modelled exactly (misses counted per fresh Commit at the
  end of each retry pass; capacity refusals exempt):
  - 169 runs end on the wrong branch;
  - 336 runs lose a message of the converged path.

  Every failing run has junked events. With aging disabled, 0/3000 fail. With a
  threshold of 6 (`max_rewind_commits + 1`), 0 runs end on the wrong branch and
  1 loses a message.
- **Example** (trial 26): `message_after_witnessed`, then `witnessed_high`
  (both held), then 3 or more fresh Commits from the earlier forks. Both are
  junked before `witnessed_low` and `depth2_*` open their epochs, and the
  branch never wins at this member.
- **Spec.** The junk ids stop later re-delivery (`junk_ids`), and the cursor
  is no longer pinned behind them. That records the events as permanently
  unreadable, which `transports/nostr.md` forbids for deferred and
  resource-refused input.
- **How likely.** Per-fork and newest-first orders are clean. It takes a
  branch event arriving 3 or more Commits ahead of its parent: multi-relay
  catch-up or relay lag in a busy group.
- **Fix.** Make aging horizon-aware: count canonical tip advances, and junk
  only after more than `max_rewind_commits` of them. Keep junked events
  eligible on refetch: don't pin `junk_ids` for them, or pin the cursor as is
  now done for `capacity`. Add the whole-capture shuffle (with aging) to
  `test_mdk_forks_every_order`.

**N2 (Medium, Groundhog privacy): the account's own message text now outlives purge, expiry, conversation delete and leaving the group**

- **Where:**
  - `gnome/groundhog/src/mls/gh-mls-service.c:4107` stores our sent inner event
    (plaintext) in `mls_messages`.
  - Nothing ever deletes from `mls_messages`. There is no `DELETE FROM
    mls_messages` in `gnome/groundhog/src/store/`, and the group cleanup does
    not touch it.
- **What the store already does.** Groundhog's purge deliberately removes
  every plaintext copy: NIP-40 expiry (gh-store.c:3887), the retention window
  (gh-store.c:3897), and conversation forget (gh-store.c:3998). The comments
  say so: "An outgoing message's text is also in its outbox row ... those go
  first"; "A downloaded file's plaintext goes with its message".
- **Failure.** A user sends an expiring message, or deletes the conversation,
  or the retention cutoff passes. The text stays in `mls_messages` in the same
  SQLCipher database indefinitely, and anyone who later holds the store key
  reads it.
- **Pre-existing part.** Inbound MLS plaintext was already retained this way;
  that gap predates this branch. Own messages were not, before `1b688b28`.
- **Fix.** Purge or forget `mls_messages` rows in lockstep with the
  conversation rows (by group and `created_at` or rumor id, inbound and own).
  Or store own messages without `content`: invalidation needs only id and
  epoch, and Groundhog could key the withdrawal by the rumor id it already
  knows.

**N3 (Low): M1's cache is invalidated by every late canonical message**

`branch_cache_get` (commits.c:4364) keys the cache by the record bytes.
`marmot_commit_decrypt_late` rewrites the record on every late message
(commits.c:915). So a member who interleaves late messages with held branch
messages forces a rebuild of up to 32 candidates per branch message. The cost
is linear in their own traffic, unlike the old per-retry blow-up. Key the
cache by the candidate set (the candidates' digests plus the entries' tags)
rather than the whole record.

**Nits**
- Groundhog has no `(mls_group_id, epoch)` index on `mls_messages`, so
  `messages_in_epochs` scans the table: about 0.1 s at 200k rows.
- The branch cache keeps rebuilt candidate states (epoch secrets) in process
  memory until the next branch message or `marmot_free`, even after the record
  has pruned them. Drop it when a group's record changes, or on a timer.
- `ConvWitness.leaf` is only used for the M1 shortcut. Since sender data is not
  signature-bound, a member can claim any leaf, but the shortcut only *skips*
  work and never adds a witness, so this is harmless. Worth a comment.

### Required before enabling encrypted groups (not before merge)

1. **N1:** horizon-aware junk aging, and junked events eligible on refetch.
2. **N2:** purge `mls_messages` with the conversation, or don't store own
   plaintext.

File both as beads.

---

## Final addendum (2026-10-01): N1–N3 and the port onto K, M and L

- **Branch:** `marmot/w25-convergence`, tip `29b49029`, on local master
  `d2c04cdd` (slices K, M and L).
- **New commits:**
  - `920017d7`: port off K's removed test hook
  - `7128c8d8`: libmarmot N3, and the aging fuzz as a test
  - `17afc5fe`: Groundhog N1 and N2
  - `b6ceef61`, `7ba434f7`, `29b49029`: docs
- **Renumbering:** `MARMOT_ERR_RESOURCE_REFUSED` is now -55 and
  `MARMOT_ERR_COMMIT_RETAINED` -56. The previous addendum's "-55" for
  `COMMIT_RETAINED` predates this. The store migration is schema 6.
- This review branch is rebased onto `29b49029`.

### Verdict: APPROVE-WITH-NITS

N1, N2 and N3 are fixed, each confirmed by my own probes and by a mutation
that makes its new test fail. The port onto K, M and L is clean, textually and
semantically.

The two conditions I set for enabling encrypted groups (N1 and N2) are met.
What remains are nits and one P4 bead, nostrc-as6q, which should be raised to
P3 (see "Remaining").

### What I ran

| Check | Result |
| --- | --- |
| Build | OK |
| `ctest -R "marmot\|groundhog"`, MDK interop excluded, `-j6` | 102 tests: 101 pass, 1 fails (see below) |
| macOS ASan+UBSan: `test_commits`, `test_adopted_commits`, `test_protocol`, `test_storage`, `test_storage_contract` | All pass, no reports |
| `scripts/linux-gate.sh --sanitizers` | Pass, 53 tests |
| `check-unsequenced-args.py` | Clean |
| MDK 0.11 matrix (driver image rebuilt from this tree's `driver-0.11`, which K and L changed) | 11/11 pass, `concurrent-commits` in 12.4 s |
| Docker volumes | None created |

- **The ctest failure.** `groundhog-mls-kp-lifecycle-adopted`
  (`legacy-switched-off`: "no MDK 0.8 KeyPackage left on the relays did not
  happen within 90 s") failed once under that load.
  - Alone it passes 3/3 (40–54 s).
  - It also passes in the sanitizer gate, whose list includes it.
  - It is slice K's test, and the branch touches no KeyPackage-lifecycle file.
  - It is a load-sensitive wait, not this branch. File it as a flake.
- **The MDK matrix.** The cases that used to assert refusals now pass for real
  (no skips), since K publishes adopted KeyPackages.

The probes are not committed. Their diff is kept outside the tree as
`/tmp/rv-w25-conv3-probes.diff`.

### The port: `git range-diff 543ca0b6..013f4484 d2c04cdd..b4a12904`

All eight reviewed commits map one-to-one. The only "changed" one is the
manifest commit, whose rows were rewritten around master's.

**No convergence code moved.** Every difference is context or the error
renumbering. No hunk of `commits.c`, `convergence.c`, `convergence.h` or
`messages.c` differs. I then checked the semantics of each slice:

- **K:**
  - K took -54 (`MARMOT_ERR_KEY_PACKAGE_CAPABILITIES`). The codes -53 to -56
    are unique in `marmot-error.h`, and the README and manifest agree.
  - K removed `gh_mls_service_test_create_adopted_group_async`. `920017d7`
    now has `concurrent-commits` publish Alice's adopted KeyPackage on her
    write relay, create the group through `gh_mls_service_create_group_async`
    (New Group's real path, as `adopted-commits` does), and assert the group is
    adopted. That is stronger than the hook.
- **M:**
  - M's producers (SelfRemove requirement, routing) and L's (`0x800b` media
    policy, `0x8002`/`0x8007` image) all end in `finish_adopted_commit` →
    `finish_local_commit` → `marmot_commit_stage_pending` with the Commit
    bytes. `pending_apply` persists them through `marmot_commit_persist_ex`, so
    a reorg still demotes our own Commits as candidates instead of dropping
    them. No producer bypasses this tail.
  - A reorg that undoes a routing rotation still reports `routing_changed`
    from `conv_install`, so M's follow logic applies. The MDK
    `routing-rotation` case passes.
- **L:**
  - L took store schema 5, so N's scrub is schema 6. A static assert ties the
    migration count to `GH_STORE_SCHEMA_VERSION`.
  - The resolutions in `gh-message.c` and `gh-message-row.c` keep a withdrawn
    message's attachment cards, files and file count out of the row and out of
    its accessible summary. The MDK `white-noise-media` case passes.

### N1 (held-event aging): fixed

- **Change** (`gh-mls-service.c`, `GH_MLS_SERVICE_JUNK_AFTER_EPOCHS` = 6): an
  event is dropped only once the group's epoch is 6 past the epoch it was held
  at. `junk_ids` now only suppresses the decrypt-pending notice: an aged-out
  event fetched again is held again (`process_event`, `aged`).
- **My fuzzer, rerun** with this exact rule: newest-first plus 3000 seeded
  shuffles of the whole 12-event capture, Groundhog's retry policy,
  `COMMIT_RETAINED` and `RESOURCE_REFUSED` included.

  | Aging threshold | Aged events refetched? | Wrong branch | Lost a converged-path message |
  | --- | --- | --- | --- |
  | 6 epochs (shipped) | no | 0/3001 | 0/3001 |
  | 3 epochs | no | 896 | 1348 |
  | 2 epochs | no | 1562 | 2032 |
  | any | yes (ideal refetch) | 0 | 0 |

  The 3-epoch row matches the author's 883 and 1347 with another seed. The
  "yes" row shows that refetch is the fallback, but the horizon threshold is
  what protects convergence input when no refetch happens.
- **Limitation.** The capture spans only 5 epochs, so the 6-epoch threshold
  never fires within it. That is the point of horizon-based aging: inside the
  rollback horizon, nothing ages out. But the boundary itself is argued from
  `max_rewind_commits`, not exercised by a vector.
- **Mutations.**
  - Threshold set to 2: `branch-event-outlives-commits` fails ("unreadable == 0,
    want 2"), as does `commit-before-proposal`.
  - Aged events not re-held: `decrypt-pending-honest` fails.

### N2 (no plaintext in libmarmot's message rows): fixed

- **Design.** `ghm_save_message` keeps only the ids, group, epoch, state,
  times and kind, with the inner event's canonical id in place of the content
  (`message_record_content`). It drops tags and the kind:445, and zeroes the
  author.
  - I checked what reads these rows back. libmarmot uses only existence
    (`find_message_by_id` for duplicates) and id, epoch and state
    (`invalidate_messages`). Groundhog uses the stored content only as the
    inner id (`withdrawn_messages`). marmot-gobject never runs on
    GhStoreMarmot.
  - The plaintext now lives only in the conversation store, which expiry,
    retention and forget purge.
- **Canary scans** (`groundhog-privacy-mls`). They decrypt every page of
  `store.db` and every WAL frame with the store key, after a restart, with
  positive controls. They cover:
  - disappearing-message expiry (received; Groundhog itself sends no expiring
    messages);
  - the retention window, sent and received;
  - forget conversation, sent and received;
  - leaving the group and then forgetting it, sent and received.

  That covers expiry, retention, forget and leave. Mutation: storing the full
  content again makes `purged-text-leaves-the-store` fail.
- **Schema 6 scrub, on disk** (my own probe). The `v6-scrubs-messages` test
  checks rows through SQL only, so I added a probe:
  - Setup: a short canary, a canary in tags, and a long canary spanning an
    overflow page, all in a schema-5 row; then the store is migrated and
    closed.
  - Method: I decrypted every page independently (`openssl`, AES-256-CBC with
    the raw store key; page 1 decrypts to a valid SQLite header).
  - Before migration the canaries sit on pages 35 and 56 (56 is the overflow
    page). Afterwards they are on no page. SQLCipher's `secure_delete`
    zeroes the freed overflow page, and closing removes the WAL.
- **Withdrawal with only inner ids: yes.**
  - libmarmot reports the withdrawn kind:445 ids. `withdrawn_messages` maps
    each stored row's content (the inner id) to the conversation message.
  - Both sides derive the same canonical id. libmarmot's `check_inner_author`
    computes it. Groundhog's `gh_message_new_from_mls` and
    `message_record_content` both validate a present `id` field, or compute it
    when absent.
  - An inner event whose `id` field lies is never shown, so there is nothing
    to withdraw.
  - Tests: `conflict-withdraws-messages` (reader and sender, restart) and the
    MDK `concurrent-commits` round 3 assertion both pass.

**nostrc-as6q (P4): the scrubbed rows still outlive forget, leave and
retention.** The bead describes them as activity metadata. One property is
missing from its description: an inner event id is SHA-256 over
`[0, pubkey, created_at, kind, tags, content]`. With the group's few members
as candidate authors, the row's time and kind, and empty tags for plain text,
the id is a confirmation oracle. Whoever holds the store key can recover a
*purged* short message ("ok", "yes", "at 5") by guessing.

The same oracle already exists in `mls_processed_messages`, whose
`message_event_id` is kept indefinitely, so this is not new to the branch.
Recommendations:
- Raise as6q to P3.
- Delete a group's `mls_messages` rows at forget and group end, and rows older
  than `tip − max_rewind_commits`.
- Treat `mls_processed_messages` the same way past the retention horizon.

### N3 (late messages no longer drop the branch cache): fixed

- **Change** (`branch_cache_key`). The key covers the group, the tip
  (epoch and confirmed transcript hash), each retained entry (epoch,
  confirmed transcript hash, Commit digest, own, reader) and each candidate
  (source epoch, digest, parent hint, own). Those are what the rebuilt tree
  depends on. Witnesses and ratchet progress are out, so a late message no
  longer forces a rebuild.
- **Freshness.** The leaf shortcut reads the record as stored now, so
  witnesses are current.
- **Mutation:** dropping the cache on every late message makes
  `test_held_branch_message_cost_bounded` fail ("after a late message: nothing
  rebuilt").
- **Memory nit, fixed.** The cache is now freed when the tip moves, at a
  reorg, at an eviction, and when the candidate set changes. Rebuilt states
  the record no longer retains don't linger.

### Remaining (nits, none blocking)

1. **nostrc-as6q:** as above; raise to P3 and note the confirmation oracle.
2. **Schema-6 rows without an id.** A pre-v6 row whose inner JSON has no `id`
   field gets NULL content. If a reorg within 5 epochs of the upgrade
   withdraws that message, it is not marked. This only matters at upgrade
   time, and the encrypted-groups flag is still off.
3. **Aging edge.** An event held 6 or more epochs before its branch's own
   parent arrives can still age out while that branch is eligible (fork
   ≥ tip − 5). It is held again if fetched again. Acceptable: such an event is
   undecryptable, so its own epoch is unknowable.
4. **WAL residue.** Between a purge and the next checkpoint, old page images
   remain in WAL frames (encrypted). The canary scans run after a restart. A
   `wal_checkpoint(TRUNCATE)` after `gh_store_purge` and forget would close
   that window.
5. **Flake.** File `groundhog-mls-kp-lifecycle-adopted`'s 90 s wait as
   load-sensitive.
