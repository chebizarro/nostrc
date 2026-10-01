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
