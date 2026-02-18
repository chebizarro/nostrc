# GLib Memory Management Audit

**Date**: 2026-02-17
**Scope**: nostr-gobject, nostr-gtk, apps/gnostr, apps/gnostr-signer, marmot-gobject

## Executive Summary

The codebase is **functionally correct** — no active memory leaks were found in
dispose/finalize methods, error paths, or signal handler management. However,
adoption of modern GLib memory management idioms (g_autoptr, g_autofree,
g_clear_object, g_clear_pointer) is very low in several areas, creating
**unnecessary risk** on error paths and making the code harder to review for
correctness.

**Verdict**: No must-fix bugs. Several should-fix patterns for risk reduction,
and a large modernization opportunity for consistency.

---

## Metrics

| Area | g_free | g_autoptr | g_autofree | g_clear_{object,pointer} | g_object_unref | Ratio (free:autoptr) |
|------|--------|-----------|------------|--------------------------|----------------|---------------------|
| **nostr-gobject/src/** | 498 | 60 | — | common | 103 | 8:1 |
| **nostr-gtk/src/** | — | 17 | — | common | 96 | — |
| **apps/gnostr/src/** | 3,167 | 38 | 104 | 461 total | 779 | 83:1 |
| **apps/gnostr-signer/src/** | — | — | 8 | — | — | ~93% manual |
| **marmot-gobject/** | 136* | 0 | 0 | 2 | — | ∞ |

*g_free + g_object_unref combined

### g_strdup_printf Usage (leak risk indicator)

| Area | Total calls | Using g_autofree | Manual g_free | % Modern |
|------|------------|------------------|---------------|----------|
| nostr-gobject/src/ | 25 | 3 | 22 | 12% |
| nostr-gtk/src/ | 62 | 13 | 49 | 21% |
| apps/gnostr/src/ui/ | 67 | 13 | 54 | 19% |
| apps/gnostr-signer/src/ | 109 | 8 | 101 | 7% |
| marmot-gobject/ | 0 | 0 | 0 | N/A |

---

## Findings by Area

### 1. nostr-gobject (Library) — ✅ Good

**dispose/finalize**: All 22 GObject types have proper cleanup methods.

- `nostr_pool.c`: Exemplary — uses `g_clear_pointer`, `g_clear_object`, proper
  signal handler disconnect via `relay_handler_ids` hash table
- `nostr_subscription.c`: Clean finalize with `g_clear_object` for relay
- `nostr_relay.c`: Thorough — dispatches `nostr_relay_free` to background thread
  to avoid blocking main loop during finalize
- `nostr_event_bus.c`: Clean — destroys hash tables, clears mutex
- `gnostr-sync-service.c`: Proper — `g_source_remove` for timers, `g_clear_object`
  for cancellable
- `nostr_subscription_registry.c`: Separate dispose (signal disconnects) + finalize
  (data cleanup)

**g_strdup_printf**: 25 calls across 7 files. All checked — properly freed on
both success and error paths. Example patterns:
- `storage_ndb.c`: 8 calls, all `g_free`'d immediately after `storage_ndb_query`
- `nostr_simple_pool.c`: 4 calls in callbacks, freed after `event_bus_emit`
- `nostr_ndb_store.c`: 3 calls with proper cleanup on query failure

**Signal handlers**: Properly tracked in `relay_handler_ids` hash table with
explicit `g_signal_handler_disconnect` on remove.

**Risk**: Low. The library is well-written.

### 2. nostr-gtk (Library) — ✅ Excellent

The GTK widget dispose methods are the most carefully written in the entire
codebase, reflecting the complexity of GTK4 widget lifecycle management.

- `nostr-note-card-row.c`: **200+ line dispose method** with:
  - `disposed` flag to prevent double-dispose
  - Cancels 5+ GCancellable instances
  - Stops video players before template disposal (prevents GStreamer/Pango crash)
  - DISPOSE_LABEL macro with Pango SEGV prevention
  - Proper popover popdown before unparenting
  - Signal handler disconnects
- `gnostr-profile-pane.c`: Cancels ~8 cancellables, clears models, hash tables,
  and LRU queues
- `gnostr-thread-view.c`: Marks disposed, tears down subscription, clears 5 hash
  tables, frees thread graph

**Risk**: Very low. These are battle-tested widgets.

### 3. apps/gnostr (Main App) — ⚠️ Low Adoption

**dispose**: `gnostr-main-window.c` has a thorough 170-line dispose:
- Timed thread join for ingest thread (2s deadline)
- Profile provider unwatch
- Source removal for idle/timeout callbacks
- Signal handler disconnects before pool unref
- Hash table cleanup

**Pattern issues** (not bugs, but risk):

1. **Manual if/unref/NULL instead of g_clear_object** (lines 7470-7475):
   ```c
   // Current pattern (3 lines, error-prone):
   if (self->profile_fetch_cancellable) {
     g_object_unref(self->profile_fetch_cancellable);
     self->profile_fetch_cancellable = NULL;
   }
   
   // Preferred (1 line, NULL-safe):
   g_clear_object(&self->profile_fetch_cancellable);
   ```

2. **g_strdup_printf without g_autofree** (safe but fragile):
   Most follow the safe "allocate → use → free" pattern with no code between
   allocation and free that could throw/return. However, using `g_autofree`
   would be more resilient to future code changes.

3. **83:1 g_free:g_autoptr ratio**: Indicates very few functions use scope-based
   cleanup. This is a consistency issue, not a bug — the cleanup is done manually.

### 4. apps/gnostr-signer — ⚠️ Low Adoption but Secure Handling

**Secret data paths**: Excellent. Uses:
- `gn_secure_alloc()` / `gn_secure_strfree()` for private keys
- `gn_secure_clear_buffer()` for temporary key material
- `secure_wipe()` before `g_free()` for password hashes
- All error paths in `secret_store_add()` properly clean up secure memory

**GObject types**: `GnClientSession`, `GnClientSessionManager`, `GnSessionManager`
all have correct finalize methods with proper timer removal and hash table cleanup.

**Pattern issues**:
- 101 of 109 `g_strdup_printf` calls use manual `g_free`
- No `g_autoptr` usage in any signer source file

### 5. marmot-gobject (GObject Wrapper) — ⚠️ Should Modernize

As the newest code in the project, this should be the easiest to modernize:

- **0 uses of g_autoptr, g_autofree, or g_clear_pointer** in any source file
- **finalize methods use bare g_free** instead of `g_clear_pointer`:
  ```c
  // Current (marmot-gobject-group.c):
  g_free(self->mls_group_id_hex);
  g_free(self->nostr_group_id_hex);
  g_free(self->name);
  
  // Preferred:
  g_clear_pointer(&self->mls_group_id_hex, g_free);
  g_clear_pointer(&self->nostr_group_id_hex, g_free);
  g_clear_pointer(&self->name, g_free);
  ```
- Client finalize does use `g_clear_object(&self->storage)` — inconsistent

### 6. libmarmot (Pure C) — N/A

Uses standard malloc/free. Not GLib-dependent by design. No issues.

### 7. G_DEFINE_AUTOPTR_CLEANUP_FUNC Declarations

All GObject types in nostr-gobject use `G_DECLARE_FINAL_TYPE` or
`G_DECLARE_DERIVABLE_TYPE`, which automatically generate autoptr cleanup.
This means `g_autoptr(GNostrRelay)` etc. work correctly — the infrastructure
is in place, it's just underutilized by calling code.

---

## Recommendations

### Should-Fix (Risk Reduction)

These changes reduce risk of leaks from future code modifications:

#### S1. marmot-gobject: Convert finalize to g_clear_pointer

**Files**: `marmot-gobject-group.c`, `marmot-gobject-message.c`, `marmot-gobject-welcome.c`
**Effort**: ~30 min
**Impact**: Prevents double-free if finalize is accidentally called twice

#### S2. gnostr-main-window.c: Use g_clear_object for cancellables

**Files**: `apps/gnostr/src/ui/gnostr-main-window.c` (lines 7470-7475)
**Effort**: ~15 min
**Impact**: Reduces 3-line patterns to 1-line, eliminates manual NULL assignment

#### S3. storage_ndb.c: Convert g_strdup_printf to g_autofree

**Files**: `nostr-gobject/src/storage_ndb.c` (8 instances)
**Effort**: ~20 min
**Impact**: Makes functions resilient to added early-return paths

### Nice-to-Have (Modernization)

These improve consistency and review-ability but don't fix bugs:

#### N1. Systematic g_autofree for g_strdup_printf

Convert the ~226 manual g_strdup_printf → g_free patterns to use `g_autofree`.
Priority files (most allocations):
1. `apps/gnostr-signer/src/` — 101 instances
2. `apps/gnostr/src/ui/gnostr-main-window.c` — 13 instances
3. `nostr-gtk/src/nostr-note-card-row.c` — multiple instances

#### N2. g_autoptr for GObject locals

Functions that create GObject instances, use them, and unref should use
`g_autoptr`:
```c
// Current:
GNostrEvent *ev = gnostr_event_new_from_json(json, &err);
if (!ev) return;
// ... use ev ...
g_object_unref(ev);

// Modern:
g_autoptr(GNostrEvent) ev = gnostr_event_new_from_json(json, &err);
if (!ev) return;
// ... use ev ... (auto-unreffed on scope exit)
```

#### N3. Convert if/unref/NULL to g_clear_object

Grep: `if (self->\w+) { g_object_unref(self->`
Estimated ~100+ instances across the codebase.

---

## No Issues Found

The following areas were specifically checked and found correct:

1. **dispose/finalize completeness**: All GObject types have matching cleanup
2. **Signal handler leaks**: All connected signals are tracked and disconnected
3. **GCancellable lifecycle**: All cancellables are cancelled in dispose
4. **Async callback safety**: GTask used correctly with destroy notifiers
5. **Secret data cleanup**: Secure wipe before free on all paths
6. **Error path cleanup**: Functions with g_strdup_printf all free on error paths
7. **Hash table ownership**: Proper free_func set on construction
8. **GVariant leaks**: Proper g_variant_unref on all paths checked
9. **Thread lifecycle**: Ingest thread has timed join; relay free dispatched to
   background thread
