# Groundhog W5 partial upstream push: independent review

- **Date:** 2026-09-27
- **Range:** `origin/master` (`5c7bcb9c`) → `master` (`64752709`), 4 commits, linear
- **Scope:** bounded peer review of this partial push only. Groundhog is intentionally not release-ready. Complete messaging is not a requirement here.

## Verdict: **APPROVED**

I found no blocking defects. Nothing in the range makes master less safe than `origin/master`:

- The libnostr change stops same-second events from being silently lost. It can still deliver a boundary-second event twice (F1), which is tolerated by protocol semantics and deduplicated by Groundhog.
- The signer change fails closed.
- Versions follow `AGENTS.md`.

Before bead `nostrc-qp24.4.1` is closed, **F1 must be fixed**. F2–F4 should be filed as follow-ups. None of this review's changes touched code or beads.

| Commit | Slice | Result |
| --- | --- | --- |
| `57617d6b` | Beads pre-push hook now also runs the build gate | OK, with follow-up F7 |
| `fc8fdfc0` | libnostr: same-second reconnect dedup, EOSE re-arm, wake reader on remote close | OK, with follow-ups F1–F3 |
| `f2ae3b58` | Groundhog signer private bus + nip55l check that the pending sender is still alive | OK, with follow-ups F4–F5 |
| `64752709` | Groundhog accessibility extraction + version merge | OK, with follow-ups F6 and F8 |

## Method

- Read the exact diff plus the focused call paths:
  - EOSE producers and consumers
  - the refire/reconnect path
  - the LWS close callback
  - the libgo channel close/free semantics
  - the service's coalescing and `forget_sender` logic
  - the account-UI signal wiring
- Built an isolated scratch tree from `git archive HEAD` under `/tmp`. It used the main checkout's submodules, whose SHAs match the gitlinks. The worktree was not modified.
- **Focused tests:** `libnostr_*` plus every `groundhog*` test. All 21 pass. The GTK tests actually ran on this host rather than skipping.
  - `groundhog-launch` skips on macOS by design. See the pre-existing observations.
- `nip55l_dbus_contract` passes, including the new `test_cancelled_private_sender` case.
- **Mutation and variant checks, scratch copy only:**
  1. Disabled the new nip55l liveness check. The contract test still passed 6/6 (F4).
  2. Ran a newest-first replay variant of `test_reconnect_same_second`. It re-delivered the already-seen boundary event (F1).
- I did **not** run the full pre-push gate (clean full build plus full `ctest`). This push will run it (F7).

## Blocking findings

None.

## Follow-up findings (ranked)

### F1 (P1): the boundary-ID dedup does not cover newest-first replay. Confirmed.

`libnostr/src/subscription.c:417-424` clears `seen_cursor_events` as soon as any newer event is enqueued.

NIP-01 relays usually replay stored events newest-first. If even one event newer than the cursor `T` was published during the outage, the replay after the inclusive `since=T` REQ looks like this:

1. The newer event arrives first. The cursor moves to `T+k` and the ID set is cleared.
2. The already-delivered event at `T` then fails the `ev_created_at >= cursor` test and is enqueued again.

**Evidence:** I changed round 2 of the scratch fixture to send `[T+1, T(new), T(replay)]`. The test then found an extra queued event with `is_boundary_replay=1`.

**Impact:** duplicates only, no loss. Groundhog deduplicates by ID (`gh-relay-scope.c:225`), and the pools deduplicate too. However:

- The bead's acceptance criterion ("both delivered exactly once") is met only for the ascending order that the test uses (`test_reconnect_same_second.c:89-95`).
- The commit title overstates the guarantee.

**Fix:** at refire time, snapshot the pre-reconnect boundary set. Keep suppressing those IDs, whatever the current cursor is, until that REQ's EOSE. Then add the newest-first test case.

### F2 (P2): the boundary ID set has no memory bound, and its scan is linear

`seen_cursor_events` grows by one ~96 B node for each enqueued event whose `created_at` equals the maximum seen so far. `subscription.c:380` scans the whole list while holding `sub_mutex` on the relay reader thread.

No filter matching happens client-side, so a hostile relay can:

1. Pin a future `created_at`.
2. Stream many signed events at exactly that second.

The result is memory that grows for the whole life of the subscription, plus O(n²) CPU on the reader thread. Signature verification cost dominates until roughly 25k same-second events, so this is an amplification risk rather than an immediate one.

**Fix:** cap the set (for example 512 entries, or use a hash set). On overflow, fall back to `since=cursor+1`. Drop the set after the replay's EOSE.

### F3 (P2): re-arming EOSE turns a single blocking send into one per reconnect

`relay.c:969` resets `eosed` on every refire. `dispatch_eose` still performs a **blocking** `go_channel_send` into an 8-slot channel (`subscription.c:507`), and it runs on the relay reader thread, which also performs the reconnect.

Any live subscription whose consumer stops draining EOSE after the first one will wedge the whole relay's reader on the 9th reconnect.

I checked the in-repo long-lived consumers:

- The `GNostrSubscription` monitor, `simplepool.c`, and the streaming loop in `nostr_simple_pool.c` all keep draining.
- The one-shot readers exit.

So this is a latent contract hazard, not a current wedge.

**Fix:** use `try_send` for re-arm EOSEs, or coalesce them. Also document multi-EOSE semantics. `nostr_subscription_is_eosed()` (header line 203, "has been observed") is no longer monotonic.

### F4 (P2): the nip55l regression test does not exercise the new branch

`test_cancelled_private_sender` closes the sender and then approves (`test_signer_dbus_contract.c:926`). On this host, `NameOwnerChanged` cleanup always wins that race. As a result, with `pending_bus_sender_alive` disabled, the test still passes 6/6.

The fix itself is correct by inspection:

- Coalescing is keyed by sender, so checking the first call's sender is sound.
- The check fails closed.
- Unique names are never reused.

The end-to-end revocation through a real daemon is genuinely covered. Only the "before cleanup runs" branch named in the 0.5.1 changelog is not.

**Fix:** add a deterministic test hook (for example, a test-only delay in handling `NameOwnerChanged`), or narrow the claim.

### F5 (P3): signer private bus is per account, not per operation

Cancelling one call closes the account's shared private connection (`gh-signer.c:90-97`). That revokes **every** pending approval for the account, and unrelated callers get `CANCELLED` (`:251`). This is documented in `gh-signer.h`.

Separately, `g_dbus_connection_new_for_address_sync` (`:319`) blocks the main thread on the first call after a select or cancel.

There are no in-app callers yet, so this is acceptable now. Before messaging is wired in, choose either:

- a connection per operation, or
- a distinct error for collateral revocation, so that a send is not silently dropped.

Identity binding is sound:

- The generation check covers reselection.
- Signed results are validated against the original request's pubkey, kind, content, and tags (`:168-186`).
- The service's `IdentityChanged` check applies.
- The GUID comparison prevents connecting to a different bus.

### F6 (P3): accessibility focus handling

`update()` grabs focus on any real state transition (`gh-account-ui.c:120`). An asynchronous transition, such as the signer becoming unavailable, can still steal focus from an in-progress edit once composing exists.

**Fix:** only grab focus when the window is active and focus is not in an editable widget. Alternatively, only grab it when focus was previously inside the stack.

Test coverage notes:

- The transition-only test is meaningful: `notify::network-available` runs `update()` synchronously.
- Both GTK tests self-skip (77) without a display.
- CI never builds Groundhog (`BUILD_GROUNDHOG` defaults to OFF), so these tests only run locally.

### F7 (P3): the pre-push gate is now effective, but its mechanics are weak

`core.hooksPath` is an absolute path to the main checkout's `.beads/hooks`, so this gate now applies to **every worktree**. The installer chaining is correct: its test passes, idempotency holds, a failing Beads hook short-circuits the build, and unknown hooks are refused. But `scripts/pre-push`:

- Rejects a push when `_build` is missing, which is the default in agent worktrees (`:19`).
- Then runs `rm -rf _build`, destroying any custom configuration (`:27`).
- Builds default options only, which excludes Groundhog.
- Masks configure failures with `| tail` without `pipefail` (`:28`).
- Tests the working tree rather than the pushed commit.

Also, `scripts/test-install-hooks.sh` is mode 100644 and is not wired into CI or `ctest`.

### F8 (nit): stale version text in the commit message

The `64752709` message says "0.2.0 -> 0.3.0". After the merge, the actual bump is 0.2.1 → 0.3.0. The sources and manifest are correct.

## Checked and OK

- **Lifecycle / deadlocks**
  - `dispatch_event` now holds `sub_mutex` across `try_send`. That call is non-blocking.
  - No path that holds `sub_mutex` waits on the reader. `refire_since` releases the lock before reacquiring it, so there is no nesting.
  - The lifecycle thread's events-channel close is now serialized against enqueue, which is an improvement.
- **Remote-close wake (`connection.c:414-425`)**
  - The channel ref is taken under `priv->mutex`.
  - `go_channel_close` is idempotent, and `go_channel_free` is a refcounted unref, so there is no UAF against the `connection_new` failure path or `nostr_connection_close`.
  - Readers drain queued frames before they see the close.
- **Security**
  - No new logging of secrets.
  - `NameHasOwner` fails closed.
  - The `.beads` changes in the range are two status moves (`nostrc-8gkz` and `nostrc-qp24.4.1` → `in_progress`) plus a reordered export. The memory records containing pubkeys and hostnames already exist on `origin/master`, unchanged.
- **Version policy**
  - libnostr 1.0.0 → 1.0.1 (PATCH), nip55l 0.5.0 → 0.5.1 (PATCH), Groundhog 0.2.0 → 0.2.1 → 0.3.0 (MINOR for the new shipped behaviour).
  - `VERSION_MANIFEST.md` matches every authoritative source.
  - None of these components have Meson sources.
  - GTK 4.14 is the floor, which `gtk_accessible_announce` requires.

## Pre-existing (not caused by this push)

- With `GROUNDHOG_RUN_GUI_SMOKE=1`, `groundhog --smoke` segfaults on macOS: `g_resources_register(NULL)` at `main.c:180`, because `groundhog_get_resource()` returns NULL. `origin/master` crashes identically. Worth a bead.
- CI does not build or test Groundhog at all.

---

## Addendum: 2026-09-27, delta re-review of `8a2d8fd3`

- **Delta:** `8a2d8fd3` is a cherry-pick of `82aa7fab`. `git range-diff` shows the two commits are identical.
- **Intervening commit:** `0ea92cda` touches `.beads` only. It files the follow-ups: `nostrc-qp24.4.2` (F2), `qp24.3.1.1` (F4), `qp24.3.2` (F5), `qp24.8.1` (F6), `8gkz.1` (F7), `qp24.15.1` (CI), and `qp24.8.2` (the macOS crash).
- **Scope:** only F1 and F3. Other findings were not reopened.

### Verdict for the updated partial push: **APPROVED**. No remaining blockers.

- F1 is **resolved**.
- F3 is **resolved**. One small documentation point remains (R2).

### What changed

- `nostr_subscription_prepare_refire()` does four things under `sub_mutex`:
  - records the refire's inclusive `since` second as `replay_boundary_created_at`
  - frees the previous boundary set
  - re-arms `eosed`
  - resets `match`

  `relay.c` now calls it.
- **Dispatch:**
  - IDs at the current cursor second stay in `seen_cursor_events`.
  - When the cursor first moves past the boundary second, that list is handed to `replay_boundary_events` instead of being freed.
  - Later events at the boundary second are deduplicated against, and added to, that list.
- **EOSE:** the blocking `go_channel_send` became `go_channel_try_send`. A full channel now drops (coalesces) the extra signal instead of blocking. This is documented on `nostr_subscription_get_eose_channel`.

### Boundary-set lifetime (inspected)

- The set exists from the first advance past the boundary until the next `prepare_refire` or until `subscription_destroy`.
  - That span includes the live phase after EOSE. This is intentional: the shipped test checks that a live replay at `T` is suppressed.
  - Freeing the set at the next refire is correct, because the new `since` equals the new cursor, so older seconds cannot be replayed.
- **No leak at hand-off:** `replay_boundary_events` is always NULL while the cursor equals the boundary.
  - `prepare_refire` clears it and sets boundary = cursor.
  - Events at the boundary second go to the cursor list while the two are equal.
  - The cursor never decreases.
- **No NULL write:** whenever a node is allocated, the list pointer is non-NULL. It is either the cursor or boundary list, or it is reassigned after the advance.
- **Locking:** all new state is guarded by `sub_mutex`. No new locking order is introduced.
- **Memory:** the worst case is now two seconds' worth of IDs per subscription (cursor second plus boundary second). This stays within already-filed `nostrc-qp24.4.2` (F2) and is not a new blocker.

### First-EOSE semantics (inspected)

- The initial EOSE, and later EOSEs while the channel has capacity, are queued. A full channel coalesces later phase notifications rather than guaranteeing one per REQ.
  - Nothing else sends on this channel.
  - Its capacity is at least 1: 8 by default, and the `NOSTR_SUB_EOSE_CAP` override only accepts values greater than 0.
  - So on a fresh or drained channel, `try_send` cannot fail unless the channel is closed.
- `try_send` wakes select waiters. The ordered and newest-first cases receive EOSE through `go_select_timeout`, which shows this.
- **Behaviour after eight undrained signals:** further reconnect EOSEs are dropped (coalesced), and the relay reader stays live.
  - A consumer that drains lazily may see fewer EOSEs than REQ phases, or a stale queued one. This is documented.
  - The in-repo long-lived consumers (the `GNostrSubscription` monitor and the streaming pools) drain promptly, so it does not affect them.

### Verification (scratch tree at `8a2d8fd3` under `/tmp`; worktree untouched)

| Check | Result |
| --- | --- |
| `test_reconnect_same_second`: ordered, newest-first, and nine-disconnect EOSE pressure cases | pass, and 10/10 with `--repeat until-fail:10` (about 11 s per run) |
| Mutant M1: blocking EOSE send restored | **fails**: reader wedged at `requests=9`, the event barrier is never delivered |
| Mutant M2: boundary hand-off replaced with a free | **fails** in the newest-first case: the replayed boundary ID is delivered again |
| Scratch probe: multi-second newest-first replay (`T+2, T+1, T(new), T(replay)`) plus a live replay after EOSE | pass 3/3; no duplicate |
| `libnostr_*`, `groundhog*`, MockRelay, `test_relay*`, `test_pool_redial`, `test_subscription_*`, `concurrency_subscription_shutdown`, `test_nostr_gobject_*` (45 tests) | all pass (`groundhog-launch` skips on macOS as before) |

**Pressure-case timing margin:** after each successful reconnect, backoff resets to 1 s ± 50%, so nine disconnects take at most about 13.5 s. The test waits up to 30 s and CTest allows 60 s.

### Residual (non-blocking)

- **R1:** the delta commit message states no version decision. No bump is needed: libnostr is already at 1.0.1 (PATCH), unreleased in this same push, and the delta is a backward-compatible fix that documents the EOSE coalescing.
- **R2:** the `nostr_subscription_is_eosed()` doc ("whether EOSE has been observed") still reads as monotonic, but it now resets on every reconnect. This doc-only nit was corrected by the coordinator after this review; no implementation behavior changed.
