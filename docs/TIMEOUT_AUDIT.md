# Timeout Audit: Elimination Strategy

> "Timeouts are a code smell in a reactive pub/sub system." — Every timeout
> should be replaced with an event-driven signal: EOSE, connection callbacks,
> done channels, WaitGroups, or state-change notifications.

## Audit Date: 2026-02-17

---

## Category A: 🔴 MUST ELIMINATE — Polling Loops / Blocking Sleeps

These are `usleep()` / `nanosleep()` / `g_usleep()` calls inside loops that
**block a thread (or fiber) doing nothing useful**. They should be replaced
with `go_select()` on channels, `GoWaitGroup`, or state-change callbacks.

| # | File | Line | Pattern | Current | Replacement |
|---|------|------|---------|---------|-------------|
| A1 | `simplepool.c` | 765-768 | ✅ FIXED: Worker uses `go_select` on wake_ch + per-sub events channels | — | — |
| A2 | `relay.c` | 1155 | ✅ FIXED: `go_select_timeout` on done channel | — | — |
| A3 | `relay.c` | 1025 | ✅ FIXED: Removed per-event usleep throttle | — | — |
| A4 | `relay_optimized.c` | 416 | ✅ FIXED: Blocking `go_channel_receive` + `go_select_timeout` batch window | — | — |
| A5 | `subscription.c` | 979 | ✅ FIXED: Direct `go_wait_group_wait()` — no polling | — | — |
| A6 | `nostr_subscription.c` | 320 | ✅ FIXED: `go_select` on events/eose/closed channels | — | — |
| A7 | `nostr_pool.c` | 728 | ✅ FIXED: `go_select` on per-relay events + eose channels | — | — |
| A8 | `nip46_session.c` | 624 | ✅ FIXED: `go_select_timeout` on monitor channel | — | — |
| A9 | `storage_ndb.c` | 382 | ✅ FIXED: Fail-fast with `sched_yield()` | — | — |
| A10 | `main-window.c` | 9082 | ✅ FIXED: Removed `g_usleep(500000)` — multi-sub handles late relays | — | — |
| A11 | `simplepool.c` | 355 | ✅ FIXED: `pthread_join` (thread no longer detached) | — | — |
| A12 | `simplepool.c` | 388 | ✅ FIXED: Removed 500ms sleep — cleanup worker already joined | — | — |
| A13 | `ipc.c` | 558 | ✅ FIXED: `poll()` on listening socket — zero CPU when idle | — | — |
| A14 | `ipc.c` | 565 | ✅ FIXED: Same `poll()` handles EAGAIN | — | — |
| A15 | `cli_live_logger.c` | 73 | ✅ FIXED: Single attempt, fail-fast | — | — |
| A16 | `nostrfs.c` | 274 | ✅ FIXED: Single pass, no retry sleep | — | — |
| A17 | `nostr-homectl.c` | 80,120 | ✅ FIXED: `g_subprocess_wait()` for systemctl | — | — |

---

## Category B: 🟡 REPLACE — Timeouts Masking Missing Events

These use `g_timeout_add()` as a fallback because the code doesn't properly
listen for the real event that should trigger the action.

| # | File | Line | Pattern | Current | Replacement |
|---|------|------|---------|---------|-------------|
| B1 | `main-window.c` | 6487 | ✅ FIXED: `g_idle_add_once` — runs on next main loop iteration | — | — |
| B2 | `main-window.c` | 9069,9103 | Retry pool_live after 5s | `g_timeout_add(5000,...)` | Fire on relay state-changed to CONNECTED |
| B3 | `main-window.c` | 9008 | ✅ FIXED: `g_idle_add_full` — runs on next main loop iteration | — | — |
| B4 | `main-window.c` | 9115 | Health check every 30s | `g_timeout_add_seconds(30,...)` | Relay emits health event on disconnect; react to that |
| B5 | `zap.c` | 268 | LNURL retry after failure | `g_timeout_add(delay,...)` | Exponential retry is standard for HTTP; acceptable but should cap |
| B6 | `zap.c` | 432,711 | LNURL request timeout | `g_timeout_add_seconds(TIMEOUT,...)` | HTTP request timeout is set on SoupMessage itself; this is redundant |
| B7 | `chess-game-view.c` | 691 | Loading timeout 10s | `g_timeout_add_seconds(10,...)` | EOSE callback should transition loading→complete |
| B8 | `relay_store.c` | 498 | ✅ FIXED: `g_idle_add` (simulated delay removed) | — | — |
| B9 | `sheet-user-list.c` | 581 | ✅ FIXED: `g_idle_add` (simulated delay removed) | — | — |
| B10 | `nwc-plugin.c` | 465 | NWC response timeout | `g_timeout_add(TIMEOUT,...)` | Use GCancellable with the async operation instead |
| B11 | `native-host.c` | 209 | Approval timeout 60s | `g_timeout_add_seconds(60,...)` | Legitimate UX timeout for user action |

---

## Category C: ✅ LEGITIMATE — UI/UX Timing (Keep)

These are inherent to desktop UX — toast auto-hide, animation timing,
countdown displays, debouncing user input, periodic diagnostics. They do NOT
mask missing events; they ARE the events (human-scale timing).

| # | File | Pattern | Reason |
|---|------|---------|--------|
| C1 | Multiple (5 files) | Toast auto-hide after 3s | Standard GTK UX |
| C2 | `gn-nostr-event-model.c` | Frame-rate drain at 16ms | Matches display refresh |
| C3 | `debounce.c` | Input debounce (configurable) | Standard UX pattern |
| C4 | `gnostr-search-results-view.c` | Search debounce 300ms | Standard UX pattern |
| C5 | `gnostr-video-player.c` | Controls hide after 3s | Standard video UX |
| C6 | `gnostr-video-player.c` | Position update every 250ms | Slider tracking |
| C7 | `gnostr-poll-card.c` | Countdown update every 30s | Time display |
| C8 | `gnostr-zap-goal-card.c` | Celebration animation | Visual effect |
| C9 | `nostr-note-card-row.c` | Timestamp "5m ago" every 60s | Relative time display |
| C10 | `nostr-note-card-row.c` | Lazy load image on map 150ms | Prevents layout thrash |
| C11 | `gnostr-chess-card.c` | Chess autoplay animation | Game animation |
| C12 | `gnostr-nip7d-thread-view.c` | Highlight removal 2s | Visual feedback |
| C13 | `gnostr-chess-publish-dialog.c` | Auto-close after publish 1s | UX feedback |
| C14 | `profile_service.c` | Debounce profile fetch | Batching optimization |
| C15 | `main-window.c` | Profile fetch debounce 150ms | Batching optimization |
| C16 | `main-window.c` | Relay discovery filter debounce 100ms | Batching optimization |
| C17 | `main-window.c` | Metrics panel refresh 2s | Dashboard display |
| C18 | `main-window.c` | Periodic stats logging 60s | Diagnostics |
| C19 | `main-window.c` | Periodic backfill (user-configurable) | Data freshness |
| C20 | `nostr_query_batcher.c` | Batch window flush | Batching optimization |
| C21 | `nostr_subscription_registry.c` | Health check tick | Monitoring (but see B4) |
| C22 | `gnostr-sync-service.c` | Sync interval timer | User-configurable sync |
| C23 | `gnostr-thread-view.c` | Thread rebuild debounce | Prevents layout thrash |
| C24 | `gnostr-timeline-view.c` | Scroll idle timeout | Metadata batch trigger |
| C25 | `nip77-negentropy-plugin.c` | Auto-sync timer | User-configurable |
| C26 | `nip34-git-plugin.c` | Deferred push to browser 100ms | UI initialization |
| C27 | Various signer files | Lockout countdown, expiration, etc. | Security UX |
| C28 | `rate-limiter.c` | Lockout expiration, save debounce | Security + UX |
| C29 | `secure-delete.c` | Clipboard clear after timeout | Security |
| C30 | `ticker.c` | Periodic tick channel | Deliberate timer |
| C31 | `init.c`, `metrics_collector.c` | Periodic metrics dump | Diagnostics |

---

## Priority Fix Order

### Tier 1: Critical Path (blocks event flow)
1. ✅ **A1** — simplepool worker: `go_select` on wake_ch + per-sub channels
2. ✅ **A6** — GObject subscription monitor: `go_select` on 3 channels
3. ✅ **A7** — GObject pool query: `go_select` on events + eose channels
4. ✅ **A4** — relay_optimized: blocking receive + timed select batch window
5. ✅ **A9** — storage_ndb: fail-fast with `sched_yield()`

### Tier 2: Startup/Reconnect (blocks UX responsiveness)
6. ✅ **A10** — removed `g_usleep(500000)` relay wait
7. ✅ **A2** — relay reconnect: `go_select_timeout` on done channel
8. **B1** — startup refresh timeout → relay connected signal
9. **B2** — retry pool_live 5s → relay state change signal

### Tier 3: Shutdown/Cleanup (blocks process exit)
10. ✅ **A11** — `pthread_join` (thread no longer detached)
11. ✅ **A12** — Removed 500ms sleep (cleanup worker already joined)
12. ✅ **A5** — Direct `go_wait_group_wait()` — no polling

### Tier 4: Edge Cases
13. ✅ **A3** — removed per-event usleep throttle
14. ✅ **A8** — NIP-46: `go_select_timeout` on monitor channel
15. ✅ **A13, A14** — `poll()` on listening socket
16. ✅ **A15** — CLI logger: fail-fast, no retry sleep
17. ✅ **A16** — nostrfs publish: single pass, no retry sleep
18. ✅ **A17** — nostr-homectl: `g_subprocess_wait()` for systemctl
19. ✅ **B1** — startup: `g_idle_add_once`
20. ✅ **B3** — relay config restart: `g_idle_add_full`
21. ✅ **B8** — relay test: `g_idle_add` (simulated delay removed)
22. ✅ **B9** — user list sync: `g_idle_add` (simulated delay removed)

### Remaining (legitimate timeouts — Category C territory)
- **B2** — 5s network retry after total connection failure (legitimate backoff)
- **B4** — 30s health check interval (monitoring)
- **B5** — LNURL HTTP retry with exponential backoff (standard HTTP pattern)
- **B6** — LNURL per-request 10s timeout (application-level timeout)
- **B7** — Chess loading 10s safety timeout (UX spinner guard)
- **B10** — NWC 10s response timeout (RPC timeout)
- **B11** — Native host 60s approval timeout (user interaction)

---

## Implementation Pattern: Channel-Based Event Loop

Replace polling loops with this pattern:

```c
// BEFORE (polling):
for (;;) {
    if (check_condition_1()) handle_1();
    if (check_condition_2()) handle_2();
    if (should_stop()) break;
    usleep(1000); // 1ms backoff ← THE SMELL
}

// AFTER (event-driven):
GoChannel *event_ch = ...;  // events pushed here by producers
GoChannel *done_ch = ctx->done;
GoSelectCase cases[] = {
    { .chan = event_ch, .dir = GO_SELECT_RECV },
    { .chan = done_ch,  .dir = GO_SELECT_RECV },
};
for (;;) {
    int which = go_select(cases, 2);
    if (which == 0) handle_event(cases[0].val);
    if (which == 1) break;  // done
}
```

No sleeps. No polling. Instant wake on any event. Zero CPU when idle.
