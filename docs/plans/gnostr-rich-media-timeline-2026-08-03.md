# gnostr Rich Media Timeline: Production Restoration Plan

## Goal

Restore inline rich content in gnostr timeline event cards — inline images, video posters (tap-to-play), OpenGraph link-preview cards, and rendered `nostr:` embeds — by extending the compositor snapshot pipeline (not bypassing it), backed by bounded, account-tenant-aware caches with nostrdb as the central index.

## Decisions (user-confirmed)

- **Extend the snapshot path**: keep the compositor architecture; carry rich-content descriptors in the immutable VM and hydrate reserved slots asynchronously without changing geometry.
- **Tenancy = both**: per-resource-class cache budgets *and* per-account partitioning with eviction on account removal.
- **Video**: poster + tap-to-play; no autoplay players in the scroll path.
- **Scope**: render every ref per note, capped per class with a "+N more" affordance (defaults: 4 media, 2 link previews, 3 event embeds). Profile refs (`npub`/`nprofile`) render as inline rich mentions, not cards; event refs (`note`/`nevent`/`naddr`) render as embed cards.

## Background

### The disconnect

The compositor refactor (commit `81854900` "Fix compositor review blockers", 2026-05-20) intentionally made snapshot rows inert as a stopgap: `apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c:968-992` binds snapshot rows via `set_precomputed_markup()` + `set_media_urls_reserved()`/`set_link_preview_urls_reserved()`, and those reserved setters discard their URL arguments and only reserve height (`nostr-gtk/src/nostr-note-card-row.c:3652-3728`). The prior plan (`docs/plans/gnostr-timeline-compositor-full-implementation.md`, Bucket 5) explicitly states placeholder-only output is not acceptable and requires async media fill **inside reserved boxes only** — visible geometry never changes after publication. The inert setter's own comment names the extension seam: *"A future snapshot with pre-resolved media VM data can fill this area through a separate immutable-data setter"* (`nostr-note-card-row.c:3686-3690`).

### What exists and works

- **Rich rendering machinery**: `gnostr_render_content()` (`nostr-gtk/src/content_renderer.c:321-559`, nostrdb content blocks), `set_content_rendered()`/`apply_deferred_content()` (`nostr-note-card-row.c:3476-3813`), `GnostrNoteEmbed`, `OgPreviewWidget`. Legacy `GnNostrEventItem` rows still use this path via a Tier-2 map handler (`gnostr-timeline-view-app-factory.c:626-684`).
- **Snapshot pipeline**: batch entry → `gnostr_timeline_hydrator_hydrate_entry()` (`apps/gnostr/src/model/gnostr-timeline-hydrator.c:385-499`) → immutable `GnostrTimelineItemViewModel` (has `links`/`media_urls`; **no nostr-ref descriptors**) → `GnostrTimelineSnapshotRow` → factory bind. Reserved heights come from the geometry resolver's width-bucket-dependent policy (`gnostr-timeline-geometry.c:274-310`).
- **Row lifecycle**: robust quiesce/cancellation on unbind (`nostr-note-card-row.c:425-514`, `6860-6940`) with binding IDs and per-media cancellables.
- **Cache substrate**: avatar LRU (200 textures) + disk with startup prune (`gnostr-avatar-cache.c`, `cache_prune.c`), nostrdb/LMDB central store (`main_app.c:353-377`), profile provider LRU 3,000 with request dedup/batching (`nostr_profile_provider.c`, `nostr_profile_service.c`), shared libsoup session 24 conns/6 per host (`apps/gnostr/src/util/utils.c:9-44`).

### Production gaps (confirmed with file:line)

1. **Inline images**: process-global 50-texture LRU, no byte accounting, no disk tier, no cross-row in-flight dedup (`nostr-note-card-row.c:99-157, 2521-2671`).
2. **OG previews**: per-widget 100-entry metadata cache that dies with the widget; no shared/persistent cache, no dedup, no body-size cap, non-cancellable (`og-preview-widget.c:10-11, 323-400, 531-537, 575-597`).
3. **Note embeds**: nostrdb-first then relays (good), but no cross-embed request dedup; `naddr` resolution broken — kind/identifier discarded (`gnostr-note-embed.c:441-540, 603-611`); `nprofile` relay hints ignored.
4. **Unbounded state**: avatar negative-URL cache (process lifetime), profile-service pending table, no app-level download queue bound.
5. **Hydrator parsing is weaker than the legacy renderer**: whitespace/suffix heuristics (`gnostr-timeline-hydrator.c:194-308`); nostr refs exist only as markup text, never structured.
6. **No tenancy**: all caches app-global; logout evicts nothing (`gnostr-main-window-auth.c:713-768`); no account-removal flow exists.
7. **nostrdb has no retention policy**: over-budget only warns (`cache_prune.c:260-284`).

## Approach

### 1. Structured descriptors at hydration time

Factor `gnostr_render_content()`'s nostrdb-blocks walk into a shared parser, `gn_content_parse()`, emitting **both** Pango markup and an ordered, typed descriptor array in one pass:

- `MEDIA_IMAGE` / `MEDIA_VIDEO` — URL + optional imeta dims/thumb from event tags (NIP-92)
- `LINK_PREVIEW` — URL (link text also stays in markup)
- `NOSTR_EVENT_REF` — note/nevent/naddr id + relay hints (token **elided** from markup)
- `NOSTR_PROFILE_REF` — pubkey + hints (rendered as inline mention markup, no card)

**Elision contract**: media URLs and event-ref tokens are removed from the markup (no URL-text + frame double render). Because text height is estimated from raw content (`estimate_text_lines()`, `gnostr-timeline-geometry.c:337`), the estimator must consume the same parse output, shipped together with a `GNOSTR_TIMELINE_GEOMETRY_LAYOUT_VERSION` bump (`gnostr-timeline-geometry.h:9`). This serializes parser → geometry → VM plumbing.

The parser keeps two consumers: the hydrator (snapshot path) and `gnostr_render_content()` as a load-bearing wrapper for the legacy Tier-2 path (thread view, profile view, non-snapshot rows), which continues to work unchanged.

**Geometry**: descriptors *feed* the existing resolver — media keeps its width-bucket aspect-clamped policy (`gnostr-timeline-geometry.c:274-295`), link previews keep theirs (`:296-310`); event embeds get a new reservation class with a fixed compact-card height. The layout signature gains the embed count (currently only `m%u:l%u`, `gnostr-timeline-item-view-model.c:236-243`) so measurement-cache rows can't collide. Reservation inputs (counts, capped) are fixed at hydration; fetched bytes never change geometry — images letterbox/crop into the reserved frame.

### 2. Hydrating reserved slots on the row

New immutable-data setter `nostr_gtk_note_card_row_set_rich_content(descriptors, reserved_heights)` replacing the inert reserved setters. At bind it builds fixed-size placeholder frames (spinner in frame); on map — reusing the existing map-handler + 40 ms scroll debounce pattern (`nostr-note-card-row.c:2700-2879`) — each frame requests its asset from the media service and fills **inside** the frame. Existing binding-id + per-frame cancellable machinery guards recycling. The factory snapshot branch switches to this API.

### 3. `GnostrMediaService` — one bounded asset service, phased

New central service (`apps/gnostr/src/services/gnostr-media-service.[ch]`) that inline media, OG (metadata + images), video posters, and eventually avatars go through.

**Phase 1 core** (unblocks all rendering):
- Byte-accounted memory LRU per resource class — memory tier accounts *decoded* texture bytes (w×h×4); decode stays on GTask workers, delivery on main loop.
- In-flight dedup: URL-keyed pending table with multi-subscriber callbacks (pattern from `nostr_profile_service.c:21-63`), hard-capped with overflow rejection.
- Fetch hygiene: shared libsoup session, per-request body-size caps, cancellable, bounded concurrency; TTL'd + bounded negative cache.
- Shared OG metadata memory cache (replaces the per-widget throwaway table).
- Budgets read from GSettings (`image-cache-max-mb` reused for inline media; new per-class keys; no settings UI).

**Phase 2 durability + tenancy**:
- Disk tier at `$XDG_CACHE_HOME/gnostr/media/<npub|anon>/<class>/<sha256(url)>` — the account-namespaced path layout is used from the first byte written, so no migration later. Disk accounts encoded bytes.
- Persisted OG metadata store with TTL.
- `gnostr_media_service_evict_account(npub)` deleting the namespace; budget enforcement on write + low-priority idle sweeps (absorbing `cache_prune.c` image pruning).
- nostrdb stays app-global — it is the shared protocol index, not a tenant asset store; retention remains out of scope (tracked separately).

**Video**: hydration classifies video URLs; slot shows a poster (imeta `image`/`thumb` via media service; else a generic poster + play affordance) and tap instantiates `GnostrVideoPlayer`. A GStreamer one-shot thumbnailer for imeta-less videos is Phase 2.

**Embeds**: `GnostrNoteEmbed` stays nostrdb-first, gains a shared event-request dedup table keyed by event id. Profile refs go through the existing profile provider/service.

## Work Items

### Phase 1 — restore rendering (bounded memory)

1. **Shared parser + geometry contract** — extract `gn_content_parse()` from `content_renderer.c:321-559` (markup + descriptors + elision, imeta enrichment); switch `estimate_text_lines()` input to parse output; bump `GNOSTR_TIMELINE_GEOMETRY_LAYOUT_VERSION`; extend layout signature with embed count. `gnostr_render_content()` becomes a wrapper (legacy path keeps working). Unit tests for classification + elision.
2. **VM + snapshot plumbing** — descriptor array + embed reservation class through `GnostrTimelineItemViewModelSpec`/VM, `GnostrTimelineSnapshotRow`, hydrator (`hydrate_entry`, replacing `extract_content_tokens`); per-note caps applied here. Depends on 1.
3. **Media service core** — memory LRU (decoded-byte accounting), in-flight dedup, body-size caps, TTL negative cache, OG metadata cache, GSettings budgets. Parallel with 1–2.
4. **Row hydration API + factory switch** — `set_rich_content()` on `nostr-note-card-row.c` (fixed frames, map-gated fill via service); switch snapshot branch (`gnostr-timeline-view-app-factory.c:968-992`); remove inert reserved setters; route row image path (`:2500-2680`) through the service. Depends on 2+3.
5. **OG + embeds, all refs (capped)** — `OgPreviewWidget` becomes a view fed by service results (cancellable, deduped); every `NOSTR_EVENT_REF` renders as a fixed-height compact card; event-request dedup table in `gnostr-note-embed.c`. Depends on 4.
6. **Video posters** — imeta poster path + generic poster + tap-to-play `GnostrVideoPlayer`. Depends on 4.

### Phase 2 — durability + tenancy

7. **Disk tier + persisted OG store** — namespaced path layout, encoded-byte budgets, write-time enforcement + idle sweeps (absorb `cache_prune.c` image pruning).
8. **Account eviction** — `evict_account(npub)` + tests; wire to account-removal hook. (Account-removal UI is a separate issue; logout alone does not evict.)
9. **Thumbnailer** — bounded one-shot GStreamer poster extraction for imeta-less videos, persisted to disk tier.

### Validation (throughout)

- Geometry invariance: row height identical before/after hydration (assert in debug builds).
- Scroll-jank check on media-heavy feeds; leak/cancellation check on rapid scroll.
- Cache-bound tests: budgets respected, negative-cache TTL, dedup coalesces, account eviction removes the namespace.

## Deferred / follow-ups (file as bd issues)

- `naddr` resolution repair (kind/identifier currently discarded) and `nprofile` relay-hint forwarding.
- Account-removal UI (known-accounts edit + keystore delete via `gnostr_identity_delete()`).
- nostrdb retention/eviction policy.
- Settings UI for per-class cache budgets.
- Migrating the avatar cache into `GnostrMediaService` (bounded negative cache included).

## Status (orchestration tracking — epic nostrc-9rs0)

- [x] WI-1 parser + geometry contract — `nostrc-obhs` (commit 8cacaeb3: gn_content_parse() + elision, geometry v3, embed-count signatures, 5/5 tests)
- [x] WI-2 VM + snapshot plumbing — `nostrc-1qpw` (commit 5774d4c1: hydrator uses gn_content_parse, descriptors + caps + embed reservation class through VM/snapshot, 4/4 tests)
- [x] WI-3 media service core — `nostrc-ovnu` (commit 72a103fb: gnostr-media-service.[ch], 4/4 unit tests, schema keys)
- [x] WI-4 row hydration API + factory switch — `nostrc-2zuz` (commit 7211edbe: set_rich_content() + loader injection, factory switched, inert setters removed, geometry safeguards)
- [x] WI-5 OG + embeds all refs — `nostrc-l25c` (commit 77ca1372: OG widget fed by service, embed cards with event-ID dedup)
- [x] WI-6 video posters — `nostrc-qqz5` (commit 77ca1372: imeta/extracted posters, tap-to-play in fixed frame)
- [x] WI-7 disk tier + persisted OG — `nostrc-dvsd` (commit 03cee1d4: namespaced disk tier, disk-before-network, write-time + idle pruning, persisted OG TTL, 7/7 tests)
- [x] WI-8 account eviction — `nostrc-e6yd` (commit 7adcec3b: evict_account with namespace generations, in-flight safety, tests)
- [x] WI-9 thumbnailer — `nostrc-gykl` (commit 7adcec3b: bounded GStreamer one-shot extraction, concurrency 2, graceful fallback, persisted posters)

Follow-ups filed: `nostrc-pkah` (naddr/nprofile), `nostrc-ub72` (removal UI), `nostrc-8rxk` (ndb retention), `nostrc-uexn` (settings UI), `nostrc-kklz` (avatar migration).

## References

- `docs/designs/gnostr-timeline-compositor.md` — compositor design (immutable reading surface)
- `docs/plans/gnostr-timeline-compositor-full-implementation.md` — Bucket 5: rich-content parity requirement
- `docs/plans/gnostr-timeline-compositor-definitive-2026-05-20.md` — geometry-stability goals
- `docs/reviews/gnostr-rich-media-timeline-critique-2026-08-03.md` — design critique folded into this revision
- Commits: `568732a1` (compositor), `81854900` (inert placeholders), `9893c943`/`d1a18bf0`/`04945d0f` (partial parity restoration)
