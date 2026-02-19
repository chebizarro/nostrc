# malloc_consolidate Crash Fix (nostrc-deferred-free)

## The Bug

```
malloc_consolidate(): unaligned fastbin chunk detected
```

A persistent, timing-dependent heap corruption crash that:
- Appeared ~7 seconds after app startup (after `initial_refresh_timeout_cb`)
- **Never** triggered under ASAN, TSAN, Valgrind, or GDB
- Was masked by `G_SLICE=always-malloc` (a no-op on GLib ≥ 2.76) due to timing shifts
- Made the application unusable on Linux

## Root Cause

**`go_channel_unref` freed channel structs while nsync waiters still held references to the embedded mutex.**

### The Race Condition

```
Thread A (unref)                    Thread B (waiter)
─────────────────                   ──────────────────
chan->refs → 0
LOCK(chan->mutex)
chan->closed = 1
CV_BROADCAST(chan)                  ← woken by broadcast
UNLOCK(chan->mutex)                 → nsync_mu_lock(&chan->mutex) [BLOCKED]
sched_yield() × 3
free(chan)  ← FREES THE STRUCT     → nsync WRITES to freed memory ← CORRUPTION
```

When `go_channel_unref` detected the last reference and freed the channel, woken
threads were still inside `nsync_mu_lock()` trying to reacquire `chan->mutex`. Since
nsync is **not compiled with ASAN instrumentation**, these writes to freed memory
went undetected by all sanitizers. But glibc's `malloc_consolidate` detected the
corrupted fastbin metadata when it tried to consolidate the freed memory.

The previous "fix" — calling `sched_yield()` three times before `free(chan)` — was
a timing hack that failed under real load when relay connections created dozens of
concurrent goroutines competing for channel access.

### Why ASAN Didn't Catch It

1. **nsync is uninstrumented**: nsync is compiled as a dependency without `-fsanitize=address`. Its internal `nsync_mu_lock` writes to the mutex struct without ASAN shadow memory checks.
2. **ASAN quarantine**: ASAN delays memory reuse via a quarantine zone. By the time nsync wrote to freed memory under ASAN, the memory was in quarantine and hadn't been reused — so the corruption was harmless.
3. **Timing**: ASAN's ~2× slowdown changed scheduling enough that nsync waiters exited before `free(chan)` ran.

### Why G_SLICE=always-malloc Masked It

`G_SLICE=always-malloc` is a no-op on GLib ≥ 2.76 (the slice allocator was removed).
The masking effect was purely from timing changes: setting any environment variable
subtly affected process startup timing, memory layout, and thread scheduling enough
to prevent the race from manifesting.

## The Fix

### 1. Deferred Channel Destruction (channel.c)

Replaced `sched_yield × 3 + free(chan)` with a **graveyard-based deferred free**:

```c
// Instead of: free(chan)
go_channel_graveyard_add(chan);   // Add to graveyard with timestamp
go_channel_graveyard_reap();     // Free channels dead > 1 second
```

Dead channels are added to a timestamped linked list. They are only freed after
1 full second has elapsed — far more than enough time for any nsync waiters to
observe the closed state and exit. Reaping is amortized onto `go_channel_create`
and `go_channel_unref` calls.

The channel struct remains valid (but logically dead) during the deferral period:
- `magic = 0` — prevents new operations via magic-number checks
- `buffer = NULL` — send/receive return -1 immediately
- `closed = 1` — woken waiters see the channel is closed and exit gracefully
- **`mutex` remains intact** — nsync can safely reacquire and release it

### 2. Removed Unused Fiber Scheduler (main_app.c)

`go_fiber_compat()` was already reverted to OS threads (`nostrc-b0h-revert` in
`go.c`), but `gof_start_background(0)` was still creating idle worker threads
and poller threads that served no purpose. Removed to eliminate unnecessary
thread contention.

### 3. Removed G_SLICE Workaround (main_app.c)

The `setenv("G_SLICE", "always-malloc", 0)` cargo cult workaround was removed.
It was a no-op on modern GLib and only masked the real issue through timing effects.

### 4. Simplified go() Wrapper (go.c)

Replaced the `go_autofree GoWrapper *w` pattern with explicit `malloc`/`free`
for clarity. The `go_autofree` + `go_steal_pointer` dance was correct but
unnecessary complexity for a simple two-path function.

## Files Changed

| File | Change |
|------|--------|
| `libgo/src/channel.c` | Graveyard infrastructure + deferred free in `go_channel_unref` |
| `libgo/src/go.c` | Simplified `go()` wrapper |
| `apps/gnostr/src/main_app.c` | Removed fiber scheduler, G_SLICE workaround |
| `gnome/nostr-homed/src/fs/nostrfs.c` | Removed fiber scheduler startup |

## Verification

The fix can be verified by:

1. **Build without ASAN**: `cmake --build build --target gnostr`
2. **Run without G_SLICE**: `./build/apps/gnostr/gnostr` (no environment hacks)
3. **Stress test**: Open app, let relays connect, scroll timeline aggressively
4. **The app should not crash** within the first 30+ seconds (previously crashed at ~7s)

To verify the graveyard is working, add `GOF_DEBUG=1` to enable scheduler debug
logging, or add a `fprintf` to `go_channel_graveyard_reap` to see channels being
reaped.

## Follow-up Fix: NostrConnection Use-After-Free (nostrc-conn-uaf)

The graveyard fix eliminated the channel-level UAF, but the crash persisted with
a new assertion:

```
GLib:ERROR:../../../glib/gmain.c:4090:g_main_context_check_unlocked:
  assertion failed: (i <= 0 || fds[i - 1].fd < fds[i].fd)
malloc_consolidate(): unaligned fastbin chunk detected
```

### Root Cause

**`nostr_connection_close()` freed the `NostrConnection` struct immediately while
the LWS service thread's callback was still referencing it.**

The LWS callback runs on the service thread and captures `conn` as a local variable
from `lws_get_opaque_user_data(wsi)`. The close thread sets the opaque data to NULL,
but this is a simple pointer write — NOT synchronized with the callback thread:

```
Close thread                       LWS service thread (callback)
────────────                       ────────────────────────────
                                   conn = lws_get_opaque_user_data(wsi) → valid!
lws_set_opaque_user_data(wsi, NULL)
free(conn)                         priv = conn->priv ← FREED MEMORY
                                   nsync_mu_lock(&priv->mutex) ← WRITES TO GARBAGE
                                   → heap corruption
```

The heap corruption eventually corrupts GLib's internal GPollFD arrays, causing the
`fds[i-1].fd < fds[i].fd` assertion. The `malloc_consolidate` error is glibc
detecting the fastbin metadata corruption during the abort.

### Fix

Extended the deferred cleanup queue to hold **both** `NostrConnection` and
`NostrConnectionPrivate` structs:

```c
typedef struct DeferredConn {
    NostrConnection        *conn;
    NostrConnectionPrivate *priv;
    struct DeferredConn    *next;
} DeferredConn;
```

In `nostr_connection_close()`:
- If `should_free_priv=1` (last connection, service thread joined): free both immediately
- Otherwise: add both to the deferred queue, freed when the service thread eventually stops

### Why `lws_set_opaque_user_data(NULL)` Wasn't Sufficient

1. The LWS callback does NOT hold `g_lws_mutex` when it calls `lws_get_opaque_user_data`
2. Pointer-sized writes are atomic on x86_64, but the callback may have already cached
   `conn` as a local variable before the NULL was written
3. `lws_cancel_service()` wakes the service thread but doesn't wait for in-flight callbacks
4. There is no LWS API to synchronize with in-flight callbacks

## Future Work

When fibers are re-enabled, restore in `main_app.c`:
```c
gof_start_background(0);
go_blocking_executor_init(4);
```

And at shutdown:
```c
go_blocking_executor_shutdown();
gof_request_stop();
gof_join_background();
```

The GoContext struct has a similar pattern (`base_context_free` frees the struct
containing an nsync mutex) but is protected by reference counting. If context-related
crashes appear, apply the same graveyard pattern to context destruction.
