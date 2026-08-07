# gnostr Timeline Performance: Restoring the Sliding-Window Ideal

## Goal
Make the Timeline behave like its design intent — a bounded sliding window over nostrdb exposed as a GListModel — instead of an ever-growing pipeline of copies. Full review evidence: oracle chat `timeline-efficiency-5AB726` (2026-08-06); key claims independently verified.

## Diagnosis — why theory ≠ practice
The compositor architecture is sound (immutable snapshots, row reuse, map-gated I/O), but four systemic divergences defeat it:
1. **Nothing is ever evicted.** Working entries, VMs, `by_event_id`, pending-head VMs, and the geometry measurement cache grow for the window's lifetime (`gnostr-timeline-feed-controller.c:31-66,156-167,198-215`; `gnostr-timeline-geometry.c:79-101,221-246`). "Page size 30" limits initial publication only.
2. **Quadratic + duplicated ordering work.** One replaced row in an N-row snapshot defeats the pointer-identity fast path and allocates a full N×N LCS matrix (~400 MB at 10k rows) (`gnostr-timeline-snapshot-model.c:78-91,139-239`), after the eligible set was sorted in the controller AND re-sorted inside `gnostr_timeline_snapshot_new()` (`feed-controller.c:445-477`; `snapshot.c:452-498`).
3. **The nostrdb boundary throws away its own advantage.** Every query result is serialized from the mmap to JSON, deserialized to `NostrEvent` just to read the ID, then looked up again (`ndb_backend.c:578-646`; `timeline-source.c:285-341`); profile hydration repeats lookup+serialize+parse per NOTE not per author (`timeline-source.c:133-209,250-268`); one event's content is parsed up to 4× (main, empty quote, empty repost, VM `recompute_derived_fields`) and its strings copied ~5× along source→batch→VM→snapshot→row (`hydrator.c`, `item-view-model.c:165-376`, `snapshot.c:117-221`).
4. **Patches scale with history; live path is unscoped.** Profile/interaction updates scan all retained entries and clone entire VMs + reparse content (`feed-controller.c:622-738`); subscriptions stay global regardless of active query, hydration runs synchronously on the dispatch context (`timeline-source.c:711-720,373-396`; `feed-controller.c:122-130`) — the async hydrator API exists unused (`hydrator.c:541-630`).

## nostrdb leverage audit (2026-08-06) — capabilities we ignore

Vendored nostrdb natively provides, with our current usage status:
- **Persisted content blocks**: parses content at ingest into typed blocks (text/url/hashtag/bech32-mention/invoice) stored in LMDB (`NDB_DB_NOTE_BLOCKS`, `ndb_get_blocks_by_key` with lazy backfill, `third_party/nostrdb/src/nostrdb.c:6118-6160,10228-10299`). Our wrapper `storage_ndb_get_blocks()` (`storage_ndb.c:2252-2258`) has **zero callers** — every render re-parses via `ndb_parse_content` with a fresh 512 KiB buffer (`content_renderer.c:602-647`), up to 4×/event.
- **Zero-copy profile flatbuffers**: `NdbProfile` accessors + native profile search + `NDB_DB_PROFILE_LAST_FETCH`. Used correctly in ONE path (`storage_ndb_get_profile_meta_direct` → provider `nostr_profile_provider.c:206-250`); the timeline source + profile service instead do record→note→JSON→NostrEvent→content-JSON→GNostrProfile per NOTE (`ndb_backend.c:766-795`, `gnostr-timeline-source.c:168-216`).
- **Persisted interaction counts**: `ndb_note_meta` (`NDB_DB_META`) stores reactions (by emoji + total), direct/thread replies, reposts, quote reposts, updated at ingest (`src/metadata.h`, `nostrdb.h:685-723`). We instead recompute aggregates with 4 DB passes per reaction/zap event (`gnostr-timeline-source.c:555-612`). **Unused.**
- **Query planner + indexes**: kind/pubkey/pubkey+kind/relay+kind indexes; kinds+limit are passed down (good), but we then post-filter twice in C (`query_matches_note` + hard kind allowlist, `timeline-source.c:109-145,245-252`). Caveat: multi-author queries have a planner TODO (fall back to scans) — relevant to follow feeds.
- **Native filter-scoped subscriptions**: we DO use `ndb_subscribe`/`ndb_poll_for_notes` via `GnNdbDispatcher` (good), but keep 5 fixed global filters regardless of the active query (`timeline-source.c:14-18,843-851`) — narrowing is natively supported.
- **JSON boundary confirmed**: every query result is eagerly `ndb_note_json`-serialized (`ndb_backend.c:631-660`) although `ndb_query_result.note` is already a zero-copy pointer in the open txn; timeline reparses it just for the ID (`timeline-source.c:335-368`).
- **No native naddr index** exists (no compound kind+pubkey+d lookup) — our filter-based naddr resolution stands; `#d` should be added to the timeline query model when needed.

## Work Items (ranked by expected impact)

### Phase 1 — stop the quadratic/unbounded behavior
1. ✅ DONE (commit f16f2d56, nostrc-vpki) — **Linear keyed diff + single sort** — `gnostr-timeline-snapshot-model.c`: both sides are uniquely keyed and deterministically sorted; replace LCS with prefix/suffix trim + linear merge, detect same-ID replacement spans, coalesce `items_changed`. Add a sorted-rows constructor to `gnostr_timeline_snapshot_new()` (drop the second sort).
2. ✅ DONE (commit 98c95332, nostrc-myg6: 150-row window = page+2×30 buffers, pending-head cap 90 as lightweight records, 600-entry geometry LRU + per-event purge, anchor-preserving directional trim, edge-cursor refetch) — **Bounded window with coordinated eviction** — `gnostr-timeline-feed-controller.c` + `gnostr-timeline-geometry.c`: visible rows + head/tail buffers; evict working entries, deferred VMs, `by_event_id` keys, and geometry-cache keys together as they leave the window; keep only pagination cursors + anchor; bound pending-head (IDs, not full VMs).
3. ✅ DONE (commit 22d4466f, nostrc-qjde: blocks-cache-first via storage_ndb_get_blocks with per-block-slice sanitization preserving cached offsets, grow-on-demand fallback buffer, refcounted GnParsedContent shared hydrator→VM→snapshot→row→patches, quote/repost/geometry reparses removed, transfer-capped descriptors) — **Parse once, share everywhere** — carry one immutable parsed-content artifact (markup + plain text + descriptors + derived geometry) from hydration through VM and snapshot; skip parsing absent quote/repost; transfer-cap descriptors instead of double deep-copy; split mutable state (profile fields, counts) from immutable content so `copy_with_*()` shares content + geometry instead of cloning + reparsing.

### Phase 2 — fix the nostrdb boundary
4. ✅ DONE (commit b6271b91, nostrc-7wzy) — **Note-key query path** — add an ndb query API returning note keys/pointers (no JSON round-trip for IDs); populate batches directly under the open transaction; stop over-querying 100 to publish 30 (`timeline-source.c:864-891`).
5. ✅ DONE (commit b6271b91, nostrc-7wzy: per-batch pubkey-keyed flatbuffer cache w/ existence + last-fetch, JSON fallback removed) — **Per-author profile hydration** — per-batch (ideally longer-lived) pubkey-keyed cache with an `event_exists` flag; one lookup per author per batch instead of per note; kill the second existence query.

### Phase 3 — live path + patches
6. ✅ DONE (commit 900e7bae, nostrc-enbe: query-scoped subscriptions incl. search, 512-key deduped per-generation queues, ordered async hydration w/ generation cancellation) — **Scoped, coalesced, async ingestion** — narrow subscriptions to the active query where possible; coalesce keys into one bounded queue per source generation; use the existing async hydrator with generation cancellation; publish merged completions.
7. ✅ DONE (commit 900e7bae, nostrc-enbe: persisted ndb_note_meta counts verified equal to recomputation — reactions/replies/reposts read from LMDB, single-pass zap fallback, legacy double-increment removed; pubkey→WorkingEntry index + coalesced patches) — **Indexed patches** — `pubkey → WorkingEntry set` index; coalesce patch batches; recalculate only the metric the subscription kind affects (reaction-only update ≠ 4 aggregation passes, `timeline-source.c:555-612`).

### Phase 4 — bind cost + trims
8. **Create rich widgets on map, pool frames** — bind lightweight placeholders; construct grids/OG/embed/video subtrees on map; reuse compatible descriptor slots across recycles (`nostr-note-card-row.c:4310-4565`).
9. **Slim snapshot rows** — `{VM ref + geometry}`; getters proxy the VM; drop duplicated strings/hashtags and the redundant descriptor-array ref (`snapshot.c:117-273`).
10. **Small fixes** — fix `nostr_ndb_store.c:185-224` profile-JSON leak (real leak, do first); cache `GSettings` in `gnostr_is_remote_media_allowed()` (`utils.c:66-91`); pagination watermarks/exhaustion so edge-idling stops re-querying + recomposing (`feed-controller.c:847-902`); remove/repair passive geometry measurement that caches without refining (`geometry.c:321-336`, `feed-controller.c:346-380`).

### Validation (throughout)
Perf invariants as tests: page far past the window → assert bounded VM/row/cache counts; replace one row in a large snapshot → assert linear diff work; count parser invocations per hydration/patch; count rich-child creations during scroll recycling.

## References
- Review: oracle chat `timeline-efficiency-5AB726`; prior efficiency findings in `docs/investigations/gnostr-rich-media-segfault-2026-08-05.md`
- Design intent: `docs/designs/gnostr-timeline-compositor.md`
