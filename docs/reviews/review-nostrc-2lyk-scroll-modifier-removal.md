# Peer Review — `b8296777` "refactor(nostrc-2lyk): remove legacy scroll modifiers from timeline view"

**Reviewer:** Claude (pre-push peer review)
**Date:** 2026-08-09
**Branch:** `master` (commit unpushed at review time)
**Verdict:** **APPROVED**

---

## Context / Scope

The commit removes widget-level runtime overrides of `GtkScrolledWindow` behavior from
`NostrGtkTimelineView`, on the premise that the "sliding view" pattern in
`GnostrTimelineFeedController` (fed by viewport updates from
`apps/gnostr/src/ui/gnostr-main-window-timeline.c`) now owns scroll anchoring for the
event timeline.

Files touched:

| File | Δ |
| --- | --- |
| `nostr-gtk/src/gnostr-timeline-view.c` | +3 / −121 |
| `nostr-gtk/include/nostr-gtk-1.0/gnostr-timeline-view-private.h` | +4 / −13 |
| `.beads/issues.jsonl` | +7 / −7 (tracker bookkeeping) |

Removed: `on_vadj_upper_notify` and its wiring, `connect_model_scroll_tracking` /
`disconnect_model_scroll_tracking` / `clear_prepend_fixup`, `SCROLL_TOP_THRESHOLD`,
`timeline_view_has_compositor_controller`, the local
`GNOSTR_TIMELINE_FEED_CONTROLLER_DATA_KEY` macro, the force-scroll-to-top blocks in
`nostr_gtk_timeline_view_prepend[_text]()`, and eight dead struct fields.

Verification performed for this review: repo-wide symbol greps (excluding `_build/`,
`.git/`), reading pre- and post-commit revisions of the touched functions, tracing the
qdata/controller wiring in `apps/gnostr`, `cmake --build _build --target nostr_gtk`
(clean), and `ctest --test-dir _build -L timeline` (8/8 passed).

---

## Findings

### 1. Dangling references — **none** ✅

Repo-wide grep for every removed identifier returns zero hits outside `_build/` and
`.git/`:

```
on_vadj_upper_notify                     — 0
clear_prepend_fixup                      — 0
connect_model_scroll_tracking            — 0
disconnect_model_scroll_tracking         — 0
prev_adj_upper                           — 0
prepend_fixup*                           — 0
timeline_view_has_compositor_controller  — 0
SCROLL_TOP_THRESHOLD                     — 0
model_items_changed_handler_id           — 0
observed_model                           — 0
```

`GNOSTR_TIMELINE_FEED_CONTROLLER_DATA_KEY` still exists, but only as **independent local
`#define`s** in the app layer (`gnostr-main-window-timeline.c:25`,
`gnostr-timeline-view-app-factory.c:68`). The library copy was never exported, so removing
it is a pure local cleanup — no app-side breakage.

### 2. Signal handler / teardown symmetry — **no asymmetry introduced** ✅

The commit removes the only `disconnect_model_scroll_tracking(self)` call from
`nostr_gtk_timeline_view_dispose()`. I checked the pre-commit revision to confirm this
function was not actually tearing anything down:

```
$ git show b8296777^:nostr-gtk/src/gnostr-timeline-view.c | grep -n 'prepend_fixup_id|model_items_changed_handler_id|observed_model'
190:  self->model_items_changed_handler_id = 0;   # write-only zeroing
191:  self->observed_model = NULL;                # write-only zeroing
312:  if (self->prepend_fixup_id > 0) {           # read
313:    g_source_remove(self->prepend_fixup_id);  # dead: never assigned non-zero anywhere
314:    self->prepend_fixup_id = 0;
```

`model_items_changed_handler_id` was never assigned a real handler id, `observed_model` was
never assigned a model, and `prepend_fixup_id` was never armed. The `g_source_remove()` path
was unreachable. So the removed dispose call disconnected nothing and freed nothing —
**no regression.**

The one handler genuinely connected by the removed code (`notify::upper` on the vadjustment,
wired in `_init` and never disconnected) is removed alongside its connect site, which is
symmetric.

### 3. Behavioral impact — **inert on the only live instance** ✅

This is the load-bearing question, and it checks out. Tracing the instance graph:

- `NostrGtkTimelineView` is instantiated exactly once, from the template
  `apps/gnostr/data/ui/widgets/gnostr-session-view.ui:537` (`id="timeline"`). There are no
  `nostr_gtk_timeline_view_new()` callers anywhere. `thread_view` / `profile_pane` are
  distinct types (`GNOSTR_IS_THREAD_VIEW`, `GNOSTR_IS_PROFILE_PANE`).
- The feed controller **is** attached to that instance via qdata, using the raw string
  literal (not the macro) at `apps/gnostr/src/ui/gnostr-main-window-startup.c:199`:
  `g_object_set_data_full(G_OBJECT(timeline), "timeline-feed-controller", ...)`.

Therefore `timeline_view_has_compositor_controller()` returned `TRUE` for the only live view,
and the removed `on_vadj_upper_notify()` early-returned before touching the adjustment — its
sole surviving effect was bookkeeping `self->prev_adj_upper`, a field also removed. **Removal
is behavior-neutral in steady state.**

It is a small *improvement* at one edge: the `notify::upper` handler was connected in
`_init()`, whereas the qdata is not attached until main-window startup. In that construction
→ startup window the compensator was live and could shift the scroll position while the feed
controller's `restore-scroll` path was also active. That double-anchoring window is now gone.

### 4. ABI / API safety of struct-field removal — **safe** ✅

- `nostr-gtk/include/nostr-gtk-1.0/gnostr-timeline-view.h:24` uses `G_DECLARE_FINAL_TYPE`, so
  no external subclass can embed or extend the instance struct.
- `gnostr-timeline-view-private.h` is not installed, and grep shows it is now included by
  **exactly one translation unit** — `nostr-gtk/src/gnostr-timeline-view.c:17`. The app
  factory deliberately dropped that include per
  `docs/TIMELINE_LIBRARY_APP_BOUNDARY.md:212`, so no app code does direct field access.
- All eight removed fields were trailing members (after `scroll_idle_id`), so even the layout
  of surviving fields is unchanged.

There is no cross-module struct-layout exposure to worry about here.

### 5. Scope — nothing needed was removed ✅

- `nostr_gtk_timeline_view_prepend()` and `..._prepend_text()` remain declared
  (`gnostr-timeline-view.h:45,48`) and defined (`gnostr-timeline-view.c:455,463`). Both still
  call `ensure_list_model()` before `g_list_store_insert(..., 0, item)`, so they remain
  correct as pure model inserts. Confirmed dead as claimed: no callers anywhere in `apps/` or
  `nostr-gtk/`.
- Kept surfaces verified live:
  - `nostr_gtk_timeline_view_is_fast_scrolling()` / `..._is_item_visible()` are consumed at
    `apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c:1446-1447` to set
    `defer_metadata` — the `value-changed` tracker must stay, and it does.
  - Feed-controller viewport sync
    (`gnostr-main-window-timeline.c:132-175` → `set_viewport` / `set_user_at_top` /
    `load_newer` / `load_older`) and the `restore-scroll` handler
    (`gnostr-main-window-timeline.c:190-220`, connected at
    `gnostr-main-window-startup.c:192`) are untouched.

### 6. Pattern consistency ✅

The change moves the codebase further toward the documented boundary in
`docs/TIMELINE_LIBRARY_APP_BOUNDARY.md`: the reusable library widget stays policy-free and
purely observational, while GNostr-specific behavior lives in app-side controllers attached
via qdata. The retained comments in both files accurately describe the new invariant ("the
widget never modifies the scroll position"). The commit message is precise and matches the
diff.

### 7. Build & tests ✅

- `cmake --build _build --target nostr_gtk -j8` — clean, no warnings surfaced for the touched
  file.
- `ctest --test-dir _build -L timeline` — `100% tests passed, 0 tests failed out of 8`
  (`timeline-query`, `timeline-snapshot`, `timeline-geometry`, `timeline-hydrator`,
  `timeline-feed-controller`, `timeline-source-live`, `filter-set-query`, `timeline-source`).

---

## Non-blocking observations (pre-existing; follow-up candidates)

None of these are introduced by this commit, and none should hold up the push. They are
recorded here because the commit touches the surrounding code and a future reader will
naturally ask about them.

### A. `dispose()` ordering can re-arm the scroll idle timeout

`nostr_gtk_timeline_view_dispose()` (`gnostr-timeline-view.c:181-206`) clears
`scroll_idle_id` **first**, then calls `gtk_list_view_set_model(NULL)` and
`gtk_widget_dispose_template()`. The `value-changed` handler is never disconnected, and
`on_scroll_value_changed()` unconditionally re-arms
`g_timeout_add(SCROLL_IDLE_TIMEOUT_MS, on_scroll_idle_timeout, self)` with an unowned `self`.
If any teardown step causes the vadjustment to emit `value-changed`, a fresh timeout is
scheduled after the removal and can fire on a finalized object.

Suggested hardening: `g_signal_handlers_disconnect_by_func(vadj, on_scroll_value_changed, self)`
at the top of `dispose()`, or move the `scroll_idle_id` removal to after
`gtk_widget_dispose_template()`. Low probability in practice (adjustment updates are deferred
to size-allocate, by which point the widget is unparented), but it is now the only remaining
adjustment handler on this widget.

### B. `nostr_gtk_timeline_view_set_tree_roots()` has latent refcount/handler bugs

`gnostr-timeline-view.c:423-452`:

- `g_signal_connect(roots, "items-changed", ...)` on every call, never disconnected — repeated
  calls multiply handlers and leave `self` dangling in the callback if `roots` outlives the view.
- `self->tree_model = (GtkTreeListModel*)roots;` takes **no reference** and performs an unchecked
  cast, yet `dispose()` and `set_model()` both `g_clear_object(&self->tree_model)` — an
  unbalanced unref.

Currently harmless because the function has no callers (dead API, same status as
`prepend[_text]`). Worth either fixing or explicitly deprecating.

### C. The "block around programmatic `set_value`" idiom disappeared from the tree

The deleted `on_vadj_upper_notify()` bracketed its own adjustment write with
`g_signal_handlers_block_by_func(adj, on_scroll_value_changed, self)`. The surviving
programmatic writer — `gnostr_main_window_on_timeline_restore_scroll_internal()` at
`gnostr-main-window-timeline.c:218` — does not. It guards the *controller* feedback loop with
`timeline_scroll_restore_depth` (`gnostr-main-window-private.h:83`), but the widget's own
velocity tracker still observes the restore jump. A large controller-driven restore can
therefore register as `is_fast_scrolling == TRUE` and cause the app factory to defer metadata
loads for one ~150 ms idle window.

This is unchanged by the commit (the compensator was already inert on the main feed), but if
the deferral heuristic is meant to track *user* scrolling only, the restore handler should
block `on_scroll_value_changed` around its `gtk_adjustment_set_value()` the way the deleted
code did.

### D. Stale doc comment in the private header

`gnostr-timeline-view-private.h:4-5` still says the header "Exposes … struct layouts for use by
app-level factory code that needs direct field access." No app code includes it anymore. A
one-line comment refresh would keep the header honest (and reinforce the boundary documented in
`docs/TIMELINE_LIBRARY_APP_BOUNDARY.md`).

---

## Recommendation

The removal is correct, well-scoped, and verifiably inert for the only live
`NostrGtkTimelineView` instance. It deletes genuinely dead machinery (fields that were never
written, a `g_source_remove` path that was never armed) and eliminates a startup-window
double-anchoring hazard. No dangling references, no teardown asymmetry, no ABI exposure, no
loss of needed API. Build and timeline tests are green.

Items A–D are pre-existing and should be filed as follow-ups rather than folded into this
commit.

**APPROVED** — safe to push.
