# Investigation: Segfault after extended runtime — rich media timeline changes

## Summary
A three-pass audit (oracle full-selection review → pair verification with file:line evidence → oracle synthesis) found **no direct use-after-free reachable during ordinary timeline scrolling on current master**. The primary rich-media ownership chains (descriptors parse→VM→snapshot→row, row recycling guards, media-service GTask worker data) are balanced. The audit did confirm **three normally-reachable memory leaks**, **unbounded background-work submission** that loads GLib's shared worker pool over long sessions, and **six dormant-but-real UAF landmines** in public APIs with zero current callers. The reported extended-runtime segfault could not be pinned to a confirmed defect; a symbolicated crash from the failing build is required (instrumentation plan below).

## Symptoms
- gnostr app segfaults after running for an extended period (not at startup, not immediately reproducible).
- Onset follows the rich-media timeline restoration work (commits below), implying an accumulation- or recycling-related defect: use-after-free on recycled rows, callback-after-dispose, unremoved sources/handlers, or ref-count leaks that eventually corrupt state.

## Scope: recent commits under review
- `8cacaeb3` — shared content parser + geometry contract (content_renderer.c, gnostr-timeline-geometry.*)
- `72a103fb` — GnostrMediaService core (apps/gnostr/src/services/gnostr-media-service.[ch])
- `5774d4c1` — descriptor plumbing (hydrator, view-model, snapshot)
- `03cee1d4` — media disk tier + persisted OG store
- `7adcec3b` — account eviction + video thumbnailer (namespace generations, GStreamer one-shot)
- `7211edbe` — row rich-content hydration + factory switch (nostr-note-card-row.c, app factory)
- `77ca1372` — OG previews via service, embed cards + event-ID dedup, video posters/tap-to-play
- `351777fa` — naddr resolution + nprofile relay hints (gnostr-note-embed.c, nostr_profile_service.c)

## Initial Hypotheses
1. **Callback-after-dispose on recycled rows**: media-service async texture callbacks (or OG/embed results) delivered to a `NostrGtkNoteCardRow` (or child frame widget) that was recycled/disposed; binding-id or cancellable checks missing in one of the new fill paths.
2. **Dedup-table dangling subscribers**: the multi-subscriber in-flight tables (media service URL table; note-embed event-ID table with "weak subscribers"; profile-service hint retries) retain pointers to freed subscriber state — weak pointers added but never removed (or vice versa), corrupting the table over time.
3. **Sources/handlers not torn down**: 40 ms debounce sources, idle disk-sweep sources, map handlers, or GStreamer thumbnailer callbacks firing after their user_data is freed.
4. **Worker-thread delivery race**: GTask decode completing against main-loop teardown (texture delivered after service/widget finalization), or namespace-generation eviction racing in-flight disk writes.
5. **Descriptor ownership double-free / dangling**: descriptor arrays copied between parse result → VM spec → snapshot row → row widget with unclear transfer semantics; one side frees what another still references (bites only when rows recycle heavily, i.e., after extended scrolling).

## Background / Prior Research
- No macOS crash reports exist for the `gnostr` app binary itself (user's crashes not captured — likely launched from terminal without crash reporter pickup, or reports pending).
- **Three crash reports for `gnostr-test-media-service`** (2026-08-04 01:13, during WI-3 development, ASan build): `EXC_BAD_ACCESS KERN_INVALID_ADDRESS at 0x0` with the faulting thread inside `g_task_thread_pool_thread` → a **GTask worker thread dereferenced NULL**. The committed test now passes, but the crash class (worker-thread task data invalid at execution time) matches hypothesis 4 and may persist in the app under race conditions the test doesn't exercise.

## Investigator Findings
### Scope and reachability standard

Reviewed current master (`05a490d32fa6683bb5053b4dc44abea4228aa879`). “Runtime-reachable” below means reachable while the app remains open and a user scrolls/recycles rows in a live timeline. A defect reachable only from an unused public API, tests, explicit account removal, or shutdown is not treated as a cause of the reported live-scroll crash.

### Summary verdict

| # | Claim | Verification | Normal live-scroll reachability | Segfault assessment |
|---|---|---|---|---|
| 1 | Profile GTask result UAF | **CONFIRMED defect** | **No current caller** | Deterministic UAF if the API is adopted, but dormant now |
| 2 | `fire_callbacks()` callback-copy leak | **CONFIRMED** | **Yes** | Accumulation only; weak direct crash fit |
| 3 | `check_ndb_cache()` JSON leak | **CONFIRMED** | **Yes** | Accumulation only; weak direct crash fit |
| 4 | Profile shutdown UAF | **CONFIRMED defect** | **No app caller; tests/teardown only** | Direct UAF under that sequence, not the reported runtime path |
| 5 | Embed request-table dangling subscribers | **MOSTLY REFUTED / PARTIAL** | Table path is safe; borrowed cancellable is latent | No verified ordinary-scroll UAF |
| 6 | `RichHydrationContext` timeout UAF | **REFUTED** | Path is live, teardown is balanced | Not a candidate |
| 7 | Media-service GTask task-data UAF | **REFUTED for worker/task data**; adjacent callback-reentrancy UAF confirmed | Workers are live and ownership-safe; reentrancy trigger absent in app | Historical ASan signature not explained by current code |
| 8 | OG label leak / same-URL cancellation | **CONFIRMED** | Label leak can occur on disposal; same-URL path is not used by current row construction | Memory pressure / stale work, not direct corruption |
| 9 | Plain `NoteCardBindingContext.cancelled` race | **REFUTED** | Readers/writers are main-context paths | Not a candidate |

### 1. Profile GTask result UAF — **CONFIRMED, but dormant**

**Ownership evidence.**

- `profile_request_gtask_bridge_cb()` puts the callback's borrowed `meta` directly into the task with no destroy notifier: `nostr-gobject/src/nostr_profile_service.c:904-912`.
- `gnostr_profile_service_request_gtask_finish()` propagates that same pointer: `nostr-gobject/src/nostr_profile_service.c:924-930`.
- The network-delivery producer obtains a profile, calls `fire_callbacks()`, and immediately frees it: `nostr-gobject/src/nostr_profile_service.c:289-292`.
- The NDB-cache producer does the same: `nostr-gobject/src/nostr_profile_service.c:538-546`.
- GTask completion is delivered asynchronously, after the synchronous bridge callback returns, so a future finish caller can receive an already-freed `GnostrProfileMeta *`.

**Caller/reachability audit.** The only current references are the declarations in `nostr-gobject/include/nostr-gobject-1.0/nostr_profile_service.h:175-203` and the implementation at `nostr_profile_service.c:904-930`. There are **zero external callers**, including tests. Live embeds instead use `gnostr_profile_service_request_with_hints()` at `nostr-gtk/src/gnostr-note-embed.c:1452-1461`; the profile pane uses the legacy callback API at `nostr-gtk/src/gnostr-profile-pane.c:4229-4238`; the profile-list model does likewise at `apps/gnostr/src/model/gn-profile-list-model.c:806-817`.

**Verdict.** This exported API is memory-unsafe, but it cannot explain the current app crash because no app path calls it. Rank it highly for severity if adopted, very low for the reported incident.

### 2. `fire_callbacks()` leaks copied `PendingCallback` objects — **CONFIRMED and reachable**

- The original request callback array has `pending_callback_free` as its free function: `nostr-gobject/src/nostr_profile_service.c:83-104`.
- The delivery copy is instead created with bare `g_ptr_array_new()`: `nostr_profile_service.c:197-198`.
- One heap `PendingCallback` copy is allocated per callback at `nostr_profile_service.c:199-205`.
- `g_ptr_array_free(to_fire, TRUE)` at `nostr_profile_service.c:226` frees the pointer vector but, because the array has no element free function, does not free the copied callback records.

This runs for cache delivery at `nostr_profile_service.c:538-546` and network delivery at `nostr_profile_service.c:289-292`. The live embed profile path at `nostr-note-embed.c:1452-1461` makes it normally reachable.

**Verdict.** A real per-delivery leak during timeline use. It can contribute to long-run growth but is too small and structurally unlike a NULL dereference in a GTask worker to be a leading direct segfault cause.

### 3. `check_ndb_cache()` JSON leak — **CONFIRMED and reachable**

- `check_ndb_cache()` receives `json` at `nostr-gobject/src/nostr_profile_service.c:172-175`, uses it to update the provider at `:179-180`, and returns without releasing it at `:181-184`.
- The underlying nostrdb backend allocates the returned serialization buffer with `realloc()` and transfers it through `*json`: `libnostr/src/store/nostrdb/ndb_backend.c:782-799`.
- The storage ownership documentation explicitly says getter results are heap strings freed by the caller: `docs/STORAGE.md:58-63`. Other callers follow that contract, for example `apps/gnostr/src/model/gn-nostr-event-model.c:434-439`.

The path runs whenever the provider LRU misses but nostrdb contains the profile: `nostr_profile_service.c:157-184, 538-546`, including profile requests issued by live embeds.

**Verdict.** A real, normally reachable variable-size leak. It is a stronger accumulation finding than claim 2, but still does not match a worker-thread NULL dereference.

### 4. Profile-service shutdown UAF — **CONFIRMED, not normal-runtime reachable**

- `BatchFetchCtx` stores a raw `GnostrProfileService *`: `nostr-gobject/src/nostr_profile_service.c:235-239`.
- It is handed to the asynchronous pool query at `nostr_profile_service.c:476-488`.
- Completion immediately dereferences the raw service and mutex: `nostr_profile_service.c:241-257`, and later writes `fetch_in_progress` and dispatches again at `:335-340`.
- Shutdown cancels the query, destroys request/batch state and pool references, clears the mutex, and frees `svc` immediately: `nostr_profile_service.c:832-898`. Cancellation can still schedule the async completion, producing a direct callback-after-free.

Repository-wide callers of `gnostr_profile_service_shutdown()` are only `nostr-gobject/tests/test_profile_service_batching.c` plus its declaration/definition; there is no app call during account switching, relay reconfiguration, or ordinary exit.

**Verdict.** Definite unsafe public teardown behavior, but incompatible with the reported “app remains open while scrolling” scenario on current master.

### 5. Embed `pending_event_requests` table — **TABLE LIFETIME REFUTED; borrowed cancellable PARTIAL**

**Table and weak-ref symmetry are correct.**

- The table owns keys and destroys values with `pending_event_request_free`: `nostr-gtk/src/gnostr-note-embed.c:1343-1346`.
- Subscribers initialize a weak ref and take a strong ref to the effective cancellable at `gnostr-note-embed.c:1302-1317`.
- Completion uses `g_weak_ref_get()`, skips cancelled/dead/stale widgets, and unreferences live embeds: `gnostr-note-embed.c:1134-1160`.
- Every final success/error/empty-result path reaches `complete_pending_event_request()` and removes the table entry at `gnostr-note-embed.c:1163-1208`.
- Removing the entry clears the request cancellable and subscriber array at `gnostr-note-embed.c:1086-1098`; subscriber destruction disconnects cancellation, releases the cancellable, and clears the weak ref at `:1072-1083`.
- Hinted failures fall back once to the main pool at `:1176-1184`; pool timeout still completes through the same callback (`nostr-gobject/src/nostr_pool.c:683-711, 843-859`).

An individually cancelled subscriber is not removed immediately: cancellation only sets an atomic flag at `gnostr-note-embed.c:1063-1069`. It can remain until the shared query/fallback completes (roughly up to 60 seconds for two 30-second attempts), but it owns a cancellable ref and only a `GWeakRef` to the widget. This is bounded retention, not a dangling widget pointer. The static empty table itself persists for process lifetime.

**Borrowed external cancellable.** The field is explicitly non-owning at `gnostr-note-embed.c:76-78`; `gnostr_note_embed_set_cancellable()` merely assigns it at `:1469-1472`; later subscriber creation refs it at `:1308-1314`. This API is intrinsically fragile and `dispose()` does not clear the field (`:117-164`). However, all current callers pass the row's `async_cancellable`, then immediately call a target setter (`nostr-note-card-row.c:4163-4168, 4374-4382, 4445-4449, 4864-4867`). The target setters perform their NDB check synchronously (`gnostr-note-embed.c:890-971`), and a miss synchronously creates a subscriber that takes its own strong cancellable ref (`:1302-1379`) before control returns to the GTK main loop. Row recycling therefore cannot interleave in the suspected borrowed-pointer window. Once established, the subscriber owns its reference.

**Verdict.** No table-entry UAF or missing weak-ref cleanup was found. The borrowed-pointer API should be fixed, but it is not a verified normal-scroll crash path in current callers.

### 6. `RichHydrationContext` raw-pointer timeout — **REFUTED**

- The context owns a binding-context ref and weak widget ref: `nostr-gtk/src/nostr-note-card-row.c:4106-4116, 4214-4223`.
- It is owned by frame qdata with `rich_hydration_context_free` as destroy notify: `nostr-note-card-row.c:4225-4230`.
- The destroy notify removes a pending source before releasing/freeing the context: `nostr-note-card-row.c:4118-4131`.
- Map creates at most one 40 ms source; unmap removes it: `nostr-note-card-row.c:4184-4204`.
- The callback clears `timeout_id` before doing anything, then obtains strong row/widget references through the ref-counted binding context and `GWeakRef`, validates binding ID/mapping, and releases both: `nostr-note-card-row.c:4134-4181`.

All of this runs on the GTK main context. If qdata is destroyed before dispatch, its destroy notify removes the source. If the callback is dispatching, it first zeroes the ID so later qdata destruction cannot remove/re-free the active source. The context itself has only one freeing owner (frame qdata); the source has no competing destroy notifier.

**Verdict.** Teardown ordering is balanced; no double-free or UAF was found.

### 7. Media-service GTask task-data lifetime — **WORKER UAF REFUTED; adjacent reentrancy defect found**

**Pending-request lifetime invariant.** A request is only freed when network work is done, every subscriber is completed, and `outstanding_workers == 0`: `apps/gnostr/src/services/gnostr-media-service.c:1101-1110`. Eviction completes/cancels work but calls the same gate: `gnostr-media-service.c:3123-3158`.

**Worker-by-worker audit.**

- **Texture decode:** task data owns a `GBytes` ref and dimensions (`:1148-1159, 1287-1297`). `DecodeJob` points to the request, but the worker count is incremented before dispatch and decremented only in completion after all uses (`:1278-1297, 1220-1266`).
- **OG parse:** task data owns a `GBytes` ref and duplicated URL (`:1419-1430`); the request worker count brackets dispatch/completion (`:1491-1532, 1546-1556`).
- **Disk lookup:** `DiskLookupJob` duplicates path, URL, namespace, epoch and limits and has a task-data destroy notify (`:1568-1594, 2039-2070`). The GTask source object strongly owns the service; the raw request callback data remains gated by its worker count (`:1994-2038`).
- **Thumbnailer:** `ThumbnailJob` remains both task data and completion data until `thumbnail_done()` frees it; the request count is incremented before dispatch and decremented in completion (`:1861-1921, 1931-1953`). Eviction cancellation cannot free the request while this count is nonzero.
- **Detached texture write:** the job duplicates namespace/URL, owns a body `GBytes` ref, and is task-owned (`:2073-2091, 2174-2193`).
- **Detached OG write:** all metadata strings are duplicated and task-owned (`:2196-2221, 2315-2341`).
- **Sweep:** the idle dispatch holds a service ref; the job is transferred with `g_steal_pointer()` into task data; both dispatch and task have destroy paths (`:2383-2432`).
- Detached tasks use the service as the GTask source object (`:2189, 2337, 2402`), so the service, config, disk root and epoch table outlive worker execution. Finalize only tears these down at `:3256-3283`.
- Epoch reads/writes are serialized by `media_disk`: helpers at `:349-379`; disk workers at `:1634-1639, 2115-2121, 2293-2299`; account eviction advances the epoch and removes the tree under the same lock at `:3078-3091`.

A `NULL` cancellable in detached jobs is not a defect: [GLib documents `g_cancellable_is_cancelled(NULL)` as returning false](https://docs.gtk.org/gio/method.Cancellable.is_cancelled.html).

**Adjacent direct UAF (not task data).** Delivery is not reentrancy-safe. `pending_fail()` invokes arbitrary subscriber callbacks while retaining a raw request and continuing its loop, then calls `pending_maybe_finish()`: `gnostr-media-service.c:1125-1145`. If such a callback synchronously calls `gnostr_media_service_evict_account()`, eviction can complete remaining subscribers and free the request at `:3123-3158`; the outer delivery frame resumes on freed memory. Similar completion loops have the same API-level hazard. Current app code has **no call** to `gnostr_media_service_evict_account()` (only tests and the definition), and ordinary widget cancellation is deliberately deferred through an idle source (`:832-917`), so this is not reachable through normal timeline callback behavior. It also would fault on the main context, not in `g_task_thread_pool_thread`.

**Empirical check.** The existing `build/apps/gnostr/gnostr-test-media-service` was run with `MallocScribble=1 MallocPreScribble=1 G_DEBUG=fatal-warnings`; all 10 cache, cancellation, disk, OG, eviction, in-flight eviction and thumbnail tests passed. The binary is not ASan-linked, so this is a quick allocator-poisoning smoke test, not proof.

**Verdict.** No current worker task-data invalidation, eviction race, or service-finalize race was found. The historical ASan worker NULL dereference is not reproduced or explained by current master. The callback-reentrancy UAF is real but requires an app trigger that does not exist today.

### 8. OG widget — **CONFIRMED non-crashing defects**

**Deliberate label leak.** During dispose, if a nonempty label no longer has a native widget, the macro takes an extra reference and never releases it: `apps/gnostr/src/ui/og-preview-widget.c:324-333`. `GNOSTR_LABEL_SAFE` only accepts labels with a native at `apps/gnostr/src/ui/gnostr-label-guard.h:6-11`. The fields are then nulled/unparented at `og-preview-widget.c:342-353`, making the reference intentionally unreachable. This avoids a known Pango finalization crash but leaks labels on the fallback path. Embed disposal uses the same strategy at `nostr-gtk/src/gnostr-note-embed.c:139-150`.

**Same-URL parent cancellation.** `og_preview_widget_set_url_with_cancellable()` disconnects the prior parent and stores a ref to the new one at `og-preview-widget.c:526-535`. `og_preview_widget_set_url()` can return for the same URL at `:500-507`, before `restart_cancellable()` connects the new parent at `:508-513`. The new parent is retained but has no cancellation handler, so stale work continues.

Current row callers create a **new** `OgPreviewWidget` immediately before each call (`nostr-note-card-row.c:3757-3762, 4355-4363, 4469-4474, 4907-4913`), and a rich hydration context starts only once (`:4157-4161`). Thus same-widget/same-URL rebinding is not part of the current scrolling path.

**Verdict.** The label leak is live and potentially accumulative; the same-URL bug is real but not currently app-reachable. Neither directly explains a worker-thread segfault.

### 9. `NoteCardBindingContext.cancelled` plain boolean — **REFUTED as a race**

- The field is a plain `gboolean`: `nostr-gtk/src/note-card-binding-ctx.c:20-30`.
- Writes occur only in `note_card_binding_context_cancel()` at `:72-84`, called from row quiesce/rebind on GTK lifecycle paths (`nostr-note-card-row.c:458-462, 7555-7563`).
- Reads occur in `is_cancelled()` and `get_row()` at `note-card-binding-ctx.c:86-118`, from GTK signal/source callbacks and media completion handlers.
- The media API explicitly guarantees callbacks on the main context that constructed the service: `apps/gnostr/src/services/gnostr-media-service.h:146-155`. Worker functions return through GTask; they do not inspect or mutate `NoteCardBindingContext`.

**Verdict.** All observed readers/writers are main-context-only. No data race was found.

### Targeted GLib-footgun sweep and additional findings

- **Signals:** rich frame map/unmap handlers use context owned by the same frame (`nostr-note-card-row.c:4225-4232`); embed/OG gesture senders are widget-owned children (`gnostr-note-embed.c:279-281`, `og-preview-widget.c:485-487`); settings signals originate from the service-owned settings object and the service is a process singleton (`gnostr-media-service.c:2919-2945, 3248-3253`). Sender destruction removes these handlers. No short-lived widget was found connected to a longer-lived new rich-media object without disconnection.
- **Sources:** media cancel/deferred/sweep sources all have destroy notifiers that retain/free their data (`gnostr-media-service.c:912-917, 1047-1052, 2427-2432`). Row 40 ms sources are removed by unmap/context destruction. No new unowned timeout/idle data was found.
- **Weak references:** reviewed `GWeakRef` init/get/clear and legacy `g_object_weak_ref/weak_unref` pairs in the changed rich paths; no asymmetry was found.
- **Steals/double frees:** the sweep transfer at `gnostr-media-service.c:2402-2407` correctly steals the job before the dispatch destroy notify; no missing `g_steal_pointer` double-free was found.
- **Additional normally reachable non-crash bug:** a debounce that fires while a profile fetch sequence is active replaces `svc->fetch_batches` at `nostr_profile_service.c:568-590`. Remaining old batches are dropped while their requests were already marked `in_flight` at `:506-517`, so those requests can remain pending indefinitely. New live profile requests can trigger this while a prior multi-batch fetch is active. This is request loss/retention, not a segfault.
- **Additional dormant pool setter UAF:** `gnostr_profile_service_set_pool()` unreferences an internally owned old pool before referencing the supplied pointer at `nostr_profile_service.c:799-820`; passing the borrowed result of `get_pool()` back to `set_pool()` can ref freed memory. There are no repository callers of either API.

### Final ranking for the reported live-scroll segfault

1. **Accumulation from deliberate rich-label leaks plus profile-service leaks (claims 2, 3, 8)** — the only defects above that are both confirmed and routinely reachable during extended scrolling. They could create memory pressure over time, especially because each rich OG/embed widget can leak multiple labels after losing its native. **Fit is still weak:** memory pressure normally produces allocation failure/termination, not a clean NULL dereference in a GTask worker.
2. **Media callback reentrancy UAF (adjacent to claim 7)** — a genuine direct UAF in recent media code, but current app callbacks do not call account eviction and the app has no eviction caller. It is therefore dormant for normal use and does not match the worker-thread stack.
3. **Embed borrowed external cancellable (claim 5)** — intrinsically unsafe ownership, but current target setup/local lookup/subscriber creation is synchronous and closes the normal recycling window. Low likelihood without a future asynchronous pre-query stage or different caller.
4. **Profile shutdown UAF (claim 4)** — direct and deterministic if shutdown occurs with a query in flight, but shutdown is not called by the app and the reported crash is not at exit.
5. **Profile GTask result UAF (claim 1)** — direct and deterministic if used, but there are zero current callers.
6. **Media worker task-data lifetime (claim 7), rich timeout teardown (claim 6), and binding-context boolean race (claim 9)** — audited and refuted as current candidates.

**Bottom line:** none of the verified direct UAFs is reachable during ordinary current-master live timeline scrolling. The only confirmed normal-runtime issues are accumulative leaks and profile request loss. The current media-service task-data ownership does **not** support the ASan-like “invalid GTask worker data” hypothesis. Capturing a fresh symbolicated crash (ASan with symbols or lldb backtrace) is necessary to identify the reported runtime segfault rather than selecting one of these dormant defects by severity alone.

## Investigation Log

### Phase 1 — Triage
**Hypotheses:** callback-after-dispose on recycled rows; dangling dedup-table subscribers; source/handler teardown gaps; GTask worker data lifetime; descriptor double-free.
**Evidence gathered:** no crash reports for the app binary; 3× ASan dev crashes in `gnostr-test-media-service` (2026-08-04): NULL deref inside `g_task_thread_pool_thread`.

### Phase 2 — Oracle full-selection audit (17 files, ~115k tokens)
Surfaced 9 candidate defects across profile service, media service, embeds, OG widget, row hydration, plus efficiency findings. Descriptor ownership chain assessed clean.

### Phase 3 — Pair verification (all 9 claims, file:line)
See `## Investigator Findings`. Outcome: 4 claims CONFIRMED (2 reachable leaks + 2 dormant UAFs), 1 PARTIAL, 4 REFUTED with evidence; 3 additional defects found (media callback-reentrancy UAF — dormant; profile batch replacement request-loss — reachable; set_pool UAF — dormant). Empirical: 10/10 media-service tests pass under MallocScribble/PreScribble + fatal-warnings.

### Phase 4 — Adversarial second look (oracle synthesis)
Fresh targets all cleared: texture-LRU eviction vs GtkPicture rendering (both hold refs — safe); tap-to-play wrapper (binding-context retained across synchronous child destruction — safe); snapshot publication vs bound-row descriptor lifetime (strings duplicated before async use — safe); injected media-loader vtable (safe with the current singleton; unsafe as a general contract). OOM-→-SEGV paths reviewed: GLib allocations abort rather than SEGV; texture/pixbuf results are NULL-checked — memory growth is a degradation contributor, not a proven crash path.

**Remaining unaudited dependency:** `GnostrVideoPlayer`/GStreamer internals (bus callbacks, teardown) — flagged as the leading unreviewed subsystem.

## Root Cause
**Unresolved — deliberately.** No ownership or async-lifetime defect reachable during ordinary scrolling was found on current master, and promoting a dormant defect to "root cause" would be dishonest. The confirmed reachable problems — leaks in `fire_callbacks()` (`nostr_profile_service.c:198-227`), `check_ndb_cache()` (`:173-184`), the deliberate label-quarantine leaks (`og-preview-widget.c:324-353`, `gnostr-note-embed.c:139-150`), and unbounded detached disk/OG/sweep GTasks (`gnostr-media-service.c: schedule_disk_write/schedule_og_write/ensure_namespace_sweep`) plus uncancellable hydration (`gnostr-timeline-hydrator.c: hydrate_batch_thread`) — produce exactly the *conditions* (memory pressure, worker-pool flooding, widened race windows) under which a latent defect elsewhere (older binary, GStreamer/GTK dependency, or the unaudited video-player teardown) would surface after extended runtime. The historical ASan crash site (`g_task_thread_pool_thread`) is a symptom location, not a cause.

## Recommendations

### Fix now (reachable)
1. **`fire_callbacks()` leak** — `nostr-gobject/src/nostr_profile_service.c:198`: create `to_fire` with `g_ptr_array_new_with_free_func((GDestroyNotify)pending_callback_free)` (or steal the request's callback array); fix monotonically-growing `stats.pending_callbacks`.
2. **`check_ndb_cache()` JSON leak** — `:173-184`: free `json` on all branches (`g_autofree`), incl. the invalid-length branch.
3. **Label quarantine leaks** — `og-preview-widget.c:324-333`, `gnostr-note-embed.c:139-150`: clear label text during prepare_for_unbind while rooted, cancel async writers, then dispose normally; only fall back to a *bounded, counted* quarantine if the historical Pango crash reproduces. Also sanitize OG/profile strings before `gtk_label_set_text()` and make `truncate_content()` UTF-8-aware.
4. **Bound detached disk work** — `gnostr-media-service.c`: dedicated bounded disk executor (small worker count, job+byte ceilings, coalesced writes/sweeps, drop-superseded policy, stats).
5. **Hydrator cancellation** — `gnostr-timeline-hydrator.c: hydrate_batch_thread`: honor the cancellable between entries; snapshot `generation` into task data (no unsynchronized worker reads); revalidate before publishing.
6. **Profile batch replacement** — `nostr_profile_service.c:568-590`: queue/append batches instead of replacing `fetch_batches` while a fetch sequence is active (in-flight requests currently stranded forever).

### Harden next (dormant API landmines — all currently zero-caller)
7. Profile GTask API: deep-copy meta into the task with a destroy notify; error-complete invalid/cancelled requests (`profile_request_gtask_bridge_cb`).
8. Profile-service shutdown: ref-count the service; `BatchFetchCtx` holds a strong ref; never free tables/mutex with queries in flight.
9. `set_pool()`: ref new before unref old.
10. Media callback delivery: ref-count `PendingRequest`; hold a ref across every callback loop (reentrancy vs `evict_account`).
11. `gnostr_get_shared_soup_session()`: return a strong ref (shutdown race window).
12. `gnostr_note_embed_set_cancellable()`: take a ref, clear in dispose.
13. Injected media-loader: ref-counted interface instead of borrowed pointer + function pointer.
14. OG `set_url_with_cancellable()` same-URL path: connect the new parent cancellable before early return.

### Efficiency (do with the above)
15. Stop double-parsing content (hydrate_entry + `recompute_derived_fields`); reuse derived geometry for profile-only VM copies.
16. Cache the `GSettings` object in `gnostr_is_remote_media_allowed()` (currently constructed per call).
17. Remove or byte-account the row's fallback 50-texture LRU; include GtkPicture-held bytes in telemetry; revisit the 500 MiB inline decoded default.
18. Maintain disk-cache accounting instead of full scan+sort per write.

## Preventive Measures
- **Crash attributability**: ship symbols/build IDs; capture all-thread symbolicated backtraces (frames below `g_task_thread_pool_thread` are the ones that matter). On macOS, keep app crashes out of terminal-suppressed paths so `.ips` reports are written.
- **Runtime telemetry** (periodic + attached to crash context): RSS; decoded bytes by class; pending/queued/active downloads, disk jobs + retained bytes + oldest-job age; hydrator started/cancelled/stale; profile pending requests/batches; embed pending subscribers; label-quarantine activations; row bind/unbind + stale-callback drops. Hash URLs/ids in logs.
- **Standing stress rig**: scripted long-scroll under ASan+UBSan, `G_SLICE=always-malloc`, `G_DEBUG=gc-friendly`, `MallocScribble` (macOS) / `MALLOC_PERTURB_` (Linux), with delayed callbacks, tiny budgets, cancellation at every async stage, shutdown with work in flight, synchronous `evict_account()` from a subscriber callback, and repeated video play/unbind cycles.
- **API convention**: no public API stores a borrowed GObject or returns borrowed heap results across async boundaries — `(transfer full)` + destroy notify by default; review new dedup/subscriber tables for ref-counted entries.
