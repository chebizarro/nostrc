# Relay Subscription Management Audit

## Overview

This audit reviews how widgets in the gnostr application manage relay connections and subscriptions via `GnostrSimplePool`.

## Widgets Using Relay Subscriptions

### 1. Per-Widget Pool Instances (Created in init, destroyed in dispose)

| Widget | Pool Field | Cancellable | Lifecycle |
|--------|-----------|-------------|-----------|
| `gnostr-thread-view.c` | `simple_pool` | `fetch_cancellable` | ✅ Proper: created in init, cleared in dispose |
| `gnostr-profile-pane.c` | `simple_pool` | Multiple cancellables | ✅ Proper: created in init, cleared in dispose |
| `gnostr-nip7d-thread-view.c` | `simple_pool` | `fetch_cancellable` | ✅ Proper: created in init, cleared in dispose |
| `gnostr-articles-view.c` | `pool` | `fetch_cancellable` | ✅ Proper: created lazily, cleared in dispose |
| `gnostr-login.c` | `nip46_pool` | `cancellable` | ✅ Proper: created when needed, cleared with signal disconnect |
| `gnostr-dm-service.c` | `pool` | `cancellable` | ✅ Proper: created in start, cleared in stop with signal disconnect |
| `gnostr-main-window.c` | `pool` | `pool_cancellable` | ✅ Proper: created lazily, managed with reconnection logic |

### 2. Static/Shared Pool Instances (Singleton pattern)

| Widget | Pool Variable | Notes |
|--------|--------------|-------|
| `gnostr-note-embed.c` | `static embed_pool` | ✅ Good: Shared singleton, syncs with relay config changes |
| `gnostr-search-results-view.c` | `static s_pool` | ⚠️ Never cleaned up (acceptable for app-lifetime singleton) |
| `gnostr-timeline-view.c` | `static s_pool` | ⚠️ Never cleaned up (acceptable for app-lifetime singleton) |

## Callback Safety Patterns

### Good Patterns Found:

1. **Cancellation check before widget access** (`gnostr-note-embed.c`):
```c
if (err) {
  if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_error_free(err);
    if (results) g_ptr_array_unref(results);
    return;  /* Widget was disposed, don't access user_data */
  }
}
```

2. **Type check before access** (`gnostr-thread-view.c`):
```c
if (!GNOSTR_IS_THREAD_VIEW(self)) return;
```

3. **Cancel before new request** (most widgets):
```c
if (self->fetch_cancellable) {
  g_cancellable_cancel(self->fetch_cancellable);
  g_clear_object(&self->fetch_cancellable);
}
self->fetch_cancellable = g_cancellable_new();
```

### Potential Issues:

1. **`gnostr-timeline-view.c`**: Uses weak references for row tracking but static pool never cleaned up.

2. **`gnostr-search-results-view.c`**: Static pool never cleaned up, but uses per-search cancellable which is good.

3. **Signal handler disconnection**: `gnostr-dm-service.c` and `gnostr-login.c` properly disconnect signal handlers before clearing pool. Other widgets using `subscribe_many` should follow this pattern.

## Subscription Types

### One-shot Queries (`query_single_async`)
- Used by: thread-view, profile-pane, articles-view, note-embed, search-results, timeline-view, nip7d-thread-view
- Pattern: Fire query, get results, done
- Cancellation: Via `GCancellable` passed to async call

### Long-lived Subscriptions (`subscribe_many_async`)
- Used by: main-window (live feed), dm-service (gift wraps), login (NIP-46)
- Pattern: Subscribe, receive events via signal, unsubscribe on dispose
- Cancellation: Via `GCancellable` + signal handler disconnect

## Recommendations

### 1. Consider Shared Pool for Query-Only Widgets
Similar to the shared `SoupSession` pattern, widgets that only do one-shot queries could share a pool:
- `gnostr-thread-view.c`
- `gnostr-profile-pane.c`
- `gnostr-articles-view.c`
- `gnostr-nip7d-thread-view.c`

**Benefits:**
- Reduced connection churn
- Better connection reuse
- Simpler lifecycle management

**Implementation:**
```c
// In utils.c
static GnostrSimplePool *s_shared_query_pool = NULL;

GnostrSimplePool *gnostr_get_shared_query_pool(void) {
  if (!s_shared_query_pool) {
    s_shared_query_pool = gnostr_simple_pool_new();
  }
  return s_shared_query_pool;
}
```

### 2. Ensure All Callbacks Check Cancellation First
Some callbacks check `GNOSTR_IS_*` but don't check cancellation first. The safest pattern is:
```c
static void on_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GError *err = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(..., &err);
  
  // Check cancellation FIRST - widget may be freed
  if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_error_free(err);
    if (results) g_ptr_array_unref(results);
    return;
  }
  
  // Now safe to access user_data
  MyWidget *self = MY_WIDGET(user_data);
  if (!MY_IS_WIDGET(self)) { ... }
}
```

### 3. Signal Handler Cleanup for Subscriptions
Widgets using `subscribe_many_async` with event signals MUST:
1. Store the signal handler ID
2. Disconnect the handler BEFORE clearing the pool
3. Cancel the cancellable

Example from `gnostr-dm-service.c`:
```c
if (self->pool) {
  if (self->events_handler > 0) {
    g_signal_handler_disconnect(self->pool, self->events_handler);
    self->events_handler = 0;
  }
  g_clear_object(&self->pool);
}
```

## Current Status

| Aspect | Status |
|--------|--------|
| Pool lifecycle management | ✅ Good - all widgets properly dispose pools |
| Cancellable usage | ✅ Good - all async calls use cancellables |
| Callback safety | ⚠️ Mixed - some callbacks could be safer |
| Signal handler cleanup | ✅ Good - subscription widgets handle this |
| Connection reuse | ⚠️ Could improve with shared pool pattern |

## Files Reviewed

- `gnostr-main-window.c` - Main app pool and live subscription
- `gnostr-note-embed.c` - Shared embed pool with relay sync
- `gnostr-thread-view.c` - Per-widget pool for thread queries
- `gnostr-profile-pane.c` - Per-widget pool for profile/posts queries
- `gnostr-nip7d-thread-view.c` - Per-widget pool for NIP-7D threads
- `gnostr-articles-view.c` - Per-widget pool for article queries
- `gnostr-search-results-view.c` - Static pool for search
- `gnostr-timeline-view.c` - Static pool for timeline queries
- `gnostr-login.c` - Per-widget pool for NIP-46 login
- `gnostr-dm-service.c` - Per-service pool for DM subscriptions
