# NoteCardRow Lifecycle Deep Analysis

**Date**: 2026-02-18
**Scope**: `nostr-gtk/src/nostr-note-card-row.c` (6175 lines, 120+ fields) + `apps/gnostr/src/ui/note-card-factory.c` (788 lines)

## Executive Summary

The `NostrGtkNoteCardRow` is a 6175-line GtkWidget with 120+ instance fields that
suffers from **persistent crashes during event card lifecycle transitions** (bind →
unbind → re-bind) in the GtkListView recycling loop. The root cause is a
**fundamental architectural mismatch**: the widget uses raw `self` pointers in
async callbacks protected only by a `gboolean disposed` flag, but the GtkListView
factory can recycle (unbind + re-bind) the same widget at any point, creating a
window where:

1. An async callback fires with a raw pointer to a recycled row
2. The `disposed` flag was already reset by `prepare_for_bind()`
3. The callback modifies a widget that now represents a *different event*

The existing patchwork of `disposed` checks, `binding_id` counters, and mixed
ownership patterns has grown organically into an unmaintainable state with at
least **8 distinct crash vectors**. A durable solution requires restructuring
async callback ownership around GLib's ref-counted primitives.

---

## 1. Widget Lifecycle in GtkListView

```
                    GtkSignalListItemFactory
                    ┌────────────────────────────┐
  setup ────────────│ Creates NoteCardRow widget  │
                    │ Connects signal handlers    │
                    └───────────┬────────────────┘
                                │
  bind ─────────────│ prepare_for_bind()          │  ← disposed=FALSE, binding_id++
                    │ Populate Tier 1 (name, ts)  │
                    │ Connect Tier 2 map handler  │
                    │ Store "bound-item" as qdata │  ← RAW POINTER, no ref
                    └───────────┬────────────────┘
                                │
  [mapped] ─────────│ on_ncf_row_mapped_tier2()   │  ← Tier 2: avatar, NIP-05, media
                    │ Starts async HTTP fetches   │     Each gets raw `self` ptr
                    │ Connects notify::profile    │     as user_data
                    └───────────┬────────────────┘
                                │
        ┌───── (async callbacks may fire at ANY point during this window) ─────┐
        │                                                                       │
  unbind ───────────│ Disconnect profile handler  │
                    │ Clear "bound-item" qdata    │
                    │ prepare_for_unbind()        │  ← disposed=TRUE, cancel ops
                    └───────────┬────────────────┘
                                │
  [re-bind] ────────│ prepare_for_bind()          │  ← disposed=FALSE again!
                    │ New event, same widget      │     binding_id incremented
                    └────────────────────────────┘
```

**Critical race window**: Between `g_cancellable_cancel()` in `prepare_for_unbind`
and the actual delivery of the cancellation to each async operation, callbacks
may still fire. If the widget has already been re-bound (`disposed=FALSE` again),
the `disposed` check passes and the stale callback modifies the wrong event's
widget state.

---

## 2. Crash Vector Inventory

### Vector 1: `on_avatar_http_done` — Raw pointer + disposed flag (CRITICAL)

**Location**: L2041-2064
```c
static void on_avatar_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  if (user_data == NULL) return;
  NostrGtkNoteCardRow *self = (NostrGtkNoteCardRow *)user_data;
  if (self->disposed) return;  // ← UNSAFE: disposed may be FALSE after re-bind
  if (!NOSTR_GTK_IS_NOTE_CARD_ROW(self)) return;
  // ... modifies self->avatar_image ...
}
```

**Problem**: `user_data` is a raw `NostrGtkNoteCardRow*` passed when avatar HTTP
fetch was started. If the row is recycled (unbind → re-bind) before HTTP
completes, `self->disposed` is FALSE again, and the callback sets the avatar
of the *new* event to the *old* event's avatar image.

**Worse**: If the widget was actually freed (teardown path), the raw pointer
is dangling and dereferencing `self->disposed` is undefined behavior.

**Contrast with media images**: `on_media_decode_done` (L2132) correctly uses
`GWeakRef` via `MediaLoadCtx` — if the widget is destroyed, `g_weak_ref_get()`
returns NULL. Avatar loading doesn't get this protection.

### Vector 2: `LazyLoadContext` — Raw `self` + raw `picture` pointer (CRITICAL)

**Location**: L2295-2306
```c
typedef struct {
  NostrGtkNoteCardRow *self;   // ← RAW pointer, can become stale
  GtkPicture *picture;          // ← Also raw (weak_ref only via g_object_weak_ref)
  char *url;
  guint timeout_id;
  gulong map_handler_id;
  gulong unmap_handler_id;
  gboolean loaded;
} LazyLoadContext;
```

**Problem**: `ctx->self` is a bare pointer. `on_lazy_load_timeout` (L2327) checks
`ctx->self->disposed` but this dereferences through a potentially-stale pointer.
The `picture` field uses `g_object_weak_ref` (not `GWeakRef`) which is slightly
better but still requires the destroy callback to fire before the pointer is
used.

**Race scenario**:
1. Picture widget is created during bind, lazy load context allocated
2. Row is unbound → `quiesce()` runs, but lazy load timeout is NOT cancelled
   (only if picture is destroyed or timeout fires naturally)
3. Row is re-bound with new event
4. Lazy load timeout fires → `ctx->self` points to recycled row with `disposed=FALSE`
5. `load_media_image_internal()` fetches the OLD event's media URL and loads it
   into whatever picture widget now exists

**Note**: `quiesce()` does NOT clean up lazy load contexts. It cancels
`media_cancellables` but the LazyLoadContext's timeout source is separate.

### Vector 3: `on_article_image_loaded` — Raw `self` pointer (HIGH)

**Location**: L4776-4814
```c
static void on_article_image_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  NostrGtkNoteCardRow *self = NOSTR_GTK_NOTE_CARD_ROW(user_data);  // ← TYPE CHECK on raw ptr
  if (!NOSTR_GTK_IS_NOTE_CARD_ROW(self) || self->disposed) return;
  // ... modifies self->article_image ...
}
```

**Problem**: Uses `NOSTR_GTK_NOTE_CARD_ROW()` cast which dereferences `user_data`
for the GType check. If widget was freed, this is UB. If recycled, `disposed` may
be FALSE again.

### Vector 4: Factory `"bound-item"` qdata — Raw pointer, no ref (HIGH)

**Location**: factory_bind_cb L486
```c
g_object_set_data(G_OBJECT(row), "bound-item", obj);
```

**Problem**: `obj` comes from `gtk_list_item_get_item()` which returns a
**borrowed** reference. The qdata stores it without `g_object_ref()`. If the
GListModel removes this item while the row is still bound, `"bound-item"` becomes
a dangling pointer.

**Impact**: `on_ncf_row_mapped_tier2` (L395) calls:
```c
GObject *obj = g_object_get_data(G_OBJECT(row), "bound-item");
if (!obj || !G_IS_OBJECT(obj)) return;  // ← G_IS_OBJECT dereferences obj
```
If `obj` was freed, `G_IS_OBJECT()` is UB.

### Vector 5: `on_item_profile_changed` — Raw row pointer as user_data (HIGH)

**Location**: L346-394 (factory)
```c
gulong handler_id = g_signal_connect(obj, "notify::profile",
                                      G_CALLBACK(on_item_profile_changed), row);
```

**Problem**: `row` is passed as raw `user_data`. Although `factory_unbind_cb`
disconnects this handler, there's a race window between signal emission
starting (main loop iteration) and unbind completing.

**Mitigation already present**: The callback checks `is_disposed()`, but this
requires dereferencing `row` which could be freed if teardown won the race.

### Vector 6: `on_note_nip05_verified` — Weak reference pattern (MEDIUM)

**Location**: L3728-3762
```c
static void on_note_nip05_verified(GnostrNip05Result *result, gpointer user_data) {
  GObject **weak_ref = (GObject **)user_data;
  NostrGtkNoteCardRow *self = NULL;
  if (weak_ref && *weak_ref) {
    self = NOSTR_GTK_NOTE_CARD_ROW(*weak_ref);
  }
  g_free(weak_ref);
  // ...
}
```

**Analysis**: This uses a manual weak reference pattern (GObject** that gets
NULLed on destroy). This is safer than raw pointers but is a hand-rolled
version of what `GWeakRef` provides. The risk is that the `g_object_add_weak_pointer`
call that backs this must be correctly paired with cleanup.

### Vector 7: Timestamp timer — ref-counted but fragile (LOW)

**Location**: L2560-2610
```c
static gboolean update_timestamp_tick(gpointer user_data) {
  NostrGtkNoteCardRow *self = (NostrGtkNoteCardRow *)user_data;
  if (self->disposed || self->timestamp_timer_id == 0) {
    self->timestamp_timer_id = 0;
    return G_SOURCE_REMOVE;
  }
  // ... uses g_object_ref at timer creation ...
}
```

**Analysis**: This is the **only** callback that correctly holds a `g_object_ref`
on the row (via `timestamp_timer_destroy` as the `GDestroyNotify`). The ref
prevents finalization while the timer is active. However, it still checks
`disposed` manually and has defensive widget-root checks.

### Vector 8: String field corruption during concurrent access (MEDIUM)

**Problem**: 30+ `char *` fields (id_hex, pubkey_hex, avatar_url, content_text,
nip05, author_lud16, etc.) are managed via `g_clear_pointer(&self->field, g_free)`
in finalize and `g_free(self->field); self->field = g_strdup(new_val)` in setters.

During the re-bind race window, an async callback may read `self->pubkey_hex`
while `prepare_for_bind` is resetting it. Since all of this runs on the main
thread, this is sequential (not truly concurrent), but the *logical* race exists:
an old callback reads fields that now belong to a new event.

---

## 3. Why the `disposed` Boolean Is Fundamentally Fragile

The `disposed` flag pattern assumes a **one-directional lifecycle**:

```
active (disposed=FALSE) → quiesced (disposed=TRUE) → finalized
```

But GtkListView recycling creates a **cyclic lifecycle**:

```
active → quiesced → active → quiesced → active → ...
```

When `prepare_for_bind()` resets `disposed = FALSE`, every in-flight async
callback from the *previous* binding suddenly passes the `disposed` check again.

The `binding_id` counter (nostrc-534d) was added as a mitigation, but it's only
checked by setter functions (`set_author`, `set_timestamp`, etc.), NOT by async
callbacks (`on_avatar_http_done`, `LazyLoadContext`, `on_article_image_loaded`).
These callbacks predate the binding_id mechanism and were never updated.

---

## 4. Current Ownership Pattern Inventory

| Pattern | Used By | Safety Level |
|---------|---------|-------------|
| Raw `self` pointer + `disposed` bool | Avatar HTTP, article image, video thumb entry point | ❌ UNSAFE |
| Raw `self` pointer in struct | LazyLoadContext | ❌ UNSAFE |
| `GWeakRef` in struct | MediaLoadCtx (media images) | ✅ SAFE |
| `g_object_weak_ref` callback | VideoThumbCtx, LazyLoadContext (picture only) | ⚠️ PARTIAL |
| Manual `GObject**` weak pointer | NIP-05 verification | ⚠️ PARTIAL |
| `g_object_ref` + GDestroyNotify | Timestamp timer | ✅ SAFE |
| Raw qdata pointer | Factory "bound-item" | ❌ UNSAFE |
| Signal user_data (raw pointer) | Factory profile change handler | ❌ UNSAFE |

**Only 2 out of 8 patterns are fully safe.** The others are various levels of
dangerous, all stemming from the same root cause: lack of proper reference-
counted ownership.

---

## 5. Proposed Solution: Ref-Counted Async Context Architecture

### 5.1 Core Design: `NoteCardBindingContext`

Create a **ref-counted opaque context object** that is the *sole* entity passed
to all async callbacks. This decouples async callback lifetime from widget
lifetime.

```c
/*
 * NoteCardBindingContext: Ref-counted context for a single bind cycle.
 *
 * Created in prepare_for_bind(), invalidated in prepare_for_unbind().
 * Async callbacks hold a strong ref. When the callback fires, it checks
 * ctx->cancelled before proceeding. Even if the widget was recycled,
 * the context's cancelled flag remains TRUE from the old unbind.
 *
 * This eliminates the disposed boolean race entirely: each binding cycle
 * gets its own context object, and old callbacks hold refs to old contexts
 * that are permanently cancelled.
 */
typedef struct _NoteCardBindingContext NoteCardBindingContext;

struct _NoteCardBindingContext {
  gatomicrefcount ref_count;

  /* Weak reference to the owning row — NULL after dispose/teardown */
  GWeakRef row_ref;

  /* Monotonically incrementing binding ID for this cycle */
  guint64 binding_id;

  /* Cancellation flag — set once in unbind, never unset */
  gboolean cancelled;

  /* The GCancellable for this binding cycle's async ops */
  GCancellable *cancellable;
};

/* Create a new context — called from prepare_for_bind */
NoteCardBindingContext *note_card_binding_context_new(NostrGtkNoteCardRow *row);

/* Ref/unref for sharing across callbacks */
NoteCardBindingContext *note_card_binding_context_ref(NoteCardBindingContext *ctx);
void note_card_binding_context_unref(NoteCardBindingContext *ctx);

/* Cancel the context — called from prepare_for_unbind */
void note_card_binding_context_cancel(NoteCardBindingContext *ctx);

/* Safe accessor — returns a strong ref to the row, or NULL if:
 * - context is cancelled (stale callback from previous binding)
 * - row was finalized (GWeakRef returns NULL)
 *
 * Caller must g_object_unref() the returned row when done.
 */
NostrGtkNoteCardRow *note_card_binding_context_get_row(NoteCardBindingContext *ctx);
```

### 5.2 Usage Pattern

**Before (unsafe)**:
```c
// In avatar load:
soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT,
                                  self->avatar_cancellable,
                                  on_avatar_http_done,
                                  self);  // ← RAW POINTER

// In callback:
static void on_avatar_http_done(..., gpointer user_data) {
  NostrGtkNoteCardRow *self = user_data;  // ← MIGHT BE STALE
  if (self->disposed) return;             // ← MIGHT BE FALSE AFTER RE-BIND
}
```

**After (safe)**:
```c
// In avatar load:
soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT,
                                  ctx->cancellable,
                                  on_avatar_http_done,
                                  note_card_binding_context_ref(self->binding_ctx));

// In callback:
static void on_avatar_http_done(..., gpointer user_data) {
  NoteCardBindingContext *ctx = user_data;
  NostrGtkNoteCardRow *self = note_card_binding_context_get_row(ctx);
  if (!self) {
    // Context was cancelled OR widget was finalized — either way, bail
    note_card_binding_context_unref(ctx);
    return;
  }
  // self is a STRONG REF — guaranteed valid for this scope
  // ... do work ...
  g_object_unref(self);
  note_card_binding_context_unref(ctx);
}
```

### 5.3 Lifecycle Integration

```
prepare_for_bind():
  1. If old binding_ctx exists: note_card_binding_context_cancel(old)
     + unref (row's ref to old ctx)
  2. Create new NoteCardBindingContext with fresh cancellable
  3. self->binding_ctx = new ctx (row holds one ref)
  4. All subsequent async operations capture ctx refs

prepare_for_unbind():
  1. note_card_binding_context_cancel(self->binding_ctx)
     → Sets ctx->cancelled = TRUE
     → Calls g_cancellable_cancel(ctx->cancellable)
  2. Clear self->binding_ctx ref (unref)
  3. In-flight callbacks hold their own refs to the OLD context
     → When they fire, note_card_binding_context_get_row() returns NULL
     → They bail out cleanly

dispose():
  1. Calls quiesce() which calls cancel on binding_ctx
  2. gtk_widget_dispose_template() — child widgets die
  3. Row's weak ref in ctx → automatically NULL'd by GWeakRef

finalize():
  1. Remaining string fields freed
  2. binding_ctx unref (should be last ref, ctx freed)
```

### 5.4 Factory Integration

**Replace raw `"bound-item"` qdata with ref'd storage**:

```c
// In factory_bind_cb:
g_object_set_data_full(G_OBJECT(row), "bound-item",
                       g_object_ref(obj),           // ← TAKE A REF
                       g_object_unref);              // ← RELEASE ON CLEAR

// In factory_unbind_cb:
g_object_set_data(G_OBJECT(row), "bound-item", NULL); // triggers unref
```

**Replace raw row pointer in profile signal with binding context**:

```c
// In on_ncf_row_mapped_tier2:
NoteCardBindingContext *ctx = nostr_gtk_note_card_row_get_binding_ctx(card);
gulong handler_id = g_signal_connect_data(
    obj, "notify::profile",
    G_CALLBACK(on_item_profile_changed_safe),
    note_card_binding_context_ref(ctx),                // ← ref'd context
    (GClosureNotify)note_card_binding_context_unref,   // ← cleanup
    0);
```

### 5.5 LazyLoadContext Fix

Replace the raw `self` pointer with a binding context ref:

```c
typedef struct {
  NoteCardBindingContext *ctx;  // ← replaces raw self pointer
  GWeakRef picture_ref;         // ← replaces raw picture pointer + weak_ref
  char *url;
  guint timeout_id;
  gulong map_handler_id;
  gulong unmap_handler_id;
  gboolean loaded;
} LazyLoadContext;
```

### 5.6 Benefits

1. **Eliminates the disposed boolean entirely** — No more fragile flag that gets
   reset during recycling. Each binding cycle has its own context with a
   one-directional `cancelled` flag.

2. **No raw pointer dereferences** — `note_card_binding_context_get_row()` uses
   `GWeakRef` internally, so it returns NULL safely if the widget was finalized.

3. **No stale callback corruption** — Even if a callback fires after re-bind,
   it holds a ref to the *old* context (which is cancelled), not the new one.

4. **Composable** — The same pattern works for all async paths: HTTP callbacks,
   timer callbacks, signal handlers, GTask completions.

5. **Self-documenting** — Seeing `note_card_binding_context_ref(ctx)` in a
   callback setup makes ownership explicit. No more guessing which callbacks
   are safe and which aren't.

---

## 6. Migration Plan

### Phase 1: Create NoteCardBindingContext (Low risk, ~200 lines)

1. Add `NoteCardBindingContext` struct and API to a new internal header
   `nostr-gtk/src/note-card-binding-ctx.h`
2. Add `NoteCardBindingContext *binding_ctx` field to `_NostrGtkNoteCardRow`
3. Wire up creation in `prepare_for_bind()` and cancellation in
   `prepare_for_unbind()` / `quiesce()`
4. Keep the existing `disposed` flag and all current guards — this is additive
5. Add `nostr_gtk_note_card_row_get_binding_ctx()` accessor

### Phase 2: Migrate async callbacks one-by-one (Medium risk, ~300 lines)

Migrate in order of severity:

1. **`on_avatar_http_done`** — Replace raw `self` with binding_ctx ref
2. **`on_article_image_loaded`** — Same pattern
3. **`LazyLoadContext`** — Replace raw self + picture with ctx + GWeakRef
4. **`on_note_nip05_verified`** — Replace manual weak pointer with ctx
5. **`VideoThumbCtx`** entry point — Already uses weak ref for picture,
   add ctx for the row reference

Each migration is independently testable:
- Build and run with ASan to verify no leaks
- Fast-scroll test in timeline to trigger recycling races
- Verify with `G_DEBUG=gc-friendly` that finalizers fire

### Phase 3: Factory safety (Medium risk, ~50 lines)

1. Ref the `"bound-item"` qdata via `g_object_set_data_full`
2. Replace raw row pointer in profile signal with binding_ctx ref
3. Validate `on_ncf_row_mapped_tier2` event-ID cross-check covers recycling

### Phase 4: Delegate to binding context (DONE — pragmatic approach)

Rather than removing `disposed` (which would touch 34 call sites), we:

1. Made `is_disposed()` delegate to `binding_ctx->cancelled` as source of truth
2. Made `is_bound()` delegate to `!binding_ctx->cancelled`
3. Kept `disposed` field as a fast-path cache for synchronous setter guards
4. `binding_id` field kept for backward compat (ctx supersedes it)

### Phase 5: NoteCardData — Ref-counted data bucket (DONE — infrastructure)

Created `NoteCardData` ref-counted struct (`note-card-data.h/.c`):

1. ✅ Struct with all 24 string fields + scalar state fields
2. ✅ `note_card_data_new/ref/unref/clear_strings` API
3. ✅ `NoteCardData *data` field added to `_NostrGtkNoteCardRow`
4. ✅ Created/swapped in `prepare_for_bind()`, unref'd in `finalize()`
5. ✅ Identity fields (`id_hex`, `root_id`, `pubkey_hex`) dual-written via `set_ids()`
6. ✅ `nostr_gtk_note_card_row_get_data()` accessor for external consumers
7. Remaining fields: migrate incrementally via dual-write pattern

**Incremental migration pattern** (apply to remaining setters):
```c
// In any setter that writes a string field:
g_free(self->field); self->field = g_strdup(value);
// Add dual-write:
if (self->data) {
  g_free(self->data->field); self->data->field = g_strdup(value);
}
```

Once all fields are dual-written, the flat `self->field` copies can be
removed and all accesses go through `self->data->field`.

---

## 7. Risk Assessment & Implementation Status

| Phase | Status | Risk | Files Changed |
|-------|--------|------|---------------|
| Phase 1 | ✅ DONE | Low | note-card-binding-ctx.h/.c, nostr-note-card-row.c |
| Phase 2 | ✅ DONE | Medium | nostr-note-card-row.c (LazyLoad, article image) |
| Phase 3 | ✅ DONE | Medium | note-card-factory.c (bound-item, profile handler) |
| Phase 4 | ✅ DONE | Low | nostr-note-card-row.c (is_disposed, is_bound) |
| Phase 5 | ✅ INFRA | Low | note-card-data.h/.c, nostr-note-card-row.c |

**Key invariant maintained**: Both the old `disposed` check AND the new context
check are active (belt-and-suspenders). The `disposed` flag is kept as a cache
but `is_disposed()` delegates to the binding context.

---

## 8. Testing Strategy

1. **Fast-scroll stress test**: Rapidly scroll the timeline to trigger maximum
   recycling. With ASan enabled, any use-after-free from stale callbacks will
   crash deterministically.

2. **Model mutation during scroll**: Add/remove events from the GListModel while
   scrolling to trigger the "bound-item freed while row still exists" vector.

3. **Slow network simulation**: Add artificial latency to HTTP responses so avatar
   and media callbacks fire well after rows have been recycled multiple times.

4. **Ref count leak detection**: After scrolling 1000 events, check that
   `NoteCardBindingContext` instances are properly freed (no ref count leaks).
   A simple `g_atomic_int_get(&total_contexts_alive)` debug counter suffices.

5. **Regression**: All existing unit tests and integration tests must continue
   to pass unchanged.
