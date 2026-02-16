# Test Strategy & Suite Documentation

## Overview

The gnostr test suite validates memory safety, performance budgets, widget correctness, event flow integrity, and zero-copy sliding window invariants across the three library layers (`nostr-gobject`, `nostr-gtk`) and the application (`apps/gnostr`).

### Architecture

```
┌──────────────────────────────────────────┐
│  Real-Component Benchmarks (nightly)     │  ← RSS ceiling with real NDB data
├──────────────────────────────────────────┤
│  Real-Component Integration (CI per-PR)  │  ← NDB violation detection, real bind
├──────────────────────────────────────────┤
│  Mock-Based Integration (CI per-PR)      │  ← Event flow, delete authorization
├──────────────────────────────────────────┤
│  Widget Tests (Xvfb, CI per-PR)          │  ← Recycling stress, sizing, latency
├──────────────────────────────────────────┤
│  Unit Tests (no display, fast)           │  ← Store contract, cache bounds, lifecycle
└──────────────────────────────────────────┘
```

> **Key distinction**: The bottom 3 layers use mock objects and test individual
> mechanisms. The top 2 layers use the **real** `GnNostrEventItem`, `GnNostrEventModel`,
> `NoteCardFactory`, and a real temporary NDB — catching the architectural violations
> that mocks hide (main-thread NDB transactions, lazy loading during bind, etc.).

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
| 9 | `nostr-gtk/tests/test_bind_latency_budget.c` | 2 | UI latency (mock) | Yes |
| 10 | `nostr-gtk/tests/test_thread_view_bounded_load.c` | 5 | Sliding windows | Yes |
| 11 | `apps/gnostr/tests/test_event_model_windowing.c` | 11 | Sliding windows | No |
| 12 | `apps/gnostr/tests/test_event_item_txn_budget.c` | 7 | Transaction budgets | No |
| 13 | `apps/gnostr/tests/integ/test_model_delete_authorization.c` | 6 | NIP-09 delete | No |
| 14 | `benchmark/gnostr_bench_memory_timeline_scroll.c` | 1 | Memory ceiling (mock) | Yes |
| 15 | `apps/gnostr/tests/test_ndb_main_thread_violations.c` | 5 | **NDB violation detection** | No |
| 16 | `apps/gnostr/tests/test_real_bind_latency.c` | 2 | **Real-component latency** | Yes |
| 17 | `benchmark/gnostr_bench_real_memory.c` | 1 | **Real-component memory** | Yes |
| — | **Total** | **~82** | | |

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

## Real-Component Integration Tests

### The Problem with Mocks

The original test suite used `MockEventItem` objects with static string properties.
These mocks exercise the GTK recycling mechanism correctly but **skip the actual
code paths that cause bugs**: NDB transactions, Pango content rendering, async HTTP
fetches, and widget lifecycle complexity.

| Real Component | What Mocks Skip |
|---|---|
| `GnNostrEventItem` + lazy NDB load | NDB transactions on main thread, content copying |
| `NostrGtkNoteCardRow` (6300+ lines) | Pango layout, async HTTP, GStreamer video |
| `storage_ndb_begin_query_retry` | `usleep()` blocking the main loop |
| `gnostr_render_content()` | Synchronous content parsing |

### Main-Thread NDB Violation Detection

The core architectural test uses **instrumentation** in `storage_ndb.c` (enabled via
`-DGNOSTR_TESTING`) to detect when NDB read transactions are opened on the main thread.

```c
// In your test setUp:
gn_test_mark_main_thread();
gn_test_reset_ndb_violations();

// Exercise the code path...
const char *content = gn_nostr_event_item_get_content(item);

// Assert zero violations:
gn_test_assert_no_ndb_violations("during item property access");
```

On failure, the assertion dumps diagnostic output:

```
╔════════════════════════════════════════════════════╗
║ MAIN-THREAD NDB VIOLATIONS: 3 during bind
╠════════════════════════════════════════════════════╣
║  [0] storage_ndb_begin_query
║  [1] storage_ndb_begin_query_retry
║  [2] storage_ndb_begin_query
╚════════════════════════════════════════════════════╝

FIX: Move NDB transactions to a GTask worker thread.
```

### Key Real-Component Tests

| Test | File | What It Catches |
|------|------|-----------------|
| NDB violations during item access | `test_ndb_main_thread_violations.c` | Lazy NDB loads on main thread |
| NDB violations during model refresh | Same | Model queries on main thread |
| NDB violations during batch metadata | Same | Batch reaction/zap queries on main thread |
| Real bind with NDB violation counter | `test_real_bind_latency.c` | NDB txns during GtkListView bind |
| Real bind with heartbeat stall detection | Same | Actual UI stalls during scroll |
| Real NDB memory benchmark | `gnostr_bench_real_memory.c` | Memory with realistic content corpus |

### Testkit Enhancements

The testkit now includes:

- **`gn_test_mark_main_thread()`** — Mark current thread for NDB violation detection
- **`gn_test_assert_no_ndb_violations(ctx)`** — Assert zero violations with diagnostics
- **`gn_test_ingest_realistic_corpus(ndb, n_events, n_profiles)`** — Ingest varied content
- **`gn_test_make_realistic_event_json(kind, style, ts)`** — Content styles: short, medium, long, unicode, URLs, mentions
- **`gn_test_make_profile_event_json(pk, name, about, pic, ts)`** — Kind-0 profiles
- **`gn_test_heartbeat_start/stop(hb, interval_ms, max_stall_ms)`** — Stall detection

### How to Use This for Iterative Bug Fixing

The test harness is designed for LLM-assisted iterative development:

1. **Run `gnostr-test-ndb-main-thread-violations`** — It will FAIL with a list of functions calling NDB on the main thread
2. **Fix one violation at a time** — Move the NDB call to a `g_task_run_in_thread()` worker
3. **Re-run the test** — The violation count decreases
4. **Repeat until zero violations** — The test passes

The violation counter is **deterministic** — it doesn't depend on timing, system load, or randomness. Each violation is a concrete function call that must be moved off the main thread.

---

## LLM Closed-Loop Debugging

This section describes how an LLM agent can perform complete debug cycles — identify
bugs, reproduce them, diagnose root causes, apply fixes, and verify corrections —
without human intervention between iterations.

Detailed skill documents are in [`skills/`](../skills/):

| Skill | File | Purpose |
|-------|------|---------|
| **Closed-Loop Debug** | [`skills/closed-loop-debug/SKILL.md`](../skills/closed-loop-debug/SKILL.md) | Full IDENTIFY → REPRO → DIAGNOSE → FIX → VERIFY workflow |
| **Broadway + Playwright** | [`skills/broadway-debug/SKILL.md`](../skills/broadway-debug/SKILL.md) | UI debugging via Broadway HTML5 backend |
| **GDB / LLDB** | [`skills/gdb-debug/SKILL.md`](../skills/gdb-debug/SKILL.md) | Memory errors, segfaults, reference counting |

### The Debug Loop

```
    ┌──────────────────────────────────────────────────────────┐
    │                                                          │
    ▼                                                          │
┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐
│ IDENTIFY │───►│ REPRO   │───►│ DIAGNOSE│───►│  FIX    │───►│ VERIFY  │
│ (test/   │    │ (test + │    │ (GDB/   │    │ (edit   │    │ (test + │
│  report) │    │  ASan)  │    │  LLDB/  │    │  code)  │    │  Bway)  │──┐
└─────────┘    └─────────┘    │  ASan)  │    └─────────┘    └─────────┘  │
                               └─────────┘                        │       │
                                                                  │ PASS  │ FAIL
                                                                  ▼       │
                                                               DONE ◄─────┘
```

### Broadway Persistent Sessions

The Broadway daemon (`gtk4-broadwayd`) persists independently of the app process.
This means the browser tab and Playwright MCP connection survive across app rebuilds:

```bash
# 1. Start daemon once (stays running)
./skills/broadway-debug/scripts/run-broadway.sh

# 2. Connect Playwright
# browser_navigate(url="http://127.0.0.1:8080")

# 3. Debug loop — daemon stays, browser stays:
#    Edit code → cmake --build build → relaunch app → browser_snapshot()
```

Any GTK4 app in the nostrc stack works with Broadway:

| App | Launch Command |
|-----|---------------|
| gnostr | `GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 GSETTINGS_SCHEMA_DIR=build/apps/gnostr build/apps/gnostr/gnostr` |
| gnostr-signer | `GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 GSETTINGS_SCHEMA_DIR=build/apps/gnostr-signer build/apps/gnostr-signer/gnostr-signer` |

### GTK Test Utilities vs Playwright

For automated tests, prefer GTK's built-in test utilities over Playwright DOM interaction:

| GTK Test Utility | Playwright Equivalent | When to Use GTK |
|------------------|-----------------------|-----------------|
| `gtk_widget_measure()` | Screenshot + pixel comparison | Widget sizing validation |
| `gtk_test_widget_wait_for_draw()` | `browser_wait_for()` | Waiting for layout completion |
| `gtk_test_accessible_assert_role()` | `browser_snapshot()` + parse | Accessibility contract tests |
| `g_signal_emit()` | `browser_click()` | Deterministic signal testing |

GTK test utilities are deterministic, run headless (via Xvfb), and don't need a
Broadway daemon. Use Playwright for **visual verification** and **interaction flow
debugging**, GTK utilities for **automated regression tests**.

### Debugger Integration

#### GDB (Linux) — batch mode for LLM agents

```bash
gdb -batch \
  -ex "set pagination off" \
  -ex "set print pretty on" \
  -ex "run" \
  -ex "bt full" \
  -ex "thread apply all bt 10" \
  --args build-debug/apps/gnostr/tests/FAILING_TEST 2>&1
```

#### LLDB (macOS) — batch mode for LLM agents

```bash
lldb -b \
  -o "run" \
  -o "bt all" \
  -k "bt all" \
  -k "quit" \
  -- build-debug/apps/gnostr/tests/FAILING_TEST 2>&1
```

#### ASan (cross-platform) — self-diagnosing memory errors

```bash
ASAN_OPTIONS=detect_leaks=1 \
  build-asan/apps/gnostr/tests/FAILING_TEST 2>&1
```

ASan reports are **self-contained diagnoses**: they show WHERE the crash happened,
WHERE the memory was allocated, WHERE it was freed, and WHICH threads were involved.
No interactive debugger session needed for most UAF/leak issues.

### GLib Debug Environment

All tests automatically set these, but for manual debugging:

| Variable | Value | Purpose |
|----------|-------|---------|
| `G_DEBUG` | `fatal-warnings,gc-friendly` | Abort on warnings; GC-friendly for leak detection |
| `G_SLICE` | `always-malloc` | Disable slab allocator (required for ASan) |
| `GOBJECT_DEBUG` | `objects` | Track all GObject instances (heavy but thorough) |
| `GTK_DEBUG` | `interactive` | Open GTK Inspector in Broadway session |

### GTK Inspector

The GTK Inspector is a built-in interactive debugger for GTK4 — widget tree
introspection, live CSS editing, GObject property/signal monitoring, rendering
performance analysis, and layout debugging. **It runs in the same Broadway session
as the app**, so Playwright can interact with both.

```bash
# Launch any app with Inspector
GTK_DEBUG=interactive GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build/apps/gnostr \
  build/apps/gnostr/gnostr
```

| Inspector Panel | Debugging Use |
|-----------------|---------------|
| **Objects** | Widget tree navigation, GObject property inspection, signal handler auditing, reference count monitoring |
| **CSS** | Live CSS editing to test layout fixes without recompiling |
| **Visual** | Box model (margins/borders/padding/content), allocation bounds, baseline alignment |
| **Accessibility** | Verify accessible roles/labels match Playwright `browser_snapshot()` output |
| **Statistics** | Frame rate, frame times (stall detection), texture memory, CSS node count |
| **Recorder** | Frame-by-frame render analysis, identify redundant redraws and layout cascades |

> **📖 Full guide**: [`skills/gtk-inspector/SKILL.md`](../skills/gtk-inspector/SKILL.md) —
> LLM-actionable workflows for each panel, Playwright-based interaction with the
> inspector, programmatic usage in tests and GDB sessions, and 4 debugging scenarios
> (widget sizing, signal leaks, memory growth, layout performance).

### Strategy by Bug Class

| Bug Class | Identify | Reproduce | Diagnose | Verify | Inspector Panel |
|-----------|----------|-----------|----------|--------|-----------------|
| **Segfault** | ASan build crashes | `ctest -R recycle` | ASan trace → freed-by location | Test passes under ASan | Objects (signals) |
| **Memory leak** | `ASAN_OPTIONS=detect_leaks=1` | Lifecycle test | LSAN trace → allocation site | `gn_test_assert_finalized()` | Objects (ref count) |
| **Main-thread block** | UI stalls visibly | `ctest -R ndb-violations` | Violation dump → function name | Violation count = 0 | Statistics (frame times) |
| **Widget sizing** | Broadway screenshot | `ctest -R note_card_measure` | Inspector Visual panel + CSS | Dimensions within bounds | **Visual + CSS** |
| **Signal race** | Random crash on scroll | Recycle stress test | Inspector Objects → Signals section | No ASan errors after 50 cycles | **Objects (Signals)** |
| **Latency** | Broadway feels slow | Heartbeat test + NDB violations | Inspector Statistics + Recorder | Heartbeat missed count = 0 | **Statistics + Recorder** |

---

## Related Documentation

- [`docs/TESTING_ARCHITECTURE.md`](TESTING_ARCHITECTURE.md) — Mock relay testing architecture
- [`docs/BROADWAY_TESTING.md`](BROADWAY_TESTING.md) — Broadway + Playwright browser UI testing
- [`docs/test-scenarios/`](test-scenarios/) — Broadway-specific test scenarios
- [`skills/`](../skills/) — LLM skill documents for debug workflows:
  - [`skills/broadway-debug/`](../skills/broadway-debug/SKILL.md) — Broadway + Playwright UI debugging
  - [`skills/gtk-inspector/`](../skills/gtk-inspector/SKILL.md) — GTK Inspector debugging (widget tree, CSS, signals, performance)
  - [`skills/gdb-debug/`](../skills/gdb-debug/SKILL.md) — GDB/LLDB debugging
  - [`skills/closed-loop-debug/`](../skills/closed-loop-debug/SKILL.md) — Full closed-loop debug workflow
- [`ARCHITECTURE.md`](../ARCHITECTURE.md) — Project architecture overview
