# Critique: gnostr Rich Media Timeline Restoration Plan

**Scope**: `docs/plans/gnostr-rich-media-timeline-2026-08-03.md`. Bias toward deletion and clarification, not added detail. Findings below were spot-checked against the hydrator, VM, and geometry resolver.

## 1. Top 3 under-specified seams

**S1 — Markup/descriptor elision contract (blocking).** The hydrator does *two independent walks*: `render_markup_from_content()` (`gnostr-timeline-hydrator.c:194-215`) emits **every** URL and bech32 token as visible `<a>` text (`append_markup_for_token`, `:169-182`), while `extract_content_tokens()` (`:257-300`) separately harvests media/link arrays. Plan §1/WI-1 replaces the tokenizer but never says whether tokens promoted to descriptors are *removed from the markup*. If not, every image renders as URL text **plus** an image frame. If yes, the plan is silent on the knock-on: text height is estimated from raw `input->content` via `estimate_text_lines()` (`gnostr-timeline-geometry.c:337`), so eliding tokens shrinks rendered text without shrinking the reservation. Decide, and if eliding, the estimator must consume the same parse output.

**S2 — "Fixed slot heights" contradicts the live resolver.** Plan §1 asserts reserved height is "a pure function of descriptor counts and fixed per-class slot heights, computed at hydration." Actual policy is `(count, width_bucket)`-dependent with clamped aspect ratios — 16:9 for a single image, 2-column grid cells for N (`gnostr-timeline-geometry.c:274-295`), link previews at `content_width * 0.35` clamped 120–220 px (`:296-310`) — recomputed per width bucket with `initial_reserved_height` differenced out (`:337-343`). The plan doesn't say whether §1 *replaces* this or *feeds* it. Related and concrete: the layout signature carries only media/link counts (`:170-179`, and `vm-v1:...m%u:l%u` at `gnostr-timeline-item-view-model.c:236-243`). Adding an embed class without extending both signatures makes two rows with identical m/l but different embed counts collide in the measurement cache (`:251/:265`) and inherit each other's heights. Measurements are in-memory only, so a `GNOSTR_TIMELINE_GEOMETRY_LAYOUT_VERSION` bump (`gnostr-timeline-geometry.h:9`) is cheap — just say it happens.

**S3 — Media-service accounting and thread ownership.** WI-3 promises "byte-accounted LRU per resource class" and WI-4 routes the row's download path (`nostr-note-card-row.c:2500-2680`) through it, but the plan never fixes the accounting unit (decoded `GdkTexture` bytes vs. on-disk encoded bytes — they differ ~10×, and the budget means nothing until chosen) nor who decodes on which thread (`GdkTexture` creation is main-thread-bound; per-class budgets are the thing the settings UI in WI-9 exposes). One sentence resolves it.

## 2. Contradictions and missing dependencies

- **All three Open Questions (lines 90-92) are already answered upstream in the same document**: OQ1 by §1's Geometry contract, OQ2 by the per-note caps in §1 (4/2/3) and WI-2, OQ3 by WI-8's "new minimal account-removal action." Delete the section; the only live version of OQ1 is S2 above.
- **"All" vs. caps**: line 12 scopes "**all** `nostr:` refs and **all** link previews"; line 49 caps them at 2 previews / 3 embeds. Say "all, capped at N with a +N more affordance" once and drop the stronger claim.
- **WI-3 / WI-8 ordering**: the disk path `.../media/<npub|anon>/<class>/...` bakes tenancy into the layout on day one, but tenancy is sequenced last (8) and the service first (3). Either land the namespace inside WI-3 or accept a cache-path migration; the current "8 closes out" ordering implies neither.
- **WI-3 / WI-9 overlap**: WI-3 "absorb `cache_prune.c` image pruning… idle prune" and WI-9 "periodic/idle budget enforcement" are the same work item counted twice.
- **Legacy path fate unstated**: WI-4 deletes the inert reserved setters, but `GnNostrEventItem` Tier-2 rows still drive the rich path (`gnostr-timeline-view-app-factory.c:626-684`). The plan says the Tier-2 restriction "becomes moot" without saying whether two render paths coexist after WI-4 or one is deleted.

## 3. Over-planning — cut or simplify

- **§3 is a CDN in one work item.** Two tiers, per-class byte budgets, dedup, TTL negative cache, persisted OG metadata store, idle sweeps, tenancy namespaces — all before anything renders. v1 needs only: bytes-accounted memory LRU + in-flight dedup + body-size cap + cancellation. That unblocks WI-4/5/6 today. Disk tier, persisted OG store, and idle sweeps are a clean second pass.
- **Cut WI-7's GStreamer thumbnailer.** Ship the imeta-poster path plus a generic poster + play affordance. A bounded-concurrency one-shot video decoder with disk persistence is its own project and gates nothing else in this plan.
- **Cut the account-removal *action* from WI-8.** Building `evict_account()` and exercising it from a test is in scope; adding a `known-accounts` edit + keystore delete + UI to a media plan is scope creep. Ship the hook, file the UI separately.
- **Cut WI-9's settings UI.** GSettings keys without UI are sufficient for v1.
- **Drop blurhash placeholders (§2)** unless a decoder already exists — nothing in the plan claims one does. A spinner in a fixed frame is the same geometry.
- Net effect: 10 work items → ~6, with the geometry/parser seam (S1+S2) as the only genuinely hard part.

## 4. Questions whose answers change implementation order

1. **Does the markup elide promoted tokens?** If yes, WI-1 must ship together with the text-height estimator change and a layout-version bump *before* WI-2 — that serializes what the plan says can parallelize. If no, WI-1 is purely additive and 1→2 ∥ 3 holds.
2. **Does §1's fixed-slot geometry replace `resolve_media_reserved_height()`'s width-dependent aspect policy, or feed it?** Replacing makes `gnostr-timeline-geometry.c` the *first* file changed; feeding makes WI-2 a small field addition and §1's "fixed per-class slot heights" wording simply wrong.
3. **Is the disk tier in v1?** If no, tenancy collapses to clearing memory maps on account switch and WI-8 leaves the critical path entirely.
4. **Do embeds get their own reservation class, or reuse the link-preview class?** Determines whether WI-6 touches geometry at all, and whether the signature/version bump in S2 is needed.
5. **After WI-4, does the legacy `GnNostrEventItem` rich path survive?** If it must keep working, the shared parser needs two consumers from day one and `gnostr_render_content()`'s wrapper is load-bearing, not transitional.
