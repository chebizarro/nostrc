# W20 review: NIP-29 refusal before group state (nostrc-kfso), loopback test listeners (nostrc-vzls)

- **Reviewer:** independent peer review (AGENTS.md), 2026-09-29
- **Branch:** `review/w20-nip29-listeners`, at the tip of `tests/w20-nip29-listeners`
- **Base:** `9c093e32` (the W19 reliability work). The requested range `8cc7c27e..HEAD` covers more than these two commits. The two commits under review are:

| Commit | Subject |
|---|---|
| `d2fce3a3` | fix(groundhog): a closed group's refusal before its state still turns CLOSED (nostrc-kfso) |
| `0723d69d` | test(groundhog): test listeners bind 127.0.0.1, never the wildcard (nostrc-vzls) |

**Verdict: APPROVED.** Nothing blocks the merge. The findings below are Low or lower.

---

## 1. What I ran (macOS 15.6 / Darwin 24.6, arm64)

| Check | Result |
|---|---|
| `git submodule update --init third_party/nostrdb third_party/nsync` | ok |
| `cmake -S . -B /tmp/w20kr -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w20kr` | builds, 2383 steps; `gnostr-profile-edit.ui` was rewritten and I restored it |
| `ctest --test-dir /tmp/w20kr -R 'groundhog-(nip29\|group\|net\|blossom\|relay)' -j6` | **11/11 pass** |
| `ctest -R 'groundhog-(nip29-service\|net\|blossom\|relay-wire\|group-ui\|group-ui-gui)$' -j6 --repeat until-fail:5` | 6/6 pass, 5 repeats each |
| `python3 scripts/check-unsequenced-args.py` | clean: "No call modifies and uses a variable in different arguments" |
| **A/B, kfso:** `gh-nip29-service.c` from `9c093e32` (unfixed) built with the new tests | `refused-before-state` fails **5/5** (`:946`, the rebuilt REQ no longer asks for `club`); `refused-state-after-eose` fails **4/10** (`:988`, CLOSED never comes) |
| Same tests, fixed service | `refused-before-state` 20/20, `refused-state-after-eose` 20/20, `denied-and-closed` 20/20 |
| **Mutation, vzls:** `/groundhog/relay/loopback-listener` with the service bound by `g_socket_listener_add_any_inet_port()` | fails **10/10** (`:406`, the port is one of the held ones); with `gh_test_listen_loopback()` it passes 20/20 |
| **Kernel probe, vzls** (Python: hold 16 explicit `127.0.0.1` ports after the next ephemeral one, then bind a listener with SO_REUSEADDR) | dual-stack `[::]:0`, V6ONLY=0: **298/300** landed on a held port, and the IPv4 dial reached the holder **298/298**. `127.0.0.1:0`: **0/300** |

Every temporary source swap was reverted with `git checkout`, and the binaries were rebuilt from the committed tree. `git status` was clean before this document was added.

---

## 2. nostrc-kfso: product fix

### 2.1 Mechanism: sound

The diagnosis holds. The OK frame (publish connection) and the 39000 (REQ connection) arrive on independent paths. The REQ's events reach the service only through GNostrSubscription's FIFO, drained at `G_PRIORITY_DEFAULT_IDLE` (`nostr-gobject/src/nostr_subscription.c:195-261`). Before the fix, a refusal handled first made the room DENIED, and `room_set_join()` then called `relay_resubscribe()` immediately because DENIED is not `join_subscribed()`. That cancelled the scope while the 39000 was still queued. After that, `relay_room()` ignored any later snapshot for a non-subscribed room.

The fix relies on one property: EOSE is delivered after every EVENT that preceded it on the wire. I checked this because the comment at `gh-nip29-service.c:1052` still says "the relay layer may report EOSE before the backfill it ends". That comment is **out of date**. Since nostrc-qp24.10.6, EVENT, EOSE and CLOSED share one FIFO, and `queue_eose_for_main()` drains `ch_events` before queueing the EOSE marker (`nostr_subscription.c:129-134, 290-307`). CLOSED also drains pending EOSEs and events first (`:309-318`). `gh_relay_scope_eose()` emits synchronously from that ordered callback. So when `on_scope_update()` sees EOSE for an awaiting room, that REQ's 39000 has already gone through `relay_event()` → `room_admit_snapshot()`. Dropping the room at that point is safe. (See L3 for the stale comment.)

Gating on `read == SYNCING || DISCONNECTED` exactly means "this REQ's EOSE has not been handled yet". If it has (LIVE), the FIFO guarantees the 39000 was admitted first, so `room->closed` is already known and the refusal goes straight to CLOSED. AUTH_REQUIRED and REFUSED mean no state is coming, so DENIED is right. `join_with_code` is excluded correctly: a refusal of a coded join never means CLOSED.

### 2.2 Privacy / what is asked for: no new exposure

- **Filters.** `relay_filters()` puts an awaiting room's group id only in the `#d` list of the 39000-39003 filter. Messages, 9005 deletions and the 9000/9001 `mine` filter are built from `joined` only. The `mine` filter was also moved from `groups[]` to `rooms`; that is equivalent for joined rooms. When only awaiting rooms remain, the function returns after the meta filter. `refused-before-state` asserts this on the wire: the rebuilt REQ mentions `"club"` exactly once.
- **Admission.** `relay_room(..., state_only=FALSE)` still requires `join_subscribed()`. A 9000/9001/9005 or chat message for an awaiting group is dropped even if a relay sends it unasked. Only 39000-39003 reach the room.
- **Contacts.** No relay other than the group's own is contacted. The 39000-39003 is public relay-signed state, and it was already part of the REQ the join opened. The NIP-11 fetch that `relay_ensure_key()` now counts for awaiting rooms goes to the same relay, and only when the room has no key. The only thing the relay learns is that the client is still connected, and it already has the account's signed join request.
- **Membership side effect.** A 39002 admitted while awaiting can list the account and move the room DENIED/CLOSED → MEMBER through `room_membership_from_members()`. This is not new behaviour: that function already handles DENIED and CLOSED (`:611-614`), and before the fix the outcome depended on arrival order. `room_set_join(MEMBER)` clears `awaiting_state` and triggers the full resubscribe (`!was_subscribed && join_subscribed`).

### 2.3 Lifetime: can it stay open indefinitely?

What ends `awaiting_state`:

| Event | Effect |
|---|---|
| EOSE of the REQ it is in (original or rebuilt) | cleared; one `relay_resubscribe()` without it |
| CLOSED of that REQ | cleared; no resubscribe (the REQ is gone) |
| Re-join (REQUESTING/MEMBER…) | cleared in `room_set_join()` |
| `gh_nip29_service_forget()` | cleared; resubscribe if it was awaiting. `forget()` refuses joined rooms (`:2069`), so `was_reading = room->awaiting_state` is the complete condition |
| Account switch / pause (`update_activity()`) | every scope is closed and key fetches are cancelled. While paused, `relay_resubscribe()` opens nothing. The flag survives, so on resume one REQ asks for the state again and its EOSE clears it. That is bounded and happens only for the same account |
| Restart | cleared (not persisted; see L2) |

**What does not end it (L1):** DISCONNECTED/ERROR leaves it set on purpose. The transport (`gh-relay-gnostr.c:206-215`) redials with exponential backoff capped at 30 s and **no limit**. Consider a refused group that is the relay's only room. If the relay's REQ connection never comes back, or the relay never sends EOSE (non-compliant), Groundhog keeps a socket to that relay, or keeps redialling it every 30 s, for a group the account is not in. This lasts until restart, forget, re-join or an account change. Before the fix, the refusal closed that scope at once. No additional data is exposed (§2.2), but it is an open-ended exception to the header rule "nothing is opened for a group that is not joined or being joined". Joined rooms tolerate a never-EOSE relay the same way (they stay SYNCING), so this is consistent with the rest of the service, but those rooms are ones the user asked to read. Suggested follow-up: bound the wait, e.g. clear `awaiting_state` on the first DISCONNECTED after the refusal, or after a short timer through `self->clock` (a timer fits the codebase's testable-clock pattern).

### 2.4 Open groups and other behaviour

- An open group accepts the join (OK true), so nothing changes.
- An open group that *refuses* (ban, "blocked:") takes the awaiting path. It reads its 39000 until EOSE and stays DENIED (`!closed`, so `room_sync_metadata()` does not move it). It may also pick up the relay-signed name. Then it is dropped. The only visible difference is that the room's read state is IDLE immediately, and the scope's single resubscribe now happens at EOSE instead of at refusal.
- The other joined rooms on the relay still get exactly one rebuilt REQ per refusal (SYNCING → LIVE again), as before; only the timing moves. On a CLOSED of the REQ, `settled` stays FALSE and the joined rooms go REFUSED/AUTH_REQUIRED as before.
- `room_set_read(IDLE)` in `apply_join_result()` is redundant on the non-awaiting path (the resubscribe already set IDLE) and harmless.
- The condition `was_reading != room_reads(room) || (!was_subscribed && join_subscribed(join))` is correct. The second clause is needed when an awaiting room is re-joined: `room_reads` stays TRUE, but the REQ must be rebuilt with messages.

### 2.5 Tests

- `refused-before-state`: holds REQ answers so the refusal is handled first, adds a second join while waiting, and checks three things: the rebuilt REQ asks for the refused group by `#d` only; CLOSED arrives after release; `club` is gone from the last REQ. It fails deterministically on the old code (5/5, confirmed above). The `req_mentions(...) == 1` assertion is tied to the wire format, but it is also the privacy property, so it is justified.
- `refused-state-after-eose`: holds NIP-11 so the 39000 is kept back without a key, then checks that the REQ closes (`nothing_subscribed`) and that CLOSED arrives when the key does. On the old code it fails intermittently (4/10 here, 2/5 in the commit message), because it depends on whether the 39000 was received before the refusal. It guards the held-snapshot path, not the ordering, so a probabilistic failure on the old code is acceptable.
- `nip29-relay.h` gains `hold_reqs` / `hold_nip11` with explicit release. These are event-driven, with no sleeps.

---

## 3. nostrc-vzls: test fix

### 3.1 Explanation: sound, reproduced independently

On XNU, a dual-stack `[::]:0` socket with SO_REUSEADDR is not checked against IPv4 sockets bound to a *specific* address. An IPv4 dial to 127.0.0.1 then goes to the more specific listener. My probe (§1) reproduced this 298/300, with the dial reaching the holder every time, and never with a `127.0.0.1:0` bind. The commit's point that sequential ephemeral allocation hides the collision from ordinary "is the port stolen" checks also holds: only explicitly chosen ports, like `dolt sql-server -P <port>`, show it. The fix, binding `127.0.0.1:0` because every dial in these tests goes to 127.0.0.1, is correct.

### 3.2 Product code: no listeners (confirmed)

`gnome/groundhog/src` contains no `bind(`, `listen(`, `g_socket_listener_add_*`, `g_socket_service_new`, `g_threaded_socket_service`, `soup_server_new/listen*`, `INADDR_ANY`, `in6addr_any` or `g_inet_address_new_any`. `nostr-gobject/src` and `libnostr/src` also have no listen calls. **Groundhog production code never listens**, so it cannot have the wildcard-bind hazard.

### 3.3 Remaining test listeners

All other Groundhog test listeners are already loopback-IPv4:

- `soup_server_listen_local(..., SOUP_SERVER_LISTEN_IPV4_ONLY)` in `nip29-relay.h`, `wire-relay.h`, `blossom-fixture.c`, `test_relay_soup.c`, `test_account_relays.c`, `test_e2e_dm.c`, `test_new_message.c`
- inline `127.0.0.1:0` `g_socket_listener_add_address()` in `socks5-fixture.c:279`, `test_privacy_e2e.c:141`, `test_relay_guard.c:370`

No `add_any_inet_port` remains (N1).

### 3.4 Other changes

- `gh_test_held_port_init()` now uses the helper. The behaviour is identical.
- `test_blossom.c` `spin_until()` gets a 30 s failure bound through `g_error()`, naming the line. The success path removes the timer. This is a failure bound, not a timing assumption, so it is fine.
- `/groundhog/relay/loopback-listener` is meaningful on macOS (the mutation fails it 10/10). On Linux, where ephemeral selection is not sequential, it passes trivially, which is acceptable for a regression guard. The port wrap `49152 + (candidate - 65536)` matches macOS's default `portrange.first/last` (49152-65535).

---

## 4. Findings

| ID | Sev | Where | Finding |
|---|---|---|---|
| L1 | Low | `gh-nip29-service.c` `on_scope_update()` / `apply_join_result()` | `awaiting_state` has no bound of its own. If the relay never sends EOSE, or its REQ connection stays down, a refused group that is the relay's only room keeps a scope open, redialling every 30 s, until restart, forget, re-join or an account change. Suggest a timer (via `self->clock`) or clearing it at the first DISCONNECTED after the refusal. Nothing extra is exposed (§2.2). |
| L2 | Low | `room_record()` | `awaiting_state` is not persisted. If the app quits between the refusal and the EOSE, the room stays DENIED even if the group is closed, because DENIED rooms never read state again. This is rare, and a re-join fixes it. |
| L3 | Nit | `gh-nip29-service.c:1050-1052` | The comment "the relay layer may report EOSE before the backfill it ends" is stale since nostrc-qp24.10.6. The kfso fix now depends on the opposite being true, so the comment should be corrected so nobody "fixes" around it. |
| N1 | Nit | `socks5-fixture.c`, `test_privacy_e2e.c`, `test_relay_guard.c` | Three inline copies of the 127.0.0.1:0 bind could call `gh_test_listen_loopback()`. Cosmetic only. |
| N2 | Nit | `nostr_subscription.c:240` (pre-existing) | `EVENT_QUEUE_CAPACITY` (200) drops the oldest *event* under backlog. A group's 39000 could in theory be dropped before its EOSE. This affects joined rooms equally and predates this change, but kfso's CLOSED detection inherits it. |

None of these block the merge.

**APPROVED**
