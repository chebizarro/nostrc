# Duplicate Item in GListModel — Root Cause Analysis

**Date**: 2026-02-19  
**Tag**: `nostrc-duplicate-fix2`  
**Symptom**: ~100 GTK warnings "Duplicate item detected in list. Picking one randomly." followed by segfault on Linux

## Root Cause

The `on_paginate_async_done()` function in `gn-nostr-event-model.c` emitted semantically incorrect `items_changed` signals during async pagination (load-older / load-newer).

### How GTK's `items_changed(position, removed, added)` Works

The signal tells GTK:
- Starting at `position`, `removed` items from the old model were removed
- `added` new items were inserted at `position` in their place
- Items after `position + removed` in the old model shift to `position + added`

GTK uses this to maintain an internal item→widget mapping. If the signal doesn't match reality, GTK's mapping becomes corrupted: the same GObject pointer appears at multiple positions, causing "Duplicate item detected" warnings and eventual segfaults.

### The Bug: `trim_newer=TRUE` (Load Older)

When scrolling down triggers `load_older_async`:

1. **New items are appended at the END** of the `notes` array:
   ```
   Before: [A₀, A₁, ..., A₉₉]  (100 items)
   After:  [A₀, A₁, ..., A₉₉, B₀, ..., B₂₉]  (130 items)
   ```

2. **Old items are trimmed from the FRONT** (to maintain window of 200):
   ```
   After trim: [A₃₀, ..., A₉₉, B₀, ..., B₂₉]  (100 items)
   ```

3. **Signal emitted was**: `items_changed(0, 30, 30)` — "30 items removed from position 0, 30 new items added at position 0"

4. **GTK interprets this as**:
   - Positions 0–29: **new items** (GTK calls `get_item()` for these)
   - Positions 30–99: surviving old items A₃₀...A₉₉ (GTK keeps existing widgets)

5. **But the actual model is**:
   - Positions 0–69: surviving old items A₃₀...A₉₉
   - Positions 70–99: new items B₀...B₂₉

6. **Crash mechanism**: GTK calls `get_item(0)` expecting a new item but gets A₃₀ (a surviving old item that GTK already has a widget for at position 30). Same GObject pointer at two positions → **"Duplicate item detected"**. ~100 warnings fire in rapid succession as GTK processes all positions, then a segfault as the corrupted widget mapping leads to double-disposal.

### The Bug: `trim_newer=FALSE` (Load Newer) with Trimming

Similar issue but reversed: items prepended at position 0, old items trimmed from the back. The signal `items_changed(0, effective_removed, effective_added)` used arithmetic that didn't correctly map to the actual model state.

## Fix

Replace the incorrect per-case signal logic with a single "replace all" signal:

```c
// Before (WRONG):
if (trim_newer) {
    emit_items_changed_safe(self, 0, trimmed, added);
} else {
    guint effective_added = added > trimmed ? added - trimmed : 0;
    guint effective_removed = trimmed > added ? trimmed - added : 0;
    emit_items_changed_safe(self, 0, effective_removed, effective_added);
}

// After (CORRECT):
g_hash_table_remove_all(self->item_cache);
g_queue_clear_full(self->cache_lru, g_free);
emit_items_changed_safe(self, 0, old_len, new_len);
```

The "replace all" signal `items_changed(0, old_len, new_len)` tells GTK the entire model changed. GTK discards all existing item→widget mappings and re-queries every position. This is the same pattern used by:
- Sync `load_older()` / `load_newer()`
- `refresh()` and `refresh_async()`
- `clear()`

The `item_cache` is cleared before the signal to ensure `get_item()` creates fresh GnNostrEventItem objects for every position, preventing stale cached items from causing duplicate pointers.

## Defense in Depth

A debug-only assertion was added to `get_item()` that scans for duplicate `note_key` values in the `notes` array. This O(N) check only runs in `NDEBUG`-disabled builds and logs a `g_critical()` if the same note_key appears at multiple positions.

## Why the Signal Was Wrong

The original code tried to emit position-accurate signals to avoid the "replace all" pattern, which was believed to cause mass widget disposal and Pango layout corruption. However, the position arithmetic was fundamentally flawed:

- For `trim_newer`: items added at the END, trimmed from the FRONT, but signal claims items added at position 0
- For `trim_newer=FALSE`: items added at position 0, trimmed from the END, but signal uses `effective_added = added - trimmed` which loses positional accuracy

The "replace all" signal is the only safe option when both additions and removals affect different ends of the array.

## Files Changed

- `apps/gnostr/src/model/gn-nostr-event-model.c`:
  - Fixed `on_paginate_async_done()` signal emission
  - Added debug duplicate-detection in `get_item()`
