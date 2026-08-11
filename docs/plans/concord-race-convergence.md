# Concord: same-epoch rotation race convergence (CORD-06)

Bead: `nostrc-r874`. Spec: CORD-06 §3 "Failure and races".

> Two rotations racing to the same epoch converge deterministically: among
> authorized candidates at the same continuity point, the lexicographically
> lowest new *base* key wins — the `control_root` pair rides the winner's blobs,
> never compared — every client computes the same winner, and a losing Refounder
> re-issues anything only its branch knew (its fresh channel keys) on the winning
> chain, its own minted pair dropped with its root. Both forks' keys are
> retained, so messages sent into the losing fork stay readable, and the
> same-epoch heal is **down-only** — a held epoch re-converges solely to a
> strictly lower sibling, so a flaky fetch that returns only the higher sibling
> can never re-fork a settled epoch.

Everything below lands in
`apps/gnostr/plugins/concord-communities/gn-concord-community-service.c`.

## 0. What exists, and what is missing

`PendingRotation` is already keyed by `(scope, rotator, newepoch, prevcommit)`,
so two Rotators at one continuity point accumulate as two rotations and never
merge into one chunk set. That is the half that works.

Three things are missing:

1. **The competitor never reaches ingest.** `ingest_rotation_wrap()` derives the
   base rekey address from the root held *now* for `root_epoch + 1`, and
   `refresh_rekey_plane()` unsubscribes the previous address on every adopt. Once
   a client adopts fork A at epoch N+1, fork B's rotation — published at the
   address derived from the *prior* root R0 for epoch N+1 — is neither
   subscribed nor derivable. It is dropped before any comparison could happen.
2. **There is no comparison.** `adopt_base_rotation()` adopts whatever it is
   handed.
3. **There is no retention.** `CommunityState` holds exactly one
   `community_root`; the loser's root is overwritten and its fork's messages
   become unreadable.

## 1. Retired roots (the retention structure)

```c
/* CONCORD_RETIRED_ROOTS_MAX 8 */
typedef struct {
  gchar   *root;       /* secret, hex — a root this device no longer writes under */
  guint64  epoch;      /* the root_epoch this root was the community_root at */
  gchar   *control_pk; /* the Control signer address at that epoch, or NULL (legacy) */
} RetiredRoot;
```

`CommunityState` gains `GPtrArray *retired` (newest first).

**No `control_root`.** A retired branch is read-only by construction: nothing is
ever written at a dead epoch, so the write secret is dropped, not retained. Only
the *address* (`control_pk`) is kept, because reading a retired epoch's Control
Plane needs it.

**Two producers, one call site.** In `adopt_base_rotation()`:

- an adopt that *wins* first retires the root it is replacing —
  `retire_root(state, state->community_root, state->root_epoch, state->control_pk)`.
  On a forward adopt that is the predecessor R0@N (needed both to read the epoch
  just left and to derive the sibling rekey plane, §3); on a heal it is the
  losing sibling R_X@N+1.
- an adopt that *loses* the comparison retires the refused root before returning:
  `retire_root(state, new_root_hex, new_epoch, new_control_pk_hex)`.
  This is what makes retention symmetric — a client that adopted the low root
  first still opened the high sibling's blob to compare it, so it holds that
  fork's key and must keep it. Without this branch, "both forks' keys are
  retained" would hold only for whoever guessed wrong first.

**Bound and eviction.** `CONCORD_RETIRED_ROOTS_MAX = 8`, prepend-newest,
evict-tail (oldest retirement first). The bound is not a cache-sizing decision:
every retained root is a readable epoch, so the set is a credential surface and
must not grow with history. Eight covers a burst of competing rotations plus a
few epochs of scrollback. Re-retiring a root already present refreshes it in
place (replay idempotence) rather than duplicating.

**Consulted where a read fails.** The current root is always tried first; retired
roots are only reached when it does not open the artifact.

- `derive_channel_key()` gains `channel_key_candidates(state, channel, out, max)`:
  candidate 0 is the key held now, then one per retired root. A **private**
  Channel contributes exactly one candidate — its key is independent of the root
  (CORD-03), so retired roots say nothing about it. (Retaining prior *Channel*
  keys across a channel rekey is a different retention problem and is out of
  scope for this bead.) A **public** Channel yields one candidate per retired
  root at the same channel epoch.
  `ingest_wrap()` selects the candidate whose `key.pk` equals the wrap's pubkey
  rather than trial-decrypting: a Chat wrap is *addressed* at the stream pk, so
  the address is already the discriminator, and selection stays O(1) crypto.
- `derive_control_read_key()` gains `control_read_candidates()`, returning
  `{key, address}` pairs — address is `control_pk` when set, else the legacy
  `concord/control` derivation's own pk (CORD-02 §5), evaluated per retired
  entry at *that entry's* epoch. `ingest_control_wrap()` walks candidates in
  order and stops at the first that accepts.
- The Guestbook is deliberately **not** given a fallback: it is off-consensus,
  re-seeded per epoch, and a member's own later word supersedes a stale seed.

**Backfill.** `gn_concord_community_service_refresh_channel()` gains a second
pass that queries each retired address for a public Channel and ingests what it
finds. This is the path that pulls in messages written into the losing fork
before the heal landed, and it costs nothing on a Community that never raced
(the retired set is empty until the first rotation). A straggler that writes into
the losing fork *after* this device healed is picked up by the next refresh, not
in real time — accepted degradation, since every honest client heals downward and
stops writing there.

**Persistence.** `build_join_material()` emits a `retired` array —
`[{root, epoch, control_pk?}, …]` — **only** in the Community List direction
(the `include_control_root == TRUE` call, i.e. this npub's own devices,
CORD-02 §8). An invite bundle must never carry retired roots: handing a fresh
joiner the epochs a Refounding severed is the one thing the whole mechanism
exists to prevent. `accept_bundle()` parses it back, bounded at
`CONCORD_RETIRED_ROOTS_MAX` and hex-validated per entry, following the existing
`control_root` precedent (parsed on the one path, emitted on the other). A
re-accepted link, which carries no `retired`, must not drop the set already held
— same carry-over rule as `control_root`.

## 2. The down-only heal

One choke point, inside `adopt_base_rotation()`, so the rule holds for the local
Rotator's own adopt and for every ingest path alike:

```c
/* CORD-06 "Failure and races": a strictly higher epoch always wins; a
 * same-epoch sibling wins only on a strictly lower new base key; a lower
 * epoch never wins. The control pair rides the winner and is never compared. */
static gboolean base_rotation_wins(CommunityState *state,
                                   const char *new_root_hex, guint64 new_epoch) {
  if (!state->community_root) return TRUE;
  if (new_epoch != state->root_epoch) return new_epoch > state->root_epoch;
  return g_strcmp0(new_root_hex, state->community_root) < 0;
}
```

Consequences worth stating:

- A replay of the rotation already adopted compares equal and is a **no-op**,
  where today it re-runs the whole adopt. Idempotence gets cheaper, and the
  existing "replaying the rotation is harmless" test still passes.
- A flaky fetch that returns only the higher sibling compares higher and is
  refused, so a settled epoch never re-forks upward. This is the second
  acceptance criterion, and it is enforced structurally rather than by ordering.
- The `control_root`/`control_pk` pair is never part of the comparison. It rides
  the winner: the loser's minted pair is dropped when the winner's adopt
  overwrites `control_pk` and clears/replaces `control_root`.

## 3. Reaching the competitor: the sibling rekey plane

A base rotation now reaches this device at one of two addresses:

| plane   | derived from                   | for epoch      | continuity point |
|---------|--------------------------------|----------------|------------------|
| forward | the root held now              | `root_epoch+1` | `root_epoch`     |
| sibling | the retired root at `root_epoch-1` | `root_epoch` | `root_epoch-1`   |

`CommunityState` gains `sibling_rekey_address` / `sibling_rekey_subscription`;
`refresh_rekey_plane()` maintains both (and backfills both). Exactly one sibling
plane is kept — the continuity point of the epoch currently held — which is the
minimum convergence needs and keeps the subscription count flat.

`ingest_rotation_wrap()` picks the plane by matching the wrap's pubkey against
the two derived addresses, and then runs the *same* path with the chosen
`new_epoch`, `held_epoch` and continuity root. The continuity check for a
sibling recomputes the commitment over the **retired** root at the retired
epoch, so a sibling is accepted only if it genuinely extends the same key the
winner extended — a fork from anywhere else is still garbage. This needs
`epoch_commitment_of(root_hex, epoch, out)`, with `held_epoch_commitment()`
becoming a wrapper.

### 3b. The prior-root Channel rekey plane (`nostrc-8h0l`)

The same retention closes a live-path bug the Channel half left behind. A
Refounding publishes its base chunks first and its Private Channel rotations
second, both addressed under the prior root (`nostrc-ay29`). A recipient
watching live receives them in that order: it folds the complete base set,
adopts, and `readdress_planes()` drops every `rekey:<channel>` subscription and
re-derives it under the *new* root — so the Channel rotation, still sitting at
the prior root's address, arrives at a subscription nobody holds. That device
keeps the old Channel key and the removed member keeps reading the Channel the
Refounding was supposed to sever them from. An offline recipient is fine, because
`refresh()` backfills the Channel rekey planes before the base one.

So `refresh_channel_rekey_plane()` maintains **two** addresses per held Private
Channel — the one derived from the root held now, and the one derived from the
prior root, in the `rekey:` and `rekey0:` subscription slots — and
`adopt_base_rotation()` re-creates them for every Channel after re-addressing,
which it does not do today. `ingest_rotation_wrap()` accepts a Channel rotation
at either address. Nothing else about a Channel rotation changes: its continuity
is a commitment over the *Channel* key, which a base roll never touched, so only
the address was ever in question.

Only the *prior* root joins the rotation planes, never the whole retired set:
older roots are read-only history, and one extra subscription per plane keeps the
subscription count flat. `prior_root(state)` — the retired entry at
`root_epoch - 1` — is the single helper behind both this and the base sibling
plane of §3.

A channel-scope *race* (two Rotators rekeying one Channel at one epoch) is a
separate convergence question and is not in this bead.

## 4. The two landmines

**(a) `adopt_base_rotation()` and `adopt_channel_rotation()` clear `state->rotations`.** Today it calls
`g_hash_table_remove_all()`, which would drop a sibling still assembling its
chunk set — precisely the competitor whose blob decides the race. New rule: drop
only entries whose `new_epoch` differs from the epoch just adopted; keep the
same-epoch siblings, because a third, even lower competitor may still be under
judgment. `adopt_channel_rotation()` has the same line and the same fix, scoped
to its own Channel — today a Channel rotation adopted mid-race would throw away
the base siblings under judgment. Since keeping entries lets the table grow, the table gains a bound
(`CONCORD_MAX_PENDING_ROTATIONS = 16`); a new rotation beyond it is refused
rather than accumulated.

`conclude_rotation()` needs one companion fix: a losing sibling that carries no
blob for this npub currently emits *"This Community was refounded without this
account"*, which is a false alarm when this device already holds that epoch under
the winning root. Gate the removal verdict on
`state->root_epoch < pending->new_epoch` — if this device already holds the
epoch, it was not removed. The Rotator-outranks-target authority check stays
unconditional: a rotation that removes someone its Rotator does not outrank is
dropped whether it wins or loses.

**(b) `adopt_base_rotation()` sets the refounder unconditionally.** With the
win-gate at the top of the function, the unconditional set becomes
correct-by-construction — the function is only reached by a winner, so on a heal
it overwrites the loser's refounder with the winner's, which is exactly "the
refounder must follow the winner". No extra branch; the fix is the gate's
placement, above the `set_refounder()` call, and the comment must say so
explicitly or the next reader will "fix" it back.

(A Guestbook snapshot already folded from the losing refounder stays folded. Both
forks attest the same *prior* epoch's membership, so the seed is the same
attestation either way; it is benign and not worth unwinding.)

## 5. Losing-Refounder re-issue semantics

The losing Refounder drops its minted pair with its root (§2) and re-issues only
what its branch knew: its fresh Channel keys.

Those keys need no re-mint. CORD-06 §3 already requires a Refounding's Channel
rekeys to be sealed and addressed under the **prior** `community_root`, never
the freshly minted one, exactly so they stay openable on either branch of a base
race. What must not happen is the address moving under them mid-flight.

- `RotationMint` gains `gchar *address_root` — the root held when the rotation
  was minted, captured up-front like the Memberlist and the Control heads.
  `derive_base_rekey_key()` / `derive_channel_rekey_key()` gain `_at(root_hex, …)`
  variants; the existing signatures stay as wrappers so no call site is forced to
  change. Publishing must address from `mint->address_root`, never from
  `state->community_root`, or a heal landing mid-publish silently relocates the
  remaining chunks to an address nobody listens at.
- A base mint is **superseded** the moment this device holds something else at
  that epoch:

  ```c
  static gboolean base_mint_superseded(CommunityState *state, RotationMint *mint) {
    if (mint->channel_id) return FALSE;
    if (state->root_epoch != mint->new_epoch) return state->root_epoch > mint->new_epoch;
    return g_strcmp0(state->community_root, mint->new_root) != 0;
  }
  ```

  (Before its own adopt, `root_epoch < new_epoch` → not superseded. After its own
  adopt, the roots match → not superseded. After a heal → superseded. No
  generation counter, no new state.)

  On supersession the losing Refounder abandons **the base half**: remaining base
  chunks, the Control re-anchor and the Guestbook snapshot are all statements
  about a dead root. It keeps publishing **the Channel rotations**, addressed
  under `mint->address_root`. That is the whole of "re-issues only its fresh
  channel keys on the winning chain".
- What it must never do is mint a *new* base rotation at `N+2` to reassert. That
  would be an upward fork wearing a higher epoch, and no rule above would stop
  it.

> **Interface note for `nostrc-ay29`** (channel rekeys during Refounding, in
> `rotation_publish_next_chunk`): address channel rekeys from
> `mint->address_root` via `derive_channel_rekey_key_at()`, not from
> `state->community_root`; and let the supersession check skip the base half
> while your channel half continues. Those two are the only seams between these
> beads.

## 6. What landed

As designed, with two adjustments the implementation forced:

- **The refused sibling republishes the List.** Retaining the loser's root is
  new membership state, so the refusal branch calls `publish_community_list()`
  too — otherwise the fork stays readable on the device that judged the race and
  nowhere else, and is lost on restart. `retire_root()` returns whether it
  actually retained anything, so a plain replay (equal root) publishes nothing.
- **`derive_channel_rekey_key_at()` takes no `CommunityState`.** A Channel
  rotation's address needs only the root and the Channel id, so the parameter
  was dead. The base variant still takes it, for the community_id.

## 7. Acceptance criteria → tests

All in `tests/test-concord-service.c`. The competing rotations are hand-minted
(the file already forges rekey wraps) rather than produced by two
`refound_async()` calls, because a minted root is random and the test needs to
name which one is lower.

`/concord/race/converges-on-lowest` — two BAN-authorized Rotators (the second
granted a BAN Role), two rotations at one continuity point with roots
`R_low < R_high`. Device 1 ingests high-then-low, device 2 low-then-high.

1. **Converge on the lowest, on every client.** Both end with the public
   Channel's stream address equal to the address the test derives from `R_low`
   — an assertion against a value computed independently of the code under
   test, not merely "the two devices agree".
2. **No upward re-fork.** Device 2's address is unchanged across the
   later-arriving higher sibling, and re-ingesting the winner is a no-op.
3. **Losing-fork messages stay readable after the heal.** A message published
   into the `R_high` fork's public Channel opens on *both* devices — device 1
   because it adopted and retired `R_high`, device 2 because the refused sibling
   was retired rather than discarded — and *not* on a third device that never
   saw either rotation. The epoch left behind reads too.

`/concord/race/retired-roots-ride-the-list` — both forks appear in the List
entry's `retired` array at their own epochs; a second device reconstructed from
that entry lands on `R_low` and reads the losing fork; and a refresh that
carries no `retired` (what an invite bundle looks like) does not take the set
away.

`/concord/refound/channel-rotation-after-base` — the `nostrc-8h0l` live order:
the base set complete and adopted first, the Channel rotation delivered after,
and the Channel key still lands — proven by opening a message the Refounder
publishes into the rotated Channel.

ASan-clean is a hard gate: the retired set holds secrets, so `retire_root()` and
eviction go through `clear_secret()`.
