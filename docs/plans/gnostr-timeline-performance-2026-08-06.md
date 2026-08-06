# gnostr Timeline Performance: Restoring the Sliding-Window Ideal

## Goal
Make the Timeline behave like its design intent — a bounded sliding window over nostrdb exposed as a GListModel — instead of an ever-growing pipeline of copies. Full review evidence: oracle chat `timeline-efficiency-5AB726` (2026-08-06); key claims independently verified.

## Diagnosis — why theory ≠ practice
The compositor architecture is sound (immutable snapshots, row reuse, map-gated I/O), but four systemic divergences defeat it:
1. **Nothing is ever evicted.** Working entries, VMs, `by_event_id`, pending-head VMs, and the geometry measurement cache grow for the window's lifetime (`gnostr-timeline-feed-controller.c:31-66,156-167,198-215`; `gnostr-timeline-geometry.c:79-101,221-246`). "Page size 30" limits initial publication only.
2. **Quadratic + duplicated ordering work.** One replaced row in an N-row snapshot defeats the pointer-identity fast path and allocates a full N×N LCS matrix (~400 MB at 10k rows) (`gnostr-timeline-snapshot-model.c:78-91,139-239`), after the eligible set was sorted in the controller AND re-sorted inside `gnostr_timeline_snapshot_new()` (`feed-controller.c:445-477`; `snapshot.c:452-498`).
3. **The nostrdb boundary throws away its own advantage.** Every query result is serialized from the mmap to JSON, deserialized to `NostrEvent` just to read the ID, then looked up again (`ndb_backend.c:578-646`; `timeline-source.c:285-341`); profile hydration repeats lookup+serialize+parse per NOTE not per author (`timeline-source.c:133-209,250-268`); one event's content is parsed up to 4× (main, empty quote, empty repost, VM `recompute_derived_fields`) and its strings copied ~5× along source→batch→VM→snapshot→row (`hydrator.c`, `item-view-model.c:165-376`, `snapshot.c:117-221`).
4. **Patches scale with history; live path is unscoped.** Profile/interaction updates scan all retained entries and clone entire VMs + reparse content (`feed-controller.c:622-738`); subscriptions stay global regardless of active query, hydration runs synchronously on the dispatch context (`timeline-source.c:711-720,373-396`; `feed-controller.c:122-130`) — the async hydrator API exists unused (`hydrator.c:541-630`).

## Work Items (ranked by expected impact)

### Phase 1 — stop the quadratic/unbounded behavior
1. **Linear keyed diff + single sort** — `gnostr-timeline-snapshot-model.c`: both sides are uniquely keyed and deterministically sorted; replace LCS with prefix/suffix trim + linear merge, detect same-ID replacement spans, coalesce `items_changed`. Add a sorted-rows constructor to `gnostr_timeline_snapshot_new()` (drop the second sort).
2. **Bounded window with coordinated eviction** — `gnostr-timeline-feed-controller.c` + `gnostr-timeline-geometry.c`: visible rows + head/tail buffers; evict working entries, deferred VMs, `by_event_id` keys, and geometry-cache keys together as they leave the window; keep only pagination cursors + anchor; bound pending-head (IDs, not full VMs).
3. **Parse once, share everywhere** — carry one immutable parsed-content artifact (markup + plain text + descriptors + derived geometry) from hydration through VM and snapshot; skip parsing absent quote/repost; transfer-cap descriptors instead of double deep-copy; split mutable state (profile fields, counts) from immutable content so `copy_with_*()` shares content + geometry instead of cloning + reparsing.

### Phase 2 — fix the nostrdb boundary
4. **Note-key query path** — add an ndb query API returning note keys/pointers (no JSON round-trip for IDs); populate batches directly under the open transaction; stop over-querying 100 to publish 30 (`timeline-source.c:864-891`).
5. **Per-author profile hydration** — per-batch (ideally longer-lived) pubkey-keyed cache with an `event_exists` flag; one lookup per author per batch instead of per note; kill the second existence query.

### Phase 3 — live path + patches
6. **Scoped, coalesced, async ingestion** — narrow subscriptions to the active query where possible; coalesce keys into one bounded queue per source generation; use the existing async hydrator with generation cancellation; publish merged completions.
7. **Indexed patches** — `pubkey → WorkingEntry set` index; coalesce patch batches; recalculate only the metric the subscription kind affects (reaction-only update ≠ 4 aggregation passes, `timeline-source.c:555-612`).

### Phase 4 — bind cost + trims
8. **Create rich widgets on map, pool frames** — bind lightweight placeholders; construct grids/OG/embed/video subtrees on map; reuse compatible descriptor slots across recycles (`nostr-note-card-row.c:4310-4565`).
9. **Slim snapshot rows** — `{VM ref + geometry}`; getters proxy the VM; drop duplicated strings/hashtags and the redundant descriptor-array ref (`snapshot.c:117-273`).
10. **Small fixes** — fix `nostr_ndb_store.c:185-224` profile-JSON leak (real leak, do first); cache `GSettings` in `gnostr_is_remote_media_allowed()` (`utils.c:66-91`); pagination watermarks/exhaustion so edge-idling stops re-querying + recomposing (`feed-controller.c:847-902`); remove/repair passive geometry measurement that caches without refining (`geometry.c:321-336`, `feed-controller.c:346-380`).

### Validation (throughout)
Perf invariants as tests: page far past the window → assert bounded VM/row/cache counts; replace one row in a large snapshot → assert linear diff work; count parser invocations per hydration/patch; count rich-child creations during scroll recycling.

## References
- Review: oracle chat `timeline-efficiency-5AB726`; prior efficiency findings in `docs/investigations/gnostr-rich-media-segfault-2026-08-05.md`
- Design intent: `docs/designs/gnostr-timeline-compositor.md`
