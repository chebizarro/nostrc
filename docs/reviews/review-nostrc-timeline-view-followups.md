# Peer Review — Timeline View Lifecycle & Tracker Follow-ups

**Commit:** `62904004` — *fix(nostrc-07n9,nostrc-14lo,nostrc-wgt7,nostrc-gogm): timeline view lifecycle and tracker fixes*
**Reviewer:** pre-push peer review
**Date:** 2026-08-09
**Predecessor review:** `docs/reviews/review-nostrc-2lyk-scroll-modifier-removal.md` (findings A–D)

## Scope

| File | Change |
|---|---|
| `nostr-gtk/src/gnostr-timeline-view.c` | dispose ordering, tree_model ref/handler lifecycle, new programmatic-scroll API |
| `nostr-gtk/include/nostr-gtk-1.0/gnostr-timeline-view.h` | two new public declarations |
| `nostr-gtk/include/nostr-gtk-1.0/gnostr-timeline-view-private.h` | comment refresh only |
| `apps/gnostr/src/ui/gnostr-main-window-timeline.c` | wrap controller-driven scroll restore |

**Verification performed:** `cmake --build _build -j8` → clean (100%). `ctest --test-dir _build -R timeline` → 7/7 passed. No source files were modified by this review.

---

## Findings

### A. nostrc-07n9 — dispose disconnect-then-remove ordering — ✅ CORRECT

`nostr_gtk_timeline_view_dispose()` (`gnostr-timeline-view.c:183–197`) now disconnects `on_scroll_value_changed` from the vadjustment before `g_source_remove(self->scroll_idle_id)`.

Confirmed:

- **The ordering closes the race.** `g_timeout_add(SCROLL_IDLE_TIMEOUT_MS, on_scroll_idle_timeout, self)` has exactly one call site — `gnostr-timeline-view.c:282`, inside `on_scroll_value_changed`. Once that handler is disconnected, nothing downstream in dispose (`gtk_list_view_set_model(..., NULL)` at :204, `gtk_widget_dispose_template` at :219) can re-arm the source. The previous ordering left a window where those teardown steps could emit `value-changed` and re-register a timeout holding a bare `self`.
- **The disconnect triple matches the connection.** `init` connects `g_signal_connect(vadj, "value-changed", G_CALLBACK(on_scroll_value_changed), self)` at `:352`, where `vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller))`. `dispose` resolves the adjustment the same way and passes the same `(instance, func, data)`.
- **The adjustment identity is stable.** `grep` finds no `gtk_scrolled_window_set_vadjustment` anywhere in the repository and no `adjustment` property in the widget's `.ui` template, so the `vadj` seen at dispose is the one connected at init. (This is an assumption worth keeping in mind if the template ever gains an explicit adjustment.)
- **Double-dispose safe.** `root_scroller` is NULLed at `:220` after `gtk_widget_dispose_template`, so the second dispose pass short-circuits the guard. Independently, `g_signal_handlers_disconnect_by_func` is idempotent and `scroll_idle_id` is zeroed at `:199`.
- **Ordering relative to template disposal is right.** `root_scroller` is still a live template child when the disconnect runs; `gtk_widget_dispose_template` happens 22 lines later.

No defects.

---

### B. nostrc-14lo — `set_tree_roots` ref + handler lifecycle — ⚠️ ONE NEW DEFECT, plus notes

#### B1. `g_return_if_fail(GTK_IS_TREE_LIST_MODEL(roots))` makes the function unusable — **NEW, blocking for the fix's own goal**

`gnostr-timeline-view.c:451`

```c
g_return_if_fail(roots == NULL || GTK_IS_TREE_LIST_MODEL(roots));
```

This precondition contradicts both the documented contract and the implementation:

- **Documented contract** (`gnostr-timeline-view.h:55`): *"Set a tree of TimelineItem roots (GListModel of internal items); view flattens via GtkTreeListModel."* The parameter is `GListModel *roots` and the promise is a list **of TimelineItem**.
- **Implementation** (`populate_flattened_model`, `:426–446`): it iterates `roots`, casts each item to `TimelineItem*`, appends it into `self->flattened_model` — a store created with `g_list_store_new(timeline_item_get_type())` (`:467`) — and calls `timeline_item_get_children_model(root)`, which is an unchecked `it->children` field read (`:134–136`).
- **The type the assertion now demands cannot satisfy that body.** A `GtkTreeListModel`'s item type is `GTK_TYPE_TREE_LIST_ROW`, not `TimelineItem`. Passing one means every `g_list_store_append(self->flattened_model, root)` trips `g_list_store`'s own `g_return_if_fail` type check, and `timeline_item_get_children_model()` reads the `children` slot out of a `GtkTreeListRow` allocation — undefined behaviour, not a checked-cast failure.

Net effect: the only argument the guard **accepts** is the one the body **cannot process**, and the argument the body is written for (a `GListStore` of `TimelineItem`, per the header) is now silently rejected and the call becomes a no-op. The checked cast was intended to harden the function for a future caller; it instead guarantees no future caller can succeed.

The `GtkTreeListModel*` field type is the root confusion — it has always been a misnomer, since `on_root_items_changed` (`:412–420`) re-flattens via `populate_flattened_model(self, G_LIST_MODEL(self->tree_model))`, i.e. it treats the field as the raw roots list, never as a tree-list wrapper.

**Recommended fix (one line + one field):** drop the tree-list assertion and hold the ref on the interface type —

```c
g_return_if_fail(roots == NULL || G_IS_LIST_MODEL(roots));
...
self->tree_model = g_object_ref(roots);   /* field retyped to GListModel *root_model */
```

Retyping `GtkTreeListModel *tree_model` → `GListModel *root_model` in `gnostr-timeline-view-private.h` also removes the need for the casts at `:418` and makes the field name honest. `g_clear_object()` at all three sites works unchanged. Alternatively, keep the assertion and rewrite the flattening to unwrap `GtkTreeListRow` — but that is a much larger change for a function with no callers.

*Severity note:* `set_tree_roots` has **zero callers** (`grep` across `*.c`/`*.h`: only the declaration and the definition). Nothing ships broken today. But since the stated purpose of nostrc-14lo was "fix the bugs for any future caller," the fix does not meet its own bar.

#### B2. Refcount balance for the ref that *was* added — ✅ CORRECT

Audited every path; no leak and no double-unref from the `g_object_ref` change:

| Path | Behaviour |
|---|---|
| `set_tree_roots(roots)` | `g_clear_object(&self->tree_model)` at `:463` releases the previous held ref (or no-ops on NULL); `g_object_ref(...)` at `:468` takes a fresh one. Balanced. |
| `set_tree_roots(NULL)` | Clears to NULL, then the `else` branch re-assigns `NULL` at `:475–477`. Redundant after `g_clear_object` but harmless — worth deleting for clarity. |
| `set_model()` after `set_tree_roots()` | Disconnect (`:398`) then `g_clear_object(&self->tree_model)` (`:402`). Previously this unreffed a borrowed pointer — a genuine over-unref. Now balanced. |
| `dispose()` | Disconnect (`:208`) then `g_clear_object` (`:212`). Balanced. |
| Double dispose | `tree_model` is NULL on the second pass; both the disconnect guard and `g_clear_object` no-op. |

The ownership-convention change (borrowed → held, `(transfer none)` on the caller side) is safe precisely because there are no callers to migrate.

Handler multiplication is also fixed: `set_tree_roots` disconnects before the swap (`:457`) and reconnects only inside the `roots != NULL` branch (`:474`), and the connect was moved *after* `populate_flattened_model` so the initial population no longer risks a reentrant re-flatten.

#### B3. Pre-existing leak in the same function: `g_object_ref_sink` on a non-floating object

`gnostr-timeline-view.c:471`

```c
self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->flattened_model)));
g_object_ref_sink(self->selection_model);
```

`gtk_single_selection_new()` returns a `(transfer full)` ref, and `GtkSingleSelection` derives from `GObject`, **not** `GInitiallyUnowned` — so it is never floating. `g_object_ref_sink` on a non-floating object simply increments the refcount to 2, and the single `g_clear_object(&self->selection_model)` at teardown drops only one. One selection model leaks per `set_tree_roots(roots)` call.

The sibling construction site, `ensure_list_model()` (`:286–291`), correctly does *not* `ref_sink`. Not introduced by this commit, but it sits three lines from the changed code and it defeats the "no leak on this path" objective of nostrc-14lo. Delete the `g_object_ref_sink` line.

#### B4. `set_model()` does not clear `flattened_model` — pre-existing, minor

`set_model()` (`:391–405`) clears `selection_model`, `list_model` and `tree_model` but leaves `flattened_model` populated. After a `set_tree_roots(roots)` → `set_model(other)` transition, a stale flattened store survives until dispose. Now that `tree_model` teardown is correct, this is the last dangling piece of that state group. Low priority.

#### B5. `set_model()`'s early return precedes the new disconnect — minor

`if (self->selection_model == model) return;` at `:394` runs before the `tree_model` disconnect added at `:398`. Today this is harmless because `tree_model != NULL` implies `selection_model != NULL` — but that invariant is not asserted or documented anywhere. A future `set_tree_roots` variant that leaves `selection_model` NULL would silently skip the disconnect. A one-line comment stating the invariant would make the ordering defensible.

---

### C. nostrc-wgt7 — begin/end programmatic scroll — ✅ CORRECT

`gnostr-timeline-view.c:605–632`, `gnostr-timeline-view.h:98–116`, `gnostr-main-window-timeline.c:217–223`.

Confirmed:

- **Block/unblock target the exact connection triple.** Both functions resolve `vadj` via `gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller))` — byte-for-byte the same expression used in `init` at `:351` — and pass `(vadj, on_scroll_value_changed, self)`. `g_signal_handlers_block_by_func` / `unblock_by_func` will match the single handler installed in `init`.
- **The caller always pairs them.** In `gnostr_main_window_on_timeline_restore_scroll_internal`, all four early returns (`!GNOSTR_IS_MAIN_WINDOW || !session_view`, `!NOSTR_GTK_IS_TIMELINE_VIEW(timeline)`, `!GTK_IS_SCROLLED_WINDOW(scroller)`, `!vadj`) occur *before* `begin_programmatic_scroll()`. Between `begin` and `end` there is exactly one statement — `gtk_adjustment_set_value(vadj, target)` — with no branch, return, or goto. Correct pairing under all inputs.
- **Header visibility is fine.** `gnostr-main-window-timeline.c:6` already includes `<nostr-gtk-1.0/gnostr-timeline-view.h>`, which now carries both declarations. Build is clean, confirming no implicit-declaration warning.
- **The re-sync genuinely prevents the next-scroll spike.** GTK4's `gtk_adjustment_set_value` emits `value-changed` synchronously, so the block covers the jump itself. Without the re-sync, `last_scroll_value` would still hold the *pre-jump* position, and the first post-restore user `value-changed` would compute `dx = user_value − pre_jump_value` over a tiny `dt` — exactly the spike this bead is about. Setting `last_scroll_value` to the restored position and `last_scroll_time` to `now` makes the next delta a true user delta. Zeroing `scroll_velocity` is consistent with `get_scroll_velocity()`'s contract.
- **The restore is now invisible to the app's metadata deferral.** With `is_fast_scrolling` no longer being set by the restore, the app factory's ~150 ms metadata defer is not triggered spuriously.
- **This is the only affected call site.** `grep` for `gtk_adjustment_set_value` shows the timeline's vadjustment is written programmatically only here (`gnostr-main-window-timeline.c:221`). The other hits are unrelated widgets (chat/DM/chess/image-viewer/picture-grid, `gnostr-thread-view.c:1900`) or tests that scroll deliberately to *simulate* fast scrolling — those must not be wrapped.

Notes (non-blocking):

- **C1 — `is_fast_scrolling` is not reset in `end_`.** The function zeroes `scroll_velocity` but leaves the boolean, so `is_fast_scrolling()` and `get_scroll_velocity()` can disagree (TRUE / 0.0) for up to `SCROLL_IDLE_TIMEOUT_MS`. This is arguably the *right* choice — a genuine user fast-scroll in progress should not be cancelled by an unrelated restore, and the still-armed idle timeout clears the flag on schedule — but the asymmetry is non-obvious. Add a sentence to the comment explaining why the flag is deliberately left alone.
- **C2 — only the synchronous emission is suppressed.** If GTK later re-clamps the adjustment (e.g. `upper` shrinks when a model refresh reflows the list), the resulting `value-changed` lands after `unblock` and can still register as a spike. Not reachable from the current call site, since the caller clamps `target` against the live `upper`/`page_size`, but it bounds what this API guarantees.
- **C3 — doc nit: "pairs must not nest" is stricter than reality.** GLib maintains a per-handler block *count*, so `block`/`unblock` nest correctly. The only nesting side-effect is the inner `end_` re-syncing the tracker early, which the outer `end_` immediately re-syncs again. Either relax the doc or state the actual reason.

---

### D. nostrc-gogm — private-header comment refresh — ✅ CORRECT

The claim in the new comment is accurate: `grep` for `gnostr-timeline-view-private.h` across all `*.c`/`*.h` finds exactly one include — `nostr-gtk/src/gnostr-timeline-view.c:17`, the widget's own translation unit. The referenced `docs/TIMELINE_LIBRARY_APP_BOUNDARY.md` exists. Comment-only change; no risk.

---

### E. Test coverage gap — note for follow-up

The author's "7/7 timeline ctest tests pass" is accurate but is **not evidence for A–C**. The seven tests are `timeline-query`, `-snapshot`, `-geometry`, `-hydrator`, `-feed-controller`, `-source-live`, `-source` — all model/controller-layer tests. None of them instantiate `NostrGtkTimelineView` or link the changed widget paths. The only test in the tree that references `nostr_gtk_timeline_view_*` at all is `nostr-gtk/tests/test_vapi_smoke.c`, and it only calls `..._get_type()`.

Suggested follow-up bead (a widget-level test in the style of `test_nostr_gtk_widget_churn_leaks`):

1. Realize a view, drive `on_scroll_value_changed` via `gtk_adjustment_set_value`, assert `scroll_idle_id != 0`, then `g_object_run_dispose` and assert no use-after-free under ASan — covers **A**.
2. `begin_programmatic_scroll()` → large `gtk_adjustment_set_value` → `end_programmatic_scroll()`; assert `is_fast_scrolling() == FALSE` and `get_scroll_velocity() == 0.0` — covers **C**.
3. Once **B1** is resolved: `set_tree_roots(store)` twice, assert the roots store's refcount returns to the caller's after `set_model(NULL)` and after dispose — covers **B**.

---

## Recommendations

| # | Finding | Priority | Action |
|---|---|---|---|
| 1 | **B1** — `GTK_IS_TREE_LIST_MODEL` assertion is unsatisfiable | **Blocking** | Replace with `G_IS_LIST_MODEL`; retype the field to `GListModel *root_model` |
| 2 | **B3** — `g_object_ref_sink` leaks a `GtkSingleSelection` | High | Delete line `:471` |
| 3 | **B4** — `set_model()` leaves `flattened_model` stale | Low | Add `g_clear_object(&self->flattened_model)` |
| 4 | **B5** — early return precedes disconnect | Low | Document the `tree_model ⇒ selection_model` invariant |
| 5 | **C1/C3** — comment/doc precision | Low | Explain the `is_fast_scrolling` asymmetry; relax the no-nesting claim |
| 6 | **E** — no widget-level test coverage | Medium | File a follow-up bead |
| 7 | **B2** — redundant NULL assignments at `:475–477` | Cosmetic | Delete |

Items 3–7 are fine as follow-up beads. Item 1 is a one-line change plus a field retype and should land before push; item 2 is a one-line deletion in the same function and is worth folding in.

## Verdict

**REQUEST CHANGES** — scoped to finding B1. A, C and D are correct, well-reasoned and safe to ship; B's refcount and handler work is also correct. The blocker is the newly added `g_return_if_fail(roots == NULL || GTK_IS_TREE_LIST_MODEL(roots))`, which admits only a model type `populate_flattened_model` cannot process and rejects the `GListModel`-of-`TimelineItem` the header documents — making a function the commit set out to repair permanently unusable. Fix that line (and preferably the `g_object_ref_sink` leak beside it) and this is an approve.

---

# Addendum — Re-review of `62d54a05`

**Commit:** `62d54a05` — *fix(nostrc-14lo): address review — correct roots model typing in set_tree_roots* (on top of `62904004`)
**Date:** 2026-08-09

**Verification performed:** `cmake --build _build -j8` → clean, no new warnings. `ctest --test-dir _build -R timeline` → 7/7 passed. `grep` for residual `tree_model` / `GTK_TREE_LIST` / `ref_sink` across `nostr-gtk` and `apps` (including `*.vala`/`*.vapi`) → zero hits. No source files were modified by this review.

## Resolution of prior findings

### B1 — unsatisfiable `GTK_IS_TREE_LIST_MODEL` guard — ✅ RESOLVED

The guard is now `g_return_if_fail(roots == NULL || G_IS_LIST_MODEL(roots))` (`:450`), which matches the documented contract. The field was retyped `GtkTreeListModel *tree_model` → `GListModel *root_model` with an ownership comment (`gnostr-timeline-view-private.h:63`), the assignment is a plain `g_object_ref(roots)` (`:469`), and the now-unnecessary `G_LIST_MODEL()` cast in `on_root_items_changed` was dropped (`:419`). The rename is complete and consistent — all 13 use sites migrated, nothing left behind.

The field rename is layout-identical (pointer → pointer) and the header is not installed, so there is no ABI or binding consequence; the `grep` over `*.vapi`/`*.vala` confirms no generated-binding references either.

The public doc comment at `gnostr-timeline-view.h:55` no longer claims a `GtkTreeListModel` flattening path. Header, field type, guard and implementation now agree — which was the substance of the objection.

### B3 — `g_object_ref_sink` leak — ✅ RESOLVED

The `g_object_ref_sink(self->selection_model)` line is gone; `set_tree_roots` now matches `ensure_list_model()`'s convention of taking `gtk_single_selection_new()`'s `(transfer full)` ref as-is.

Importantly, dropping the extra ref did **not** introduce a premature free. In both `set_model()` and `dispose()`, `g_clear_object(&self->selection_model)` runs while the `GtkListView` still holds its own ref on the same object, so the selection model survives until `gtk_list_view_set_model()` replaces or clears it. That was the one way this change could have gone wrong, and it doesn't.

## Full re-audit — `set_tree_roots` / `set_model` / `dispose`

Traced refcounts and handler state across every transition. All balanced:

| Transition | `root_model` | `selection_model` | `flattened_model` | Handlers |
|---|---|---|---|---|
| `set_tree_roots(A)` | `+1` (`g_object_ref`) | new, rc 1 (self) → 2 after `list_view` | new, rc 1 → 2 (selection holds one) | `items-changed` connected once, after `populate_flattened_model` |
| `set_tree_roots(A)` → `set_tree_roots(B)` | A `−1` ✅, B `+1` | list_view→NULL drops to 1, `g_clear_object` frees | freed after selection releases it | A disconnected before the swap ✅ |
| `set_tree_roots(A)` → `set_tree_roots(NULL)` | A `−1` ✅ | cleared | cleared | A disconnected ✅ |
| `set_tree_roots(A)` → `set_model(m)` | A `−1` ✅ | old drops 2→1, freed when `list_view` takes `m` — no UAF | released by old selection; self's ref persists (**B4**) | A disconnected ✅ |
| `set_tree_roots(A)` → `dispose()` | A `−1` ✅ | freed | freed | vadj + A both disconnected ✅ |
| second `dispose()` | NULL, no-op | no-op | no-op | guards short-circuit ✅ |

No leak, no double-unref, no use-after-free, no handler multiplication, and no path where a disconnected-then-cleared model can emit into the view. The A (nostrc-07n9) and C (nostrc-wgt7) code is untouched by this commit and my prior confirmations stand.

## Remaining open items — all non-blocking, all previously noted

| # | Item | Priority |
|---|---|---|
| B4 | `set_model()` still doesn't clear `flattened_model` — stale until dispose; no leak | Low |
| B5 | `set_model()`'s `if (self->selection_model == model) return;` precedes the disconnect; the `root_model ⇒ selection_model` invariant is still undocumented | Low |
| B2 | Redundant `= NULL` assignments in the `else` branch (`:476–478`) after `g_clear_object` | Cosmetic |
| C1/C3 | `is_fast_scrolling` asymmetry in `end_programmatic_scroll`; "pairs must not nest" is stricter than GLib's block-count semantics | Low |
| E | No widget-level test exercises any of these paths — the 7 passing "timeline" tests are model/controller tests | Medium |

**One new optional nit (not blocking):** `G_IS_LIST_MODEL` is the correct guard for the documented contract, but `populate_flattened_model` still casts items unchecked to `TimelineItem*` and calls `timeline_item_get_children_model()`, which is a raw `it->children` field read (`:134`). If a caller passes a `GListModel` of some other item type, `g_list_store_append` emits a critical and *returns*, but execution continues into that field read on a foreign allocation — UB rather than a clean failure. A one-line hardening would close it:

```c
g_return_if_fail(roots == NULL ||
                 g_type_is_a(g_list_model_get_item_type(roots), timeline_item_get_type()));
```

Worth folding into whichever follow-up bead picks up B4/B5, alongside a widget-level test (item E). None of this needs to hold the push.

## Addendum verdict

Both requested changes are fully and correctly addressed, the rename is complete with no residue, and the refcount/handler lifecycle is balanced on every path I traced. Nothing regressed in the A/C/D work.

**APPROVED**
