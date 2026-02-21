# Async UI Callback Inventory - Generation Fencing Checklist

## Status Legend
- ✅ FENCED - Generation validation implemented
- 🔴 HIGH RISK - Touches GTK widgets, needs immediate fencing
- 🟡 MEDIUM RISK - May touch widgets conditionally
- ⚪ LOW RISK - Background work only, no direct UI mutation

---

## FENCED (Complete)

### NostrGtkNoteCardRow
- ✅ `on_media_decode_done` - Media image decode callback
- ✅ `on_media_image_loaded` - Media HTTP fetch callback
- Uses: `MediaLoadCtx` with weak ref + generation + cancellable

### GnostrNoteEmbed
- ✅ `on_relay_query_done` - Relay query callback
- Uses: `RelayQueryCtx` with weak ref + generation + cancellable

---

## HIGH PRIORITY - Fenced and Verified

### NostrGtkThreadView ✅ COMPLETE
- ✅ `on_thread_query_done` - **FENCED** (commit e3c8fe22)
  - Owner fence: `ThreadView::fence`
  - Context: `ThreadQueryCtx` with weak ref + generation + cancellable
  - Validates before: `rebuild_thread_ui()`, `fetch_missing_ancestors()`, `fetch_children_from_relays()`
  - Call sites: 3 (all updated to use `thread_query_ctx_new()`)

- ✅ `on_root_fetch_done` - **FENCED** (commit e3c8fe22)
  - Owner fence: `ThreadView::fence`
  - Context: `ThreadQueryCtx`
  - Validates before: `rebuild_thread_ui()`, `fetch_missing_ancestors()`
  - Call sites: 1

- ✅ `on_missing_ancestors_done` - **FENCED** (commit e3c8fe22)
  - Owner fence: `ThreadView::fence`
  - Context: `ThreadQueryCtx`
  - Validates before: `rebuild_thread_ui()`, `fetch_missing_ancestors()`, `fetch_children_from_relays()`
  - Call sites: 1

- ✅ `on_children_query_done` - **FENCED** (commit e3c8fe22)
  - Owner fence: `ThreadView::fence`
  - Context: `ThreadQueryCtx`
  - Validates before: `rebuild_thread_ui()`, `fetch_missing_ancestors()`, `fetch_children_from_relays()`
  - Call sites: 1

**Validation Pattern:**
```c
g_autoptr(NostrGtkThreadView) self = g_weak_ref_get(&ctx->view_ref);
THREAD_VIEW_VALIDATE_OR_OUT(ctx, self, out);
// Safe to mutate UI
out:
  thread_query_ctx_free(ctx);
```

### NostrGtkNoteCardRow (remaining)
- ✅ `on_avatar_http_done` - **FENCED** (commit afee2da1)
  - Owner fence: `NoteCardRow::fence`
  - Context: `MediaLoadCtx` (reused existing context)
  - Note: Most avatars use global cache, this is edge case

- � `on_article_image_loaded` (line 5142)
  - Uses: NoteCardBindingContext (partial fence)
  - Action: Verify binding context has generation check
  - Priority: MEDIUM (article mode less common)

### NostrGtkProfilePane
- 🔴 `on_image_loaded` (line 2374)
  - Touches: `gtk_picture_set_paintable` on avatar/banner
  - Risk: Pane closed during image load
  - Action: Add GnUiFence to ProfilePane, create ImageLoadCtx

- 🔴 `on_banner_loaded` (line 2485)
  - Touches: Banner image widget
  - Risk: Pane closed during banner load
  - Action: Use same ImageLoadCtx pattern

---

## MEDIUM PRIORITY

### NostrGtkComposer
- 🟡 Upload progress callbacks
  - Touches: `gtk_widget_set_visible`, `gtk_label_set_text`
  - Risk: Composer closed during upload
  - Action: Add fence if uploads are async

### GnTimelineTabs
- 🟡 Tab management
  - Mostly synchronous
  - Low risk

---

## Fencing Pattern Template

```c
// 1. Add fence to owner struct
struct _MyWidget {
  GnUiFence fence;
  ...
};

// 2. Initialize in init
gn_ui_fence_init(&self->fence);

// 3. Bump on lifecycle transitions
// In dispose/unbind/set_content:
gn_ui_fence_bump(&self->fence);

// 4. Create async context
MyAsyncCtx *ctx = g_new0(MyAsyncCtx, 1);
g_weak_ref_init(&ctx->owner_ref, self);
ctx->generation = gn_ui_fence_gen(&self->fence);
ctx->cancel = gn_ui_fence_cancel_ref(&self->fence);

// 5. Validate in callback
MyWidget *self = g_weak_ref_get(&ctx->owner_ref);
if (!self) {
  g_debug("[FENCE][MyWidget] gone, dropping callback");
  goto out;
}

if (ctx->generation != gn_ui_fence_gen(&self->fence)) {
  g_debug("[FENCE][MyWidget] stale drop gen=%lu cur=%lu",
          ctx->generation, gn_ui_fence_gen(&self->fence));
  g_object_unref(self);
  goto out;
}

if (g_cancellable_is_cancelled(ctx->cancel)) {
  g_debug("[FENCE][MyWidget] cancelled");
  g_object_unref(self);
  goto out;
}

// Safe to update UI
...
g_object_unref(self);

out:
  my_async_ctx_free(ctx);
```

---

## Testing Gate

Run 30-minute stress test with:
```bash
export G_DEBUG=fatal-criticals
export GNOSTR_STRESS_SCROLL=1
export GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.tcache_max=0
export MALLOC_CHECK_=3
export MALLOC_PERTURB_=165
timeout 1800 ./_build/apps/gnostr/gnostr 2>&1 | tee fence_test.log
```

**Victory Condition:**
- Lots of `[FENCE]` stale drops in logs
- Zero crashes (no malloc_consolidate, no G_IS_OBJECT criticals, no pollfd assertions)

---

## Next Actions

1. ✅ Fence `on_avatar_http_done` in NoteCardRow
2. ✅ Fence ThreadView relay callbacks (4 callbacks)
3. ✅ Fence ProfilePane image callbacks (2 callbacks)
4. Run 30min stress test
5. Verify victory condition
