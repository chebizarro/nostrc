# Test Strategy & Suite Documentation

## Overview

The gnostr test suite validates memory safety, performance budgets, widget correctness, event flow integrity, and zero-copy sliding window invariants across the three library layers (`nostr-gobject`, `nostr-gtk`) and the application (`apps/gnostr`).

### Architecture

```
┌─────────────────────────────────────┐
│  Perf / Memory Benchmarks (nightly) │  ← RSS ceiling, latency budgets
├─────────────────────────────────────┤
│  Integration Tests (CI per-PR)      │  ← Event flow, delete authorization
├─────────────────────────────────────┤
│  Widget Tests (Xvfb, CI per-PR)     │  ← Recycling stress, sizing, latency
├─────────────────────────────────────┤
│  Unit Tests (no display, fast)      │  ← Store contract, cache bounds, lifecycle
└─────────────────────────────────────┘
```

### Test Locations

| Layer | Directory | Links Against | Display Required |
|-------|-----------|---------------|-----------------|
| Shared infrastructure | `tests/testkit/` | `nostr-gobject` + glib | No |
| nostr-gobject unit | `nostr-gobject/tests/` | `nostr-gobject` + testkit | No |
| nostr-gobject integration | `nostr-gobject/tests/integ/` | Same + local NDB | No |
| nostr-gtk widget | `nostr-gtk/tests/` | `nostr-gtk` + `nostr-gobject` + testkit | Yes (Xvfb) |
| Application-level | `apps/gnostr/tests/` | app model sources + `nostr-gobject` | No |
| Application integration | `apps/gnostr/tests/integ/` | Same + NDB | No |
| Benchmarks | `benchmark/` | Release-with-debug-info | Yes (Xvfb) |

---

## Shared Test Infrastructure: The Testkit

**Location**: `tests/testkit/gnostr-testkit.{h,c}`

A static library that all test binaries link against, providing:

### `GnTestNdb` — Temporary NDB Instance

```c
GnTestNdb *gn_test_ndb_new(const char *opts_json);  // temp dir + storage_ndb_init
const char *gn_test_ndb_get_dir(GnTestNdb *ndb);
gboolean gn_test_ndb_ingest_json(GnTestNdb *ndb, const char *json);
void gn_test_ndb_free(GnTestNdb *ndb);
```

Each test gets an isolated nostrdb instance in a unique temp directory. Automatically cleaned up on free.

### Event Fixture Generation

```c
char *gn_test_make_event_json(int kind, const char *content, gint64 created_at);
char *gn_test_make_event_json_with_pubkey(int kind, const char *content,
                                           gint64 created_at, const char *pubkey_hex);
GPtrArray *gn_test_make_events_bulk(guint n, int kind, gint64 start_ts);
```

Generates minimal valid nostr event JSON strings with random (or specified) pubkeys and IDs.

### Main Loop Helpers

```c
gboolean gn_test_run_loop_until(GnTestPredicate pred, gpointer data, guint timeout_ms);
void gn_test_drain_main_loop(void);
```

### Memory Measurement

```c
gsize gn_test_get_rss_bytes(void);   // Linux: /proc/self/status VmRSS; macOS: mach_task_basic_info
double gn_test_get_rss_mb(void);
```

### Object Lifecycle Watchers

```c
GnTestPointerWatch *gn_test_watch_object(GObject *obj, const char *label);
void gn_test_assert_finalized(GnTestPointerWatch *watch);
void gn_test_assert_not_finalized(GnTestPointerWatch *watch);
```

Uses `g_object_weak_ref()` to track GObject finalization. Call `gn_test_assert_finalized()` after `g_object_unref()` to verify the object was actually freed.

---

## Test Suite by Blocker Category

### 1️⃣ Memory Footprint (target < 1 GB)

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `benchmark/gnostr_bench_memory_timeline_scroll.c` | 1 | Creates 10,000-item GtkListView, scrolls through entire content, samples RSS at each step, enforces hard ceiling (default 1 GB) |
| `nostr-gobject/tests/test_profile_provider_lru.c` | 5 | LRU cache capacity enforcement, eviction ordering, init/shutdown cycles, watcher cleanup |

**Key invariants enforced:**
- Profile provider cache size ≤ configured capacity after any number of insertions
- LRU evicts least-recently-used entries (recently accessed survive)
- Process RSS stays below ceiling during sustained timeline scrolling

### 2️⃣ Memory Leaks

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gobject/tests/test_lifecycle_leaks.c` | 5 | GNostrEvent create/destroy loops with weak refs, GNostrNdbStore subscribe/unsubscribe cycles, event with signal connections, rapid ref/unref churn (500x) |
| `nostr-gtk/tests/test_widget_churn_leaks.c` | 2 | MockItem with atomic live-count tracking; 50× model clear/repopulate cycles; model-to-NULL swap cycles |
| `apps/gnostr/tests/test_event_model_windowing.c` | 1 (subset) | `test_model_finalize_no_leak`: 20 create/destroy cycles with weak-ref verification |

**CI enforcement:**
All tests run with `G_DEBUG=fatal-warnings,gc-friendly` and `G_SLICE=always-malloc`. Sanitizer builds use `-fsanitize=address` with `detect_leaks=1`.

### 3️⃣ Widget Sizing

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gtk/tests/test_note_card_measure.c` | 3 | Content corpus with 9 edge cases (short text, long text, many links, many hashtags, unicode-heavy, empty, newlines-only, single long word). Tests `gtk_widget_measure()` bounds and ScrolledWindow clamping. |

### 4️⃣ Segfaults from Recycling / Signal Detachment

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gtk/tests/test_listview_recycle_stress.c` | 6 | **Most critical test.** Uses MockEventItem with `notify::profile` signal. Tests: basic bind/unbind, model replacement, **profile notification after unbind** (the exact crash path), rapid scroll churn (500 items), 20× clear/repopulate cycles, simultaneous profile updates during scroll. |

**Crash vector tested:**
1. `factory_bind_cb()` → installs handler on item's `notify::profile`
2. `factory_unbind_cb()` → attempts to disconnect the handler
3. If unbind races with async profile completion → UAF crash
4. Test verifies handler is properly disconnected by setting profile after unbind

### 5️⃣ UI Latency

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gtk/tests/test_bind_latency_budget.c` | 2 | Bind 300 items in GtkListView + scroll through entire list; heartbeat idle detects main-thread stalls > threshold. Model swap (10× clear/repopulate) stall detection. |

**Heartbeat mechanism:** A 5ms `g_timeout_add` heartbeat tracks gaps between ticks. If any gap exceeds `MAX_STALL_MS` (100ms, or 1000ms under ASan), it's counted as a "missed" heartbeat. Tests assert heartbeat actually fired (`count >= MIN_HEARTBEATS`) to prevent vacuous passing.

### 6️⃣ Event Flow Correctness

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gobject/tests/integ/test_event_flow_ingest_subscribe.c` | 5 | Ingest→subscribe→poll round-trip, multiple subscriptions, double-poll consumed, unsubscribe stops delivery, note counts |
| `nostr-gobject/tests/test_store_contract.c` | 7 | GNostrStore interface contract: implements check, subscribe, poll, lifecycle |
| `apps/gnostr/tests/integ/test_model_delete_authorization.c` | 6 | NIP-09: authorized removes from model, unauthorized preserves, non-existent harmless, multi-target, re-ingest, repeated cycles |

**NIP-09 delete tests:** Each test ingests both kind-0 profiles and kind-1 notes (so the model's readiness filter passes), creates a `GnNostrEventModel`, refreshes it, asserts the note is visible, then ingests a kind-5 delete and verifies model state.

### 7️⃣ Zero-Copy Sliding Windows

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `apps/gnostr/tests/test_event_model_windowing.c` | 11 | Model lifecycle, refresh with profile ingestion (asserts n > 0 AND n ≤ MODEL_MAX_ITEMS), trim-newer/older, clear, thread-view bypass, pending count/flush, visible range, drain timer, finalize no leak |
| `apps/gnostr/tests/test_event_item_txn_budget.c` | 7 | Item creation budget, bulk create/destroy, profile set/get, metadata accessors (legacy constructor path), thread info, reaction/zap stats, animation skip |
| `nostr-gtk/tests/test_thread_view_bounded_load.c` | 5 | MAX_THREAD_EVENTS clamping, swap no accumulation, ancestor dedup, large GtkListView, empty thread |

**Windowing invariants:**

| Invariant | Type | Scope |
|-----------|------|-------|
| No duplicate keys across `note_key_set` ∩ `insertion_key_set` | Hard (always) | All views |
| `item_cache.size ≤ ITEM_CACHE_SIZE` | Hard | All views |
| `insertion_buffer.len ≤ INSERTION_BUFFER_MAX` after backpressure | Hard | All views |
| `notes->len ≤ MODEL_MAX_ITEMS` | Eventual (quiescent) | Non-thread views |

---

## Profile Service Tests

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gobject/tests/test_profile_service_batching.c` | 9 | Singleton lifecycle, request dedup (1 pending, 2 callbacks), debounce batching, cancel-for-user-data, stats accuracy, shutdown cleanup, debounce setting, invalid pubkey rejection (counter stays 0), relay provider registration |

---

## ASan / Sanitizer Relaxation

All timing-sensitive tests use a `SANITIZER_SLOWDOWN` multiplier:

```c
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) \
    || (defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
#  define SANITIZER_SLOWDOWN 10
#else
#  define SANITIZER_SLOWDOWN 1
#endif
#define TIMING_BUDGET_US(base) ((gint64)(base) * SANITIZER_SLOWDOWN)
```

This prevents flaky failures in CI sanitizer jobs where execution is 5-10× slower.

---

## Build & Run

### CMake

```bash
# Standard build with tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure

# With sanitizers
cmake -B build-san \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGNOSTR_ENABLE_ASAN=ON \
  -DGNOSTR_ENABLE_UBSAN=ON
cmake --build build-san
cd build-san && ctest --output-on-failure

# Widget tests only (requires Xvfb on Linux)
ctest -R nostr_gtk --output-on-failure

# Benchmarks (optional, for nightly)
cmake -B build-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_BENCHMARKS=ON
cmake --build build-bench
cd build-bench && ctest -R bench --output-on-failure
```

### Meson

```bash
meson setup build-meson
meson compile -C build-meson
meson test -C build-meson
```

### By Label (CMake)

```bash
ctest -L unit --output-on-failure       # Unit tests (no display)
ctest -L widget --output-on-failure     # Widget tests (Xvfb)
ctest -L integ --output-on-failure      # Integration tests
ctest -L "memory|nightly" --output-on-failure  # Benchmarks
```

### Environment Variables

All tests automatically set:

| Variable | Value | Purpose |
|----------|-------|---------|
| `G_DEBUG` | `fatal-warnings,gc-friendly` | Turn GLib warnings into fatal errors; GC-friendly mode for leak detection |
| `G_SLICE` | `always-malloc` | Disable GSlice so ASan/Valgrind can track allocations |
| `GDK_BACKEND` | `x11` | Force X11 backend for Xvfb compatibility (widget tests) |

---

## CI Configuration

| Job | What | When | Sanitizers |
|-----|------|------|------------|
| **Linux Debug** | All unit tests (no X) | Every PR | None |
| **Linux ASan/UBSan** | All tests including widgets under Xvfb | Every PR | `address`, `undefined`, `detect_leaks=1` |
| **Nightly Perf/Memory** | Benchmarks with RSS ceiling enforcement | Nightly | None (RelWithDebInfo) |

---

## `cmake/GnTest.cmake` Helper Macros

### `gn_add_gtest(name SOURCES ... LIBS ... [ENV ...] [TIMEOUT n])`

Registers a standard GTest binary. Automatically sets `G_DEBUG`, `G_SLICE`, applies sanitizer flags, registers with CTest.

### `gn_add_gtest_xvfb(name SOURCES ... LIBS ... [ENV ...] [TIMEOUT n])`

Same as above, but wraps execution in `xvfb-run -a` on Linux. On macOS, runs natively.

---

## Test Inventory

| # | File | Tests | Category | Display |
|---|------|-------|----------|---------|
| 1 | `nostr-gobject/tests/test_store_contract.c` | 7 | Store interface | No |
| 2 | `nostr-gobject/tests/test_lifecycle_leaks.c` | 5 | Memory leaks | No |
| 3 | `nostr-gobject/tests/test_profile_provider_lru.c` | 5 | Memory bounds | No |
| 4 | `nostr-gobject/tests/test_profile_service_batching.c` | 9 | Service correctness | No |
| 5 | `nostr-gobject/tests/integ/test_event_flow_ingest_subscribe.c` | 5 | Event flow | No |
| 6 | `nostr-gtk/tests/test_listview_recycle_stress.c` | 6 | Segfault prevention | Yes |
| 7 | `nostr-gtk/tests/test_note_card_measure.c` | 3 | Widget sizing | Yes |
| 8 | `nostr-gtk/tests/test_widget_churn_leaks.c` | 2 | Memory leaks | Yes |
| 9 | `nostr-gtk/tests/test_bind_latency_budget.c` | 2 | UI latency | Yes |
| 10 | `nostr-gtk/tests/test_thread_view_bounded_load.c` | 5 | Sliding windows | Yes |
| 11 | `apps/gnostr/tests/test_event_model_windowing.c` | 11 | Sliding windows | No |
| 12 | `apps/gnostr/tests/test_event_item_txn_budget.c` | 7 | Transaction budgets | No |
| 13 | `apps/gnostr/tests/integ/test_model_delete_authorization.c` | 6 | NIP-09 delete | No |
| 14 | `benchmark/gnostr_bench_memory_timeline_scroll.c` | 1 | Memory ceiling | Yes |
| — | **Total** | **~74** | | |

---

## Adding New Tests

### Pattern for nostr-gobject tests

```c
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/your_header.h>

static void test_your_feature(void) {
  GnTestNdb *ndb = gn_test_ndb_new(NULL);
  // ... test code ...
  gn_test_ndb_free(ndb);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-gobject/your-feature/test-name", test_your_feature);
  return g_test_run();
}
```

```cmake
gn_add_gtest(nostr-gobject-test-your-feature
  SOURCES tests/test_your_feature.c
  LIBS gnostr-testkit nostr_gobject PkgConfig::GLIB
)
```

### Pattern for nostr-gtk widget tests

```c
#include <gtk/gtk.h>

int main(int argc, char *argv[]) {
  gtk_test_init(&argc, &argv, NULL);  // NOT g_test_init
  g_test_add_func("/nostr-gtk/your-widget/test-name", test_your_widget);
  return g_test_run();
}
```

```cmake
gn_add_gtest_xvfb(nostr-gtk-test-your-widget
  SOURCES tests/test_your_widget.c
  LIBS gnostr-testkit nostr_gtk nostr_gobject PkgConfig::GTK4
)
```

### Pattern for timing-sensitive tests

```c
#if defined(__SANITIZE_ADDRESS__) || ...
#  define SANITIZER_SLOWDOWN 10
#else
#  define SANITIZER_SLOWDOWN 1
#endif
#define TIMING_BUDGET_US(base) ((gint64)(base) * SANITIZER_SLOWDOWN)

static void test_fast_operation(void) {
  gint64 start = g_get_monotonic_time();
  // ... operation ...
  g_assert_cmpint(g_get_monotonic_time() - start, <, TIMING_BUDGET_US(1000));
}
```

---

## Related Documentation

- [`docs/TESTING_ARCHITECTURE.md`](TESTING_ARCHITECTURE.md) — Mock relay testing architecture
- [`docs/BROADWAY_TESTING.md`](BROADWAY_TESTING.md) — Broadway + Playwright browser UI testing
- [`docs/test-scenarios/`](test-scenarios/) — Broadway-specific test scenarios
- [`ARCHITECTURE.md`](../ARCHITECTURE.md) — Project architecture overview
