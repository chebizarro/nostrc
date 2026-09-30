# Peer Review — W21 MLS prerequisites: lossless ceiling, backfill paging, oldest-first MLS backfill

**Branch:** `groundhog/w21-mls-prereqs`, 3 commits on `b5ca25bf`, reviewed on `review/w21-mls-prereqs` at `721c5be1`:

| Commit | Bead | Subject |
|---|---|---|
| `d02c5e81` | nostrc-5rfp | fix(nostr-gobject,groundhog): lossless GNostrSubscription has a hard backlog ceiling |
| `e4514f10` | nostrc-cpwf | fix(groundhog): page a relay's backfill past its result cap; MLS applies it oldest first |
| `721c5be1` | nostrc-kzun | test(groundhog): an MLS backlog over 200 events arrives complete; fix held-event ordering it exposed |

**Reviewer:** independent peer reviewer (AGENTS.md, "Peer Review (REQUIRED)")
**Date:** 2026-09-30
**Verdict:** **REQUEST CHANGES.** There are four blocking findings, all in `e4514f10`, and two of them are reproduced with scratch tests:
- B1: NIP-29 cursor passes an incomplete gap.
- B2: a false "incomplete" permanently stalls the cursor.
- B3: the oldest-first order breaks across relays.
- B4: MLS backfill buffering is unbounded.

`d02c5e81` and `721c5be1` are correct as they stand.

This review changed no source files and no beads. Mutations, probes and build trees live under `/tmp` and were reverted; `git status` is clean.

---

## Verification performed

| # | What | Result |
|---|---|---|
| 1 | macOS: `cmake -S . -B /tmp/w21pr -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w21pr` | Build clean. The build rewrote `apps/gnostr/data/ui/dialogs/gnostr-profile-edit.ui`; it was restored |
| 2 | `ctest --test-dir /tmp/w21pr -j6 --timeout 300` | **437/437 passed**, 5 skipped (`test_nip5f_tcp`, `groundhog-launch`, `groundhog-store-key-keyring`, `groundhog-background-gui`, `groundhog-notifier-gui`) |
| 3 | `scripts/linux-gate.sh .` (arm64, Ubuntu 24.04, GCC) | Built all targets; **smoke tests passed, 422 run** |
| 4 | `python3 scripts/check-unsequenced-args.py` | "No call modifies and uses a variable in different arguments" (exit 0) |
| 5 | macOS ASan+UBSan (`-DGNOSTR_ENABLE_ASAN=ON`, `detect_leaks=0`): `groundhog-relay`, `groundhog-relay-wire`, `groundhog-mls-service` (20/20 subtests), `groundhog-nip29-service`, `groundhog-dm-inbox`, `test_nostr_gobject_subscription_eose_order` | **All pass, no reports.** I did not run Linux ASan: the gate has no sanitizer mode, and a second container build was not worth it for code that is platform-neutral. `-DSANITIZE=address,undefined` alone fails to build on macOS (`_FORTIFY_SOURCE` redefined, because `GO_ENABLE_ASAN` stays off). That is pre-existing and unrelated |
| 6 | **Mutation A** (`721c5be1`): `retry_held()` goes through the whole pass (`pass_ends` ignored) | `catch-up-over-200` **FAILS** ("every message of the backlog did not happen within 90 s"); `catch-up-4-commits` passes, as the author says |
| 7 | **Mutation B** (`e4514f10`): MLS applies backfill events as they arrive (`keep_backfill` disabled) | `catch-up-past-relay-cap` **FAILS** (backlog never completes) |
| 8 | **Mutation C** (`e4514f10`): MLS scope without `gh_relay_scope_set_backfill_paging()` | `catch-up-past-relay-cap` **FAILS** |
| 9 | **Mutation D** (`d02c5e81`): the ceiling check disabled (`if (0 && self->lossless && …)`) | `lossless-flood-event-ceiling` and `lossless-flood-byte-ceiling` **FAIL** (`overflowed` never set); `lossless-backfill-at-ceiling` passes |
| 10 | **Mutation E** (`e4514f10`): DM inbox back to `events < self->limit` | `backfill-relay-cap-below-limit` **FAILS** |
| 11 | **Probe P1** (B2), scratch test in `test_relay_scope.c`: `since` 1000, paging 500×64. The relay holds 5 events at t=6000 and 25 at t=5000 and answers each REQ honestly | All 30 delivered, then **EOSE with `incomplete = TRUE`** after 2 pages. The relay sent everything it holds |
| 12 | **Probe P2** (B1), scratch test in `test_nip29_service.c`: 30 messages in one second behind a 25 cap (genuinely incomplete), then one live message | Service logs "could not fetch every older message … its read cursor stays where it was". After the live message, the next REQ has **`since` = 1790743071 > burst second 1790738640**: the 5 unfetched messages are never asked for again |

The author's "fails without the fix" claims all hold (rows 6–10).

---

## Findings summary

| ID | Severity | Blocking | Location | Summary |
|---|---|---|---|---|
| B1 | **High** | **Yes** | `gh-nip29-service.c:1018`, `:1148` | After an `incomplete` EOSE, the next live message moves the room cursor past the unfetched stretch (P2) |
| B2 | **Medium-High** | **Yes** | `gh-relay-scope.c:542-560` (`judge_filter`) | ≥20 events in the oldest second of the window give a false `incomplete` (P1). For MLS, whose cursor then never moves, this is a permanent stall that also re-pages a growing backlog on every subscribe |
| B3 | **Medium-High** | **Yes** | `gh-mls-service.c:1142` (`flush_backfill`) | Scope dedup is shared by all group relays, but each relay's EOSE flushes only the events that relay delivered first. With ≥2 relays the backlog is applied in interleaved chunks, not oldest first, which breaks the 32-key window the fix exists for |
| B4 | **Medium** | **Yes** | `gh-mls-service.c:1165-1177` (`keep_backfill`) | The MLS backfill buffer has no bound. A relay that ignores `limit`, never sends a page's EOSE, or streams live events during paging grows it without limit. This reopens w20 M1 one layer up |
| L1 | Low | No | `gh-relay-scope.c:571-620` (`start_page`) | Every page is a new WebSocket (plus TLS, Tor stream, AUTH): up to 64 sequential handshakes per relay, group and round |
| L2 | Low | No | `nostr_subscription.c:398` (`queue_closed_for_main`) | An overflow while draining ahead of a relay's CLOSED replaces the relay's reason (for example `auth-required:`) with the overflow reason. It heals itself through the overflow retry |
| I1 | Info | No | `start_page`, commit message | "Same AUTH identity" is exact for account AUTH (NIP-29). For MLS's ephemeral AUTH, each page connection signs with a **fresh** ephemeral key (`auth/ephemeral-fresh-per-connection`). Privacy-neutral |
| I2 | Info | No | `gh_relay_page_threshold()` | Relays that cap below 20 are never paged. This is a documented limitation |
| I3 | Info | No | paging design | A page that never answers holds the URL's EOSE forever. There is no timeout, which is correct under the protocol lens, but it combines with B4 |

---

## B1 — NIP-29: live events move the cursor past an incomplete backfill (blocking)

- **How the cursor moves.** `room_advance_cursor()` (`gh-nip29-service.c:1010`) moves `room->cursor` at once for a live event (`backfill == FALSE`), unless `room->sync_failed` is set.
- **What the new EOSE branch does.** On `update->incomplete` (`:1148`) it only logs, and it does not set `sync_failed`. The header says "an EOSE whose paging could not fetch everything moves no cursor", and that holds at the EOSE itself.
- **What goes wrong.** The first live message afterwards jumps the cursor to *now*. The next REQ's `since` (cursor − 600) lies past the missing stretch, so those messages are never requested again. The log message "its read cursor stays where it was" is also wrong.
- **Reproduced** by P2 (row 12).

**Fix:** on `update->incomplete`, set `room->sync_failed = TRUE` for the rooms of that relay's REQ. `relay_resubscribe()` already resets it per REQ (`:888`). Add P2 as a regression test.

## B2 — a false `incomplete` when ≥20 events share the oldest second (blocking)

`judge_filter()` steps over a second, and marks the answer incomplete, whenever a page has ≥ `gh_relay_page_threshold()` (20) events and none older than its `until`.

That condition also holds when the relay has **no older events at all** and ≥20 events in the oldest second of `[since, …]`. The page `{until: T}` then returns exactly the k ≥ 20 events at T that were already received, and nothing older exists. The rule can't tell "cut at the relay's cap" from "that's all there is".

**Consequences:**
- **MLS.** `settled` becomes 4, so `all_relays_answered()` is FALSE and the cursor never moves, not even for live events. The next subscription has the same `since` (cursor − 600), so the same burst is again the oldest second, and it is incomplete again: a **permanent stall** for that group. Every subscribe re-pages the whole growing backlog since the stuck cursor, until the 64-page budget runs out, which is also incomplete. This also feeds B4.
- **NIP-29.** B1 currently hides it. Once B1 is fixed, NIP-29 stalls the same way.
- **DM inbox.** The same rule, but gift-wrap timestamps are randomized, so it is negligible there.
- **Trigger.** 20 group events in one second, for example a bot, a bridge import or a burst of Commits and messages. It is rare, but once it lands at the window's start it never clears.

**Fix, cheap and sound.** Keep `max_count`, the largest answer count this relay has returned for the filter in this round. It is a lower bound on the relay's cap.
- A page with `count < max_count` was not cut, so it is complete: close the filter and don't step.
- Step over, and mark incomplete, only when `count >= max_count`.

In P1 the live answer had 30 events and the step page 25. With `25 < 30` the filter closes complete. A relay genuinely capped at C with more than C events in one second still returns C events on every page, so it is still stepped and still marked incomplete. Add P1 as a regression test.

## B3 — oldest-first breaks across relays (blocking)

- **The partition.** One GhRelayScope covers all group relays, and `deliver_event()` deduplicates by id **scope-wide**. Each event is therefore delivered, and so `keep_backfill`ed, under whichever relay sent it *first*.
- **The flush.** `flush_backfill(url)` applies only that URL's share.
- **What goes wrong.** Two group relays holding the same backlog and answering at similar speed split it into an arbitrary interleaving. At the first relay's complete EOSE, its share is applied oldest first, but with gaps. In libmarmot the keys of skipped generations survive only within 32 generations of the newest one read (`mls_key_schedule.h:106-114`). When the second relay flushes, a sender's older messages in its share lie outside that window and fail closed.
- **Why the tests miss it.** They use one relay, where the per-URL partition is the whole set.

**Fix:** since dedup already makes the per-URL split arbitrary, flush **all** stored events, from every URL and sorted oldest first, at any relay's EOSE or failure.
- A relay that has completed paging has delivered every event it holds, or seen it as a duplicate of one already in the queue. The union is therefore a complete, correctly ordered window.
- Consider a two-relay variant of `catch-up-past-relay-cap` with interleaved delivery.

## B4 — MLS backfill buffer has no bound (blocking)

**What goes into `group->backfill`.** Everything flagged `backfill`, and the scope flags everything before the URL's final EOSE, including live events during paging. Nothing caps it:
- **Limits are advisory.** A relay may ignore `limit`, so a page can deliver any number of events. The scope counts them and keeps paging.
- **No EOSE, no timeout.** A relay that never sends a page's EOSE (or the live REQ's) keeps the URL in backfill forever. By design there is no timeout, and every live event is buffered meanwhile.
- **Junk is cheap.** Anyone can sign kind 445 with the group's public `#h`, so junk costs a hostile relay nothing.
- **Dedup doesn't limit it.** The scope's seen set evicts after 4,096 ids and bounds nothing.

**Before and after:**
- **Before this branch**, such events went straight to `process_event()`: rejected, or held (bounded at 256 with junk eviction).
- **Now** they accumulate without limit. The nostr-gobject ceiling from `d02c5e81` bounds only the main-loop *lag*, not what a consumer keeps.

**Even honest relays** can fill it to 500 × 65 = 32,500 events per relay per group per round, all groups subscribing at once.

**To the question asked** ("64×500 buffered per group; is that OK?"): honest relays reach roughly 32 k events (tens of MB) per relay per group. That would be tolerable if it were enforced, but it is not enforced.

**Fix:** a per-group (or per-URL) cap on stored events and bytes, for example `(max_pages + 1) × page_limit` plus the held bound. When it's hit, flush what's stored oldest first, stop buffering for that URL, and treat the URL as answered-incomplete (settled 4), so the cursor holds. The scope could also stop a page (and mark the round incomplete) once a page exceeds its `limit` by some margin.

Encrypted groups are behind `GH_FEATURE_ENCRYPTED_GROUPS=0`, so none of this is user-visible yet. It must land before that flag flips, and the fix is small.

---

## d02c5e81 — lossless ceiling (nostrc-5rfp): correct

- **The check.** It runs under `event_queue_mutex` in `queue_item_for_main()`, before an event is queued.
  - `queued_events >= max` gives "exactly at the ceiling completes". This is tested with 1,000 of 1,000.
  - `queued_bytes + bytes > max_bytes` is the byte ceiling.
  - Byte accounting is symmetric: added on queue, subtracted on drain and on bounded-mode drop.
- **After an overflow.** `overflowed` is sticky. Items later in the same drain are freed, the overflow CLOSED is the last queued item, and it is emitted after every event queued before it. `get_overflowed()` tells it apart from a relay's CLOSED.
- **CLOSE is sent once.** `core_close_sent` (atomic CAS) is shared by the monitor's overflow path and `gnostr_subscription_close()`. The monitor still holds its own ref on `self`, so `sub` is live while it sends. No CLOSE is sent when the overflow happened while draining ahead of a relay's CLOSED (the relay already ended it), which is correct.
- **Bounded mode.** `GNostrPool` and Gnostr are unaffected: both pool call sites (`nostr_pool.c:1196`, `:1372`) set `lossless = FALSE`, and nothing in `apps/gnostr` or `nostr-gtk` creates a `GNostrSubscription` directly. The ceiling branch requires `self->lossless`, and the bounded drop path only gained byte bookkeeping. **Gnostr behaviour is unchanged.**
- **Groundhog.** `gh-relay-gnostr.c:68` maps the overflow to `GH_RELAY_CLOSED_OVERFLOW_PREFIX`. `maybe_recover()` re-issues the REQ from an idle at most **once per connection** (`GH_RELAY_SCOPE_OVERFLOW_RETRIES`). It resets on DISCONNECTED and never fires for a relay's own CLOSED.
- **Can a hostile relay loop us?** Per connection it can cause at most 2 REQs and 2 × 100 k events (or 64 MiB). To reset the counter it must drop the connection, and reconnects go through libnostr's exponential backoff with jitter (`relay.c:1156`). That backoff resets after a successful connect, so the loop rate is the base backoff: bounded and slow. Acceptable.
- **L2 (non-blocking).** If the ceiling is hit while `queue_closed_for_main()` drains the events ahead of a relay's CLOSED, the relay's reason (for example `auth-required:`) is replaced by the overflow reason. Groundhog then retries without AUTH, gets `auth-required` again and authenticates, so it heals itself at the cost of one extra REQ.
- **Versions.** nostr-gobject 2.2.0 (CMake and meson agree, MINOR for new API), groundhog stays at unreleased 0.11.1, and gnostr has no bump. These match `VERSION_MANIFEST.md`.

## e4514f10 — backfill paging (nostrc-cpwf): sound design, four defects

**Correct:**
- **`until` boundary.** `until` is inclusive, and the next page repeats the boundary second (the relay may have cut inside it). Answers are counted **before** dedup (per relay), and the repeats are deduplicated by id in the scope. There is no off-by-one at the boundary. The only defect is the false "stepped over" in B2.
- **Dedup across pages and the live REQ.** It goes through the one scope-wide `seen` set, so the live REQ's events and page events never reach a consumer twice. The seen set evicts after 4,096 ids, and MLS duplicates are then caught by libmarmot's processed markers and NIP-29's store. That is fine.
- **Cursor safety in the scope.** The URL's EOSE is withheld until paging ends, and every event before it is flagged backfill, live ones included. `incomplete` is set on:
  - a failed page (CLOSED, ERROR or DISCONNECTED);
  - a same-second step;
  - an exhausted budget;
  - a page that could not start.

  A relay CLOSED on the live REQ before EOSE, a disconnect, an overflow retry or an AUTH retry calls `reset_paging()`: the page is cancelled and the next REQ starts a new round. MLS gates the cursor on `settled == 1` for every relay (`all_relays_answered()`), for live events too, which is correct. NIP-29 is correct at EOSE but not afterwards (**B1**).
- **Reentrancy.** Answers are judged in `settle_now()` from an idle, never inside a transport callback. `emit_update()` refs the scope around consumer callbacks, so a consumer that cancels and rebuilds the parent scope from inside a page event (NIP-29 does) cannot free the page or endpoint underneath. ASan is clean.
- **Privacy.** A page uses the same URL and filter, plus `until`, and goes to no other relay.
  - **Same Tor isolation label**, so the same circuit, same exit and same stream isolation as the live REQ.
  - **AUTH.** The same signer and per-URL auth mode. NIP-29 signs as the account, as the live REQ does. For MLS, a fresh ephemeral key per connection (I1).
  - **No new exposure.** The relay already knows the `#h` and the client, and the pages are linkable to the live REQ by circuit and filter anyway. Paging reveals no more than `since` already does: how long the client was away.
  - **Bounded.** One page at a time, at most 64 (MLS) or 16 (NIP-29) per relay per round. Rounds restart only on disconnect, overflow retry or AUTH retry, each bounded per connection as above.
- **Nostr protocol lens.** Backfill is EOSE-aware, and nothing polls: pages are driven by EOSE, not timers. CLOSED is handled on the live REQ and on pages, and the page's own AUTH/OK stay inside the page scope. Nothing is closed on a timeout. Pages are one-shot and cancelled (CLOSE) once judged, while the live REQ stays open. Filters with their own `limit` ("newest N") are left alone.
- **L1.** One new WebSocket per page. A single page connection reused for successive REQs (NIP-01 allows several subscriptions per connection) would avoid up to 64 TLS, Tor and AUTH handshakes per relay and group, and would look less like a connection flood to relays that rate-limit connects.
- **DM inbox.** The threshold change (`< gh_relay_page_threshold(limit)`) fixes strfry's 500-vs-1000 case (Mutation E). The B2 ambiguity applies in principle, but randomized wrap timestamps make it negligible.

**Defects:** B1, B2, B3, B4 above.

## 721c5be1 — held-event ordering (nostrc-kzun): correct

- **The old failure.** A pass used to continue after applying a Commit. New-epoch messages that sorted *before* the Commit (same second, relay order) were re-held, while that epoch's later seconds were read in the same pass and pushed the sender's ratchet more than 32 generations past them.
- **The fix.** A pass now ends at the Commit's **second**: the rest of that second is still tried, because it may hold the closing epoch's messages. The next pass starts oldest first again.
- **Termination.** Each repeat is triggered by a Commit applied, and Commits are consumed, so the fixpoint still terminates.
- **Pinned from both sides.** Mutation A fails `catch-up-over-200`. The author's other side, where ending at the Commit itself fails `catch-up-4-commits`, is consistent with the code comment.
- **Out-of-order relays.** Both the held queue and the new stored backfill sort by `created_at`, so a relay that doesn't answer newest first is handled across seconds. Within one second the order is a heuristic (reverse arrival for stored events, arrival for held ones). That is only harmful beyond 32 same-second messages from one sender, which the retry pass partly absorbs. The heuristic is acceptable, and it is documented.
- **The tests.** `catch-up-over-200` (251 events in one stored answer) and the reworked `junk-does-not-evict` (300 in one burst) exercise the lossless path. Both fail under a bounded subscription, per the author, and `catch-up-over-200` fails under Mutation A here.

---

## Recommendations (in order)

1. **B1:** set `sync_failed` for the relay's rooms on an incomplete EOSE, and add P2 as a test.
2. **B2:** judge "cut short" against the largest count this relay has returned in the round, not the fixed threshold, and add P1 as a test. Re-check the DM inbox's `finish_page()` against the same rule.
3. **B3:** flush the whole stored backfill (all URLs), sorted, at any relay's EOSE or failure, and add a two-relay interleaved catch-up test.
4. **B4:** bound the stored backfill in events and bytes, and treat overflow as answered-incomplete. Optionally, abandon a page that returns far more than its `limit`.
5. **L1** (follow-up bead): reuse one page connection per round.
6. **L2** (follow-up bead, optional): when a relay CLOSED is being drained, keep its reason if the overflow happened only among the events ahead of it.

## Verdict

**REQUEST CHANGES.**
- **Accepted as they stand:** the lossless ceiling (`d02c5e81`) and the held-event pass rule (`721c5be1`). They are correct, and their tests are shown to fail without them.
- **The paging design** in `e4514f10` is sound on the protocol and privacy axes: EOSE-driven, bounded, no polling, no timeout closes, same circuit and AUTH mode.
- **Blocking before merge:**
  - **B1:** NIP-29 cursor passes an incomplete gap (reproduced).
  - **B2:** a false `incomplete` becomes a permanent MLS cursor stall (reproduced).
  - **B3:** oldest-first fails with more than one relay.
  - **B4:** unbounded MLS backfill buffering.

---

# Final pass — review fixes (2026-09-30)

**Branch:** `groundhog/w21-mls-prereqs`, rebased onto master. The review branch was moved to its tip `d4ffa896`, and this review commit was cherry-picked on top. `git range-diff` shows the original three commits unchanged by the rebase (`782e78f9`, `b86e0539`, `aa22e608`), apart from VERSION_MANIFEST context. The new commits:

| Commit | Addresses | Subject |
|---|---|---|
| `ab09848e` | B1 | an incomplete NIP-29 backfill holds the room cursor for the whole REQ |
| `15e16745` | B2 | a relay's own cap is judged from its answers, not a fixed count |
| `8dd07dcb` | B3 | MLS applies the backfill of all group relays as one oldest-first set |
| `c2182e05` | B4 | bound the MLS backfill store; a page that ignores its limit fails |
| `d4ffa896` | — | CMake policy fix for the preview-guard script test (already on master) |

**Verdict of the final pass:** **REQUEST CHANGES.** B1 and B4 are fixed, and B2 and B3 are fixed for the cases I reported. The fixes bring two new blocking defects, both reproduced:
- **N1:** one stalled relay freezes the whole group, live messages included.
- **N2:** the B2 rule can now declare a cut answer *complete*.

## Verification performed (final pass)

| # | What | Result |
|---|---|---|
| F1 | macOS build at `d4ffa896` + `ctest -j6 --timeout 300` | Build clean. **447/447 passed**, 5 skipped (the same 5 as before) |
| F2 | `scripts/linux-gate.sh .` (arm64) | Built all targets; **smoke tests passed, 438 run** |
| F3 | `python3 scripts/check-unsequenced-args.py` | Clean (exit 0) |
| F4 | macOS ASan+UBSan (`-DGNOSTR_ENABLE_ASAN=ON`, `detect_leaks=0`): `groundhog-relay`, `-relay-wire`, `-mls-service` (with `catch-up-two-relays-partial`, `backfill-store-bounded`), `-nip29-service`, `-dm-inbox`, `subscription_eose_order` | **6/6 pass, no reports** |
| F5 | **P1 rerun verbatim** (5 events at 6000, 25 at 5000, all sent) | **Fixed:** one page, EOSE complete, 30 events |
| F6 | **P2 rerun verbatim** (NIP-29, 30 in one second behind a 25 cap, then a live message) | **Fixed:** the next REQ's `since` (1790738510) is before the burst second (1790742723), so the unfetched messages are asked for again |
| F7 | **P3, new** (`test_relay_scope.c` scratch): a relay capped at 20 holds 60 stored events at 3001–3060; one live event (t=9000) arrives before the live REQ's EOSE; every answer honest | **EOSE complete with 40 of 61 events.** The 21 oldest are never fetched, and a consumer moves its cursor past them (N2) |
| F8 | P3 with the rule narrowed as recommended under N2 (local patch, reverted) | P1, P3 and all 14 `groundhog-relay` tests pass, including the author's `paging/same-second` and `paging/overrun-and-end` |
| F9 | **P4, new** (`test_mls_service.c` scratch plus a `stall_pages` knob in `wire-relay.h`, both reverted): a group on relays g and h; Bob misses 60 messages; h serves all but the newest; g (capped at 50) is the only one serving the newest, then never answers an `until` page | **h's complete backlog is not shown within 90 s.** In a variant, a live message Alice sends after g's page is out (delivered by h) **is not shown within 90 s** either. The control (same setup, g answers its pages) passes: backlog and live message read (N1) |

All scratch changes are reverted; `git status` is clean.

## B1 — fixed

An incomplete EOSE now sets `room->sync_failed`, so the cursor holds for the rest of that REQ, live messages included. `relay_resubscribe()` clears it for the next REQ. P2 passes, and the author's `incomplete-backfill-holds-cursor` pins it.
- **Side effect, acceptable.** A reconnect inside the same scope, whose next round completes, still holds the cursor until the next resubscribe (or restart). That matches how a failed admission already behaves. It costs a re-fetch, never a loss.

## B2 — fixed for P1, but the rule now errs toward loss (N2, blocking)

The fix remembers `max_count`, the largest answer this round, and treats any answer with `count < max_count` as "not cut", so the filter closes complete. That is the rule I recommended. Applied to **every** answer, it has a failure mode I missed: an answer's count can exceed the relay's real per-query cap.
- **Live events before EOSE.** Some relays register the live listener before running the stored query and send EOSE after it (khatru does, and relay29 is built on it). A live event arriving in between is counted in the live answer: cap + 1.
- **Variable caps.** Relays that cut answers by time or byte budget cap at a different count each time.

A later page that is genuinely cut at the relay's cap then has `count < max_count`, and the filter closes as **complete**. The EOSE is complete, the MLS cursor saves (settled 1), and the NIP-29 cursor commits. The unfetched older events are passed silently (P3: 21 of 61 lost). The old rule erred toward "incomplete", a re-fetch; this one errs toward loss. `GhDmInbox::finish_page()` has the same rule (`events < most`, `run_max` seeded from `round_events`, which counts live events that arrive before EOSE).

**Fix (verified locally, F8):** use `max_count` only for a page that made **no progress**, the case B2 was about. A page whose oldest event is older than its `until` keeps the old threshold rule and pages on.
```c
gboolean stuck = p->oldest >= p->until;
gboolean cut = p->count >= gh_relay_page_threshold(scope->page_limit) &&
               (!stuck || p->count >= p->max_count);
```
- **What remains.** A false complete now needs more than the relay's cap in one second *and* an inflated earlier answer. Optionally clamp `max_count` to `page_limit` as well.
- **DM inbox.** Apply the same narrowing to `finish_page()`.
- **Test.** Add P3 as a regression test.

## B3 — fixed; the deviation is sound, but the wait needs a bound (N1, blocking)

**The deviation, "flush once no relay is mid-backfill" instead of "at any relay's EOSE", is correct.** My any-EOSE suggestion assumed every relay holds the whole window. The author's `catch-up-two-relays-partial` (a third of the events only on g, a third only on h) shows it does not, and the test fails 5/5 with the any-EOSE rule. Letting a relay that has sent nothing hold nothing up is a reasonable way to keep silent relays from blocking.
- **Residual race (Low, L3).** A relay that starts late, for example still in its AUTH round trip when the others finish, and holds unique older events will apply them after newer ones.

**N1 — a stalled relay freezes the group (blocking).** A relay that has delivered one backfill event first and then stalls holds the whole flush:
- **How it stalls.** Its live REQ or a page never gets EOSE: the relay drops the REQ silently (some relays ignore REQs over a rate or concurrency limit without CLOSED), a query hangs, or a relay does it on purpose.
- **What is held.** Everything, including **live events from healthy relays**, because the store takes every event while `backfilling` is non-empty. Nothing releases it except the B4 cap (32,500 events or 64 MiB). A normal group never reaches that, so **the group shows nothing new indefinitely** (P4).
- **Blast radius.** One group relay out of N is enough, it needs no misbehaviour beyond silence, and the scope correctly has no timeouts, so nothing else ends it.

**Is it acceptable?** No, and it needs a bound. The bound does not need a timeout-driven close, so it doesn't conflict with the protocol lens:
- **The rule.** Once every *other* relay has ended its round (or has sent nothing), a relay still in `backfilling` that has delivered **nothing new for a quiet period** (for example 30 s on the service's `GhClock`) is given up exactly as `backfill_full()` does it for that URL.
  - Call `gh_relay_scope_end_backfill()`.
  - Mark it settled 4, so it is not answered, the cursor holds, and `history-incomplete` is set.
  - Flush the store oldest first.
- **Why this is not a timeout close.** The relay's subscription stays open and is never treated as answered. Its later events are applied live. Only the local wait for ordering is bounded.
- **Test.** P4 is the regression test: the live message must be read within the bound.

## B4 — fixed; the deviation is acceptable

- **The bound.** 65 × 500 events or 64 MiB per group, checked on every stored event.
- **When it trips.** `backfill_full()` ends the round for the relays still delivering (`gh_relay_scope_end_backfill()`, settled 4, cursor held, `history-incomplete`) and applies the store oldest first. Later events of those connections are live.
- **Overrun guard.** A page that delivers more than 2× its limit per filter now fails (incomplete) and its excess is dropped.
- **Reentrancy.** `end_backfill` can run inside the page's own event callback: a page event goes to `deliver_event`, then MLS `keep_backfill`, then `backfill_full`, which cancels the page. `emit_update()` holds refs on both the page and the parent scope, and GNostrRelay's `close_relay()` only disconnects handlers and drops refs, with nothing touching `handle` after `on_event` returns. ASan is clean on the tests that trip the bound.
- **The deviation (on overflow, the store keeps the newest part of the backlog, so some older messages can end up outside libmarmot's 32-key window) is acceptable.**
  - The bound is sized to one honest round's page budget, and exceeding it already means the round is incomplete.
  - The limitation is documented at `backfill_full()` and in the header, and it is surfaced through `history-incomplete`.
  - The cursor holds, so nothing is passed silently.
- **L4 (Low).** The bound is per group across relays, but the scope's `seen` set evicts after 4,096 ids. With two relays at different speeds and a large backlog (more than about 16 k events), the slow relay's copies are re-delivered and stored twice, so the cap trips at about half an honest round. Deduplicate by id in the store, or scale the cap by relay count.

## New findings summary (final pass)

| ID | Severity | Blocking | Location | Summary |
|---|---|---|---|---|
| N1 | **High** | **Yes** | `gh-mls-service.c` `on_group_update` / `backfilling` (8dd07dcb) | A relay that delivers one backfill event and then stalls holds every stored and live event of the group until the 32,500-event cap. The group shows nothing new (P4). It needs a quiet-period bound that gives the relay up as incomplete, without closing anything |
| N2 | **High** | **Yes** | `gh-relay-scope.c` `judge_filter`; `gh-dm-inbox.c` `finish_page` (15e16745) | `count < max_count` on a page that made progress closes a genuinely cut answer as complete (live events before EOSE, variable caps). The cursor passes unfetched events (P3). Restrict the rule to no-progress pages |
| L3 | Low | No | `8dd07dcb` | A relay that starts late (for example mid-AUTH) with unique older events is applied after the others' flush. This is the documented tradeoff of "sent nothing holds nothing up" |
| L4 | Low | No | `c2182e05` | Seen-set eviction (4,096) re-delivers a slow relay's copies, which count toward the per-group cap twice |

Findings L1 and L2 of the first pass remain open as non-blocking follow-ups.

## Final verdict

**REQUEST CHANGES.**
- **Fixed:** B1 (P2 passes); B4, including the documented newest-part deviation, which is acceptable.
- **Fixed for the reported cases:** B2 (P1 passes) and B3 (the all-relays deviation is correct).
- **Still blocking:**
  - **N1:** a stalled relay must not freeze a group's messages. Bound the wait with a quiet period that gives the relay up as incomplete, and add P4.
  - **N2:** restrict the largest-answer rule to pages that made no progress, in both the scope and the DM inbox, and add P3.

Both fixes are small. With them, and P3 and P4 as regression tests, I expect to approve.

---

# Closing pass — N1/N2 fixes (2026-09-30)

**Branch:** `groundhog/w21-mls-prereqs` at `82f6afdf`: `d4ffa896` plus two commits. The review commits were rebased onto it.

| Commit | Addresses | Subject |
|---|---|---|
| `07326e84` | N2 | the largest-answer rule applies only to a page stuck in one second |
| `82f6afdf` | N1 | a silent group relay stops holding the MLS backfill after a quiet period |

**Verdict:** **APPROVED.**
- **Both blockers fixed.** N1 and N2 are fixed, and my repros pass.
- **No regressions.** None in any gate.
- **Low findings tracked.** All four have beads.
- **One Medium follow-up (M1)**, a residual of N1 in busy groups (P5). It must land before `GH_FEATURE_ENCRYPTED_GROUPS` is flipped, but it does not block this merge: encrypted groups ship disabled, and M1 is a bounded delay, not a loss.

## Verification performed (closing pass)

| # | What | Result |
|---|---|---|
| C1 | macOS build at `82f6afdf` + `ctest -j6 --timeout 300` | Build clean. **447/447 passed**, the same 5 skipped |
| C2 | `scripts/linux-gate.sh .` (arm64) | **Smoke tests passed, 438 run, after one rerun.** Three tests failed only in the parallel run and passed alone: `test_nostr_gtk_bind_latency_budget`, `groundhog-conversation-info-gui`, and `groundhog-relay-wire /groundhog/relay/offline-at-start`. That last test predates this branch (`c04e2be9`, on master) and belongs to the parallel-load flake class tracked in **nostrc-xdg9**. It passes 5/5 alone here. None of the three touches the changed code paths |
| C3 | `python3 scripts/check-unsequenced-args.py` | Clean |
| C4 | macOS ASan+UBSan: `groundhog-relay`, `-relay-wire`, `-mls-service` (with `stalled-relay-given-up`), `-nip29-service`, `-dm-inbox` (with `backfill-inflated-live-answer`), `subscription_eose_order` | **6/6 pass, no reports** |
| C5 | **P3 rerun (N2)**: a relay capped at 20 with 60 stored events and one live event before EOSE | **Fixed:** 3 pages, **61/61 events, EOSE complete** (it was 40/61, complete). All 15 `groundhog-relay` tests pass, including the author's `paging/live-before-eose` |
| C6 | **P4 rerun (N1)**: g the only relay with the newest message, its page stalled; default 30 s quiet period | **Fixed:** the live message sent during the stall and h's whole backlog are read after **30.0 s**, the quiet period. It used to be never (the probe hit its 90 s timeout) |
| C7 | **P5, new** (scratch, reverted): the same stall, quiet period 1 s, but g carries the group's live traffic (h withholds new events): one message every 300 ms for 8.3 s | **Nothing is read for the whole 8.3 s burst.** Everything is read **0.8 s after the burst stops** (M1) |
| C8 | 5× repeated runs: `stalled-relay-given-up`, `catch-up-two-relays-partial`, `backfill-inflated-live-answer` | 5/5 each |

All scratch changes are reverted; `git status` is clean.

## N2 — fixed

`judge_filter()` now applies the largest-answer rule only to a page that got no older than its `until` (`stuck`). A page that made progress keeps the threshold rule and pages on, and `max_count` is clamped to the page limit. This is exactly the narrowing recommended in the final pass, with the clamp added. `GhDmInbox::finish_page()` gets the same rule, with `run_max` clamped to the REQ limit (the variables `oldest`/`until` are the page's own).
- **P1 (B2) passes**, still complete after one page.
- **P3 passes:** complete with all 61 events.
- **A genuine same-second overflow** (`paging/same-second`, `backfill-tied-second`) is still stepped over and marked incomplete.

## N1 — fixed; one residual (M1, Medium, non-blocking for merge)

`group->backfilling` now records when each relay last delivered a backfill event. A monotonic quiet timer (30 s, `gh_mls_service_set_backfill_quiet()`) gives up relays that are all silent for the period, as `backfill_full()` does: `end_backfill`, settled 4 (the cursor holds), `history-incomplete`, and the store applied oldest first. Nothing is closed.
- **Relays still delivering** keep the timer waiting, so a silent relay is given up only after the others finish.
- **Cleanup.** The timer is removed on unsubscribe and finalize.
- **Reentrancy.** `give_up_backfill()` runs under a group ref.
- **The protocol lens holds:** this is a bound on a local ordering wait, not a timeout-driven close.
- **Testability (Info).** The timer uses `g_timeout_add` and `g_get_monotonic_time()`, not the service's `GhClock`, so tests need a real short quiet period (1 s). Acceptable.

**M1: the quiet timer is refreshed by the stalled relay's live traffic.**
- **The cause.** The scope flags every event before a URL's final EOSE as `backfill`, including live events arriving on the still-open live REQ while that URL's page is stalled. Each one the stalled relay delivers *first* (scope-wide dedup) calls `touch_backfilling()` and restarts its quiet period.
- **The effect.** In a busy group whose stalled relay wins some of the live-delivery races, the group shows nothing new until there is a 30 s gap in that relay's first deliveries (or the store reaches its bound). P5 shows it: with a 1 s quiet period, nothing was read during an 8.3 s burst, and all of it was read 0.8 s after the burst ended.
- **What limits it.** It is a delay, not a loss: the cursor holds and nothing is skipped. The trigger is also narrow: a relay that drops or hangs its `until` pages while serving live events faster than the other group relays.
- **Why it still matters.** It freezes a group exactly when it is busy, so it should land before encrypted groups are enabled.

**Suggested fix:** count only stored-answer progress as "delivering". The scope knows which events are which (`endpoint->live_eose`). Events of the live REQ's stored answer (before `live_eose`) and page events are progress; events arriving on the live REQ after its own EOSE are live traffic. Expose that on `GhRelayUpdate` (for example `.stored`), and have `touch_backfilling()` refresh a relay's timestamp only for stored events, while those events still go to the store. Add P5 (a stalled relay carrying live traffic, a quiet period of 1 s, the first live message read within a few quiet periods) as its test.

## L1–L4 — tracked

The low findings are not addressed in code, and none of them blocks. Each has an open bead:
- **L1**, reuse one page connection: **nostrc-wdlz**.
- **L2**, a relay's CLOSED reason masked by the ceiling: **nostrc-5595** (related: nostrc-ue8f).
- **L3**, a late-starting relay applied after the flush: **nostrc-f7s7**.
- **L4**, the store deduplicated by id against seen-set eviction: **nostrc-jjyz**.

## Final verdict

**APPROVED.**
- **All first-pass and final-pass blockers are fixed** and pinned by tests that fail without them: B1–B4, N1 and N2.
- **The repros confirm it:** P1–P4 now pass (B1, B2, N1, N2).
- **The gates are clean:** full suite, Linux gate (one known parallel-load flake class), ASan+UBSan and the unsequenced-args check.
- **The design holds** on the protocol and privacy axes:
  - backfill is EOSE-driven, bounded, with no polling and no timeout-driven closes;
  - pages use the same circuit and AUTH mode;
  - cursors hold on every incomplete path.
- **Condition for enabling encrypted groups (not for this merge):** file and fix **M1** before `GH_FEATURE_ENCRYPTED_GROUPS` is flipped.
