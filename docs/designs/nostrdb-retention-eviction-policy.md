# nostrdb Retention & Eviction Policy — Design

**Issue:** `nostrc-8rxk` (design half; implementation remains open)
**Status:** Proposed — not implemented
**Date:** 2026-08-09
**Predecessor:** `docs/plans/gnostr-rich-media-timeline-2026-08-03.md` (finding #7: "nostrdb has no retention policy: over-budget only warns")

---

## 1. Context & Scope

gnostr uses nostrdb (LMDB) as its central protocol index. It grows without bound. The
`ndb-cache-max-mb` GSettings key exists and is checked, but exceeding it produces only a
log warning telling the user to delete the database by hand.

**In scope:** retention/eviction policy for the nostrdb LMDB store — what to keep, what to
drop, when, how, and how to observe it.

**Out of scope:** the rich-media disk/texture caches (already bounded by `GnostrMediaService`
per-account budgets), the legacy avatar cache (already LRU-pruned in `cache_prune.c`), and
the blossom-cache LMDB (`apps/blossom-cache/`, separate budget).

**Deliverable of this document:** a concrete policy, config surface, algorithm, phased
implementation plan with a suggested beads breakdown, and risks. No code is changed by this
document.

---

## 2. Current State

### 2.1 The warn-only path

`apps/gnostr/src/util/cache_prune.c:275-284`:

```c
gint64 ndb_size = gnostr_cache_get_ndb_size();
gint64 ndb_limit_bytes = (gint64)ndb_max_mb * 1024 * 1024;

if (ndb_max_mb > 0 && ndb_size > ndb_limit_bytes) {
  g_warning("cache_prune: nostrdb size (%.1f MB) exceeds limit (%d MB). "
            "Note: Automatic nostrdb pruning is not yet implemented. "
            "Consider deleting %s/data.mdb to reset the database.", ...);
}
```

The comment block immediately above (`cache_prune.c:268-274`) already sketches the intended
policy (age, kind priority, reference counting). This document makes it concrete.

### 2.2 Four defects in the existing check

These must be fixed regardless of which eviction strategy is chosen.

| # | Defect | Evidence |
|---|--------|----------|
| D1 | **The metric is wrong.** `gnostr_cache_get_ndb_size()` `g_stat()`s `data.mdb`. LMDB **never shrinks its file**; freed pages go on the freelist and are reused. After a successful eviction pass the file size is unchanged, so the warning would fire forever and any "prune until under budget" loop would never terminate. | `cache_prune.c:130-146` |
| D2 | **The budget equals the mapsize.** gnostr opens ndb with `mapsize = 1073741824` (1 GiB) and `ndb-cache-max-mb` defaults to `1024`. The DB hits `MDB_MAP_FULL` at (or just before) the point the warning fires. | `apps/gnostr/src/main_app.c:391`; `apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml:295-301` |
| D3 | **`MDB_MAP_FULL` is silently swallowed.** nostrdb never references `MDB_MAP_FULL`. A failed `mdb_txn_commit` logs via `ndb_debug` (`nostrdb.c:7382`) and per-index `mdb_put` failures are logged to stderr with the return value ignored by the writer loop (e.g. `nostrdb.c:7274`). The real production failure mode today is **silent event loss**, not a crash or a visible error. | `third_party/nostrdb/src/nostrdb.c` |
| D4 | **Ordering makes real pruning impossible.** `gnostr_cache_prune_init()` is called at `main_app.c:381`, *before* `storage_ndb_init()` at `main_app.c:394`. The ndb env is not open yet, so nothing beyond a file `stat` is possible from there. | `apps/gnostr/src/main_app.c:381,394` |

The gschema description is also actively misleading: it claims "When exceeded, oldest events
are pruned on startup" (`gschema.xml:297-301`), which has never been true.

### 2.3 Storage stack

```
apps/gnostr  ──►  storage_ndb_*        nostr-gobject/src/storage_ndb.c
             ──►  NostrStorage vtable  libnostr/include/nostr-storage.h:16
             ──►  ndb_backend.c        libnostr/src/store/nostrdb/ndb_backend.c
             ──►  nostrdb_storage.c    components/nostrdb/src/nostrdb_storage.c
             ──►  libnostrdb           third_party/nostrdb (git submodule)
```

The `NostrStorage` vtable already declares `delete_event`, but the nostrdb implementation
returns `-ENOTSUP` (`components/nostrdb/src/nostrdb_storage.c:134-153`). There is no prune
or compact operation anywhere in the stack.

`third_party/nostrdb` is a **git submodule** of `https://github.com/damus-io/nostrdb.git`,
currently at `2d459e8`, 7 commits behind `origin/master`, with uncommitted local edits to
`ndb.c` and `test.c`. Anything added to nostrdb is a fork carry + upstream PR.

---

## 3. Constraints

### C1 — nostrdb has no delete API at all

`grep -i 'delete\|purge\|prune\|evict\|expire' src/nostrdb.h` yields exactly one hit:
`NDB_NOTE_FLAG_DELETED` (`nostrdb.h:17`), which is **defined but never used** anywhere in
`src/`. Nothing can be removed today.

### C2 — Single writer, no public write transaction

The only public transaction entry point is `ndb_begin_query()` (`nostrdb.h:622`), which is
read-only. Every write flows through one internal writer thread fed by a `prot_queue` of
`enum ndb_writer_msgtype` messages (`nostrdb.c:189-197`, dispatched at `nostrdb.c:7213`).

**Consequence:** eviction must be a new writer message type, not a caller-held write txn.
This preserves the single-writer invariant for free.

### C3 — Deleting one note means deleting up to 11 index entries

`ndb_write_note()` (`nostrdb.c:6460-6543`) fans a single note out into:

| DBI | Key layout | Flags | Written by |
|-----|-----------|-------|-----------|
| `NDB_DB_NOTE` | `note_key` u64 | `INTEGERKEY` | `ndb_write_note:6508` |
| `NDB_DB_NOTE_ID` | `ndb_tsid{ id[32], created_at u64 }` → `note_key` | `DUPSORT DUPFIXED` | `ndb_write_note_id_index:4115` |
| `NDB_DB_NOTE_KIND` | `ndb_u64_ts{ kind u64, created_at u64 }` → `note_key` | `DUPSORT INTEGERDUP DUPFIXED` | `ndb_write_note_kind_index:5498` |
| `NDB_DB_NOTE_PUBKEY` | `ndb_tsid{ pubkey[32], created_at u64 }` → `note_key` | `DUPSORT INTEGERDUP DUPFIXED` | `ndb_write_note_pubkey_index:1925` |
| `NDB_DB_NOTE_PUBKEY_KIND` | `ndb_id_u64_ts{ pubkey[32], kind u64, created_at u64 }` → `note_key` | `DUPSORT INTEGERDUP DUPFIXED` | `ndb_write_note_pubkey_kind_index:1949` |
| `NDB_DB_NOTE_TAGS` | `tag_char u8 + tag_val + len u8 + created_at u64` → `note_key` | `DUPSORT DUPFIXED` | `ndb_write_note_tag_index:5443` |
| `NDB_DB_NOTE_TEXT` | varint-packed `note_key + strlen + word + timestamp + word_index`, **one entry per word** | `DUPSORT` | `ndb_write_note_fulltext_index:5627` |
| `NDB_DB_NOTE_BLOCKS` | `note_key` u64 | `INTEGERKEY` | `ndb_write_new_blocks` |
| `NDB_DB_NOTE_RELAY_KIND` | `note_key u64 + kind u64 + created_at u64 + relay_len u8 + relay cstr` | — | `ndb_write_note_relay_kind_index:1876` |
| `NDB_DB_NOTE_RELAYS` | `note_key` u64 → relay strings | `DUPSORT` | `ndb_write_note_relay:1754` |
| `NDB_DB_META` | `id[32]` → counters | — | `ndb_process_note_stats` / `ndb_write_reaction_stats` |

Every index key except `NOTE_BLOCKS`, `NOTE_RELAY_KIND` and `NOTE_RELAYS` is derived purely
from the note body, which is still available (in `NDB_DB_NOTE`) at delete time. So **delete
= read the note, re-derive every index key with the same builders the writer used, `mdb_del`
each, then delete the note.**

The fulltext index is the hard case: its keys are sorted by *word*, not by note, so they
cannot be found by scanning; they must be regenerated by re-tokenizing the content exactly
as `ndb_write_note_fulltext_index` did.

**Hard requirement:** write and delete must share one key-construction implementation.
Two parallel implementations *will* drift, and drift means orphaned index entries pointing
at freed (and possibly reused) `note_key`s — i.e. the timeline renders the wrong note.

### C4 — `note_key` is not a persistent counter (blocking hazard)

```c
note_key = note->overwrite_note_id
        ? note->overwrite_note_id
        : ndb_get_last_key(txn->mdb_txn, note_db) + 1;   // nostrdb.c:6499-6501
```

`ndb_get_last_key()` (`nostrdb.c:3649-3667`) is literally `MDB_LAST` on the note DBI.
**Delete the highest-keyed notes and the next ingest reuses those keys.** `profile_key` has
the same problem (`nostrdb.c:3898`).

This is not theoretical for gnostr: `note_key` values are retained in app memory across time
in `GnNostrEventItem` (`apps/gnostr/src/model/gn-nostr-event-item.c:13`) and in
`GnNostrEventModel`'s `note_key_set`, `insertion_key_set` and `cache_lru`
(`apps/gnostr/src/model/gn-nostr-event-model.c:133,175,375`). A reused key silently
resolves a stale handle to a *different* event.

**Fixing key allocation is a prerequisite, not a nice-to-have.**

### C5 — Deletes never shrink the file

LMDB returns freed pages to a freelist for reuse within the same env. `data.mdb` only shrinks
via a compacting copy. nostrdb already exposes the primitive:

```c
int ndb_snapshot(struct ndb *ndb, const char *path, unsigned int flags);  // nostrdb.h:596
{ return mdb_env_copy2(ndb->lmdb.env, path, flags); }                     // nostrdb.c:7975
```

Passing `MDB_CP_COMPACT` writes a defragmented copy. That is our reclaim path (§6.6).

### C6 — `ndb_stat()` is a full scan

`ndb_stat()` opens a cursor on every DBI and walks every entry, accumulating
`{count, key_size, value_size}` (`struct ndb_stat_counts`, `nostrdb.h:382-393`). It is
accurate but O(entire database) — fine for a CLI (`ndb stat`), unusable for a periodic
budget probe. We need a cheap metric instead (§6.1).

### C7 — Long-lived read transactions inflate the DB

LMDB cannot reuse a page freed while an older reader still holds a snapshot. If gnostr holds
`ndb_begin_query()` txns across frames or across async work, eviction will free pages that
cannot be recycled, and the file will keep growing anyway. **Auditing reader-txn lifetimes is
part of this work**, not a separate concern.

### C8 — There is already a repair path

`ndb_rebuild_note_indices()` (`nostrdb.c:1999`) `mdb_drop`s the secondary indices and rebuilds
them from `NDB_DB_NOTE`. This is our escape hatch if eviction ever corrupts an index, and the
oracle for the eviction test suite (§8, WI-7).

---

## 4. Data Classes & Value Tiers

nostrdb is a **cache of the network** for most content and a **system of record** for a
small, precious subset. The policy must distinguish them.

### 4.1 Never evict (the pin set)

| Class | Rationale |
|-------|-----------|
| Any note authored by a locally-known account pubkey | The user's own posts; may exist nowhere else |
| DM / wrapped kinds: 4, 1059, 1060, and PNS rumors (kind 1080, see `giftwrap.c` / `nip-pns`) | Relays expire these; the local copy is often the only copy |
| Kind 5 deletion events | Tiny; needed so re-sync does not resurrect deleted content |
| Kind 0 profiles for own accounts + follows | Small, constantly needed, expensive to refetch |
| Kind 3 contact lists for own accounts | Identity-critical |
| Replaceable/addressable list kinds (10000–19999, 30000–39999) authored by own accounts — mutes, relay lists, bookmarks, NIP-51 lists | One per author, small, identity-critical |
| Any event id in the bookmarks set | Explicit user intent |
| Any note referenced by an `e` tag of a pinned note (one hop) | Keeps bookmarked threads readable |
| Anything with `note_key > max_note_key - keep_recent_keys` | Tail protection against `note_key` reuse (C4) |
| Anything ingested more recently than `min-age-days` | Protects the live timeline |

**The pin set must be resolved from nostrdb itself**, by querying kinds 3 / 10000 / 10003 /
30000-series authored by the known account pubkeys — *not* from the GObject UI models. The
eviction pass may run before the UI has loaded them, and must work headless. The UI accessors
are usable as an optional fast path / cross-check:

- own pubkey — `gnostr_signer_service_get_pubkey()` (`apps/gnostr/src/ipc/gnostr-signer-service.h:187`)
- follows — `gn_follow_list_model_load_for_pubkey()` (`apps/gnostr/src/model/gn-follow-list-model.h:47`)
- bookmarks — `gnostr_bookmarks_get_event_ids()` (`apps/gnostr/src/util/bookmarks.h:214`)
- mutes — `gnostr_mute_list_get_pubkeys()` (`nostr-gobject/include/nostr-gobject-1.0/gnostr-mute-list.h:301`)

### 4.2 Evictable classes, cheapest first

| Tier | Class | Information loss |
|------|-------|------------------|
| 0 | Ephemeral kinds 20000–29999, if any were ever persisted | None — should never have been stored |
| 1 | `NDB_DB_NOTE_BLOCKS` for cold notes | **None.** Parsed render blocks are a pure derived cache, regenerated on demand from note content. Independently deletable — it is keyed by bare `note_key`, no fan-out. |
| 2 | `NDB_DB_NOTE_TEXT` fulltext entries for old notes | Search recall on old notes only |
| 3 | Cold low-value notes: kinds 7 (reactions), 6/16 (reposts), 9735 (zap receipts) from non-pinned authors | Aggregate counts survive in `NDB_DB_META` |
| 4 | Cold general notes: kinds 1, 1111, 30023 etc. from non-pinned authors, older than the note TTL | Refetchable from relays (best effort) |
| 5 | **Pressure mode** — ignore per-kind TTLs; evict globally oldest non-pinned notes until the low watermark | Same as tier 4, applied more aggressively |
| 6 | **Compaction** — reclaim freed pages back to the filesystem | None |

Tier 1 deserves emphasis: dropping `NOTE_BLOCKS` for everything older than a week is
zero-information-loss, single-DBI, no index fan-out, and on a note-heavy DB it is a
meaningful fraction of the bytes. It is the safest possible first shipping increment.

---

## 5. Proposed Policy

### 5.1 Ordering key: ingest order, not `created_at`

Candidates are enumerated **ascending by `note_key`**, i.e. ingest order, with `created_at`
used only to evaluate per-kind TTL predicates.

Rationale:

1. `created_at` is attacker-controlled. Ordering evictions by it makes a future-dated note
   immortal and lets a hostile relay pin garbage in the cache forever.
2. `note_key` ascending is the natural `MDB_NEXT` cursor order on an `INTEGERKEY` DBI — the
   cheapest possible scan, sequential in page order.
3. For a cache, ingest recency is a better proxy for "will be needed again" than authored
   time.

`NDB_DB_NOTE_KIND` (`{kind, created_at}` DUPSORT) remains available for targeted per-kind
TTL sweeps where a full scan is not warranted.

### 5.2 Watermarks and hysteresis

- Start a pass when `used_bytes > budget * high_watermark_pct` (default 90%).
- Stop when `used_bytes <= budget * low_watermark_pct` (default 75%).

Hysteresis prevents evict-on-every-tick thrash and, more importantly, prevents the
pathological "evict one note per pass forever" loop.

### 5.3 Budget must be well below mapsize

Fix D2: raise gnostr's mapsize to 8 GiB (matching `ndb_backend.c:261`) and let
`ndb-cache-max-mb` be the enforced ceiling. LMDB's mapsize is a virtual address reservation;
the file grows on demand, so a generous mapsize costs address space, not disk. Enforce
`mapsize >= budget * 2` at open time and log loudly if the configuration violates it.

### 5.4 Cadence

| Trigger | Behaviour |
|---------|-----------|
| **Startup** | After `storage_ndb_init()` (fixing D4), take one cheap usage reading. Log it. If over the high watermark, arm a deferred pass ~30 s after the UI settles. Never block startup. |
| **Periodic** | `g_timeout_add_seconds(ndb-retention-interval-mins * 60)` → cheap usage probe. Run a pass only if over the high watermark, or once per day for the TTL sweep. |
| **On-write pressure** | The nostrdb writer thread checks `me_last_pgno` every N commits (it is already in the env, so this is free) and raises a pressure signal above 95% of mapsize. Defends against a burst filling the DB between periodic ticks. |
| **Manual** | A debug/CLI entry point and (later) a Settings button. |
| **Never** | On the UI thread. The pass runs in a `GTask` worker that enqueues batches; the actual mutation happens on nostrdb's writer thread regardless. |

---

## 6. Algorithm

### 6.1 Measurement (cheap)

Replace the `data.mdb` file `stat` with LMDB page accounting. New nostrdb API:

```c
struct ndb_usage {
    size_t   page_size;        /* MDB_envinfo / MDB_stat ms_psize          */
    size_t   map_size;         /* me_mapsize                                */
    uint64_t hwm_pages;        /* me_last_pgno + 1  — allocation high-water */
    uint64_t live_pages;       /* sum over DBIs of branch+leaf+overflow     */
    struct ndb_stat_counts per_db[NDB_DBS];  /* entry counts only, O(1)     */
};
int ndb_usage(struct ndb *ndb, struct ndb_usage *out);
```

Implementation: one read txn, `mdb_env_info()` + `mdb_env_stat()` + `mdb_stat()` per DBI.
`mdb_stat` reads the DB's root page header — O(1) per DBI, ~16 DBIs. Microseconds, safe to
call on every tick.

Two derived quantities, and both matter:

- **`used_bytes = hwm_pages * page_size`** — what actually consumes disk and exhausts the
  mapsize. **This is the budget metric.** It does not decrease on delete.
- **`live_bytes = live_pages * page_size`** — real live data. `used_bytes - live_bytes` is
  reclaimable-by-compaction.

Eviction targets `live_bytes` (that is what it can move). Compaction converts a
`live_bytes` reduction into a `used_bytes` reduction. Reporting both is what makes
"pruning ran but the file didn't shrink" explicable instead of a bug report.

Termination for a pass therefore uses `live_bytes` against the watermarks; `used_bytes`
drives the decision to compact.

### 6.2 Building the pin set

```
accounts   := local account pubkeys (signer service / account store)
pinned_pk  := accounts
if protect_follows:
    for a in accounts: pinned_pk |= p-tags of newest kind-3 by a   (ndb_query)
pinned_ids := e-tags of newest kind-10003 (bookmarks) by each account
           |  e-tags of newest kind-30001/30003 sets by each account
pinned_ids |= e-tags of any note already in pinned_ids   (one hop, bounded)
max_key    := ndb_get_last_key(NOTE)
floor_key  := max_key - keep_recent_keys
floor_time := now - min_age_days
```

Represent `pinned_pk` and `pinned_ids` as hash sets of 32-byte keys. Sizing: a 5,000-follow
account is 160 KB — trivial. Cap `pinned_ids` at a configured maximum and, on overflow,
degrade to "do not evict any note with an `e` tag" (fail-safe direction).

### 6.3 Candidate selection

```
open read txn
cursor over NDB_DB_NOTE, MDB_FIRST then MDB_NEXT      # ascending note_key
for (note_key, note) in cursor:
    if note_key >= floor_key:            break        # tail protection (C4)
    if evicted_this_pass >= max_per_pass: break
    if live_bytes_estimate <= low_mark:   break

    if note.pubkey in pinned_pk:                      continue
    if note.id     in pinned_ids:                     continue
    if note.kind in PINNED_KINDS:                     continue   # 4,5,1059,1060,1080,...
    if note.created_at > floor_time:                  continue

    tier := classify(note.kind)
    if note.created_at > now - ttl_for(tier):         continue

    emit(note_key, tier)
close read txn                                         # ← do not hold across the deletes (C7)
```

The read txn is closed before mutation is enqueued, so freed pages are immediately
recyclable. The scan is restartable: record the last `note_key` examined and resume from
there on the next batch, rather than rescanning from the start.

Tiers 1 and 2 (blocks / fulltext only) run as separate, much cheaper passes that emit
`note_key`s for partial eviction rather than full note deletion.

### 6.4 The delete primitive (nostrdb fork)

```c
/* public */
struct ndb_evict_stats { uint64_t notes, blocks, index_entries, bytes_freed; };

int ndb_evict_notes (struct ndb *, const uint64_t *note_keys, size_t n, struct ndb_evict_stats *);
int ndb_evict_blocks(struct ndb *, const uint64_t *note_keys, size_t n);  /* tier 1 */
int ndb_evict_text  (struct ndb *, const uint64_t *note_keys, size_t n);  /* tier 2 */
uint64_t ndb_eviction_generation(struct ndb_txn *);
```

Dispatched via new `enum ndb_writer_msgtype` members `NDB_WRITER_EVICT_NOTES` /
`NDB_WRITER_EVICT_BLOCKS` / `NDB_WRITER_EVICT_TEXT`, handled in the writer-thread switch
(`nostrdb.c:7213`). This keeps the single-writer invariant (C2) with no new locking.

Internal, executed on the writer thread inside one write txn per batch:

```c
static int ndb_delete_note(struct ndb_txn *txn, uint64_t note_key)
{
    note = mdb_get(NOTE, note_key);
    if (!note) return 0;                       /* idempotent */

    /* exact mirror of ndb_write_note:6514-6533, in reverse */
    ndb_foreach_note_id_key       (note, note_key, del_cb, txn);
    ndb_foreach_note_kind_key     (note, note_key, del_cb, txn);
    ndb_foreach_note_tag_key      (note, note_key, del_cb, txn);
    ndb_foreach_note_pubkey_key   (note, note_key, del_cb, txn);
    ndb_foreach_note_pk_kind_key  (note, note_key, del_cb, txn);
    ndb_foreach_fulltext_key      (note, note_key, del_cb, txn);   /* re-tokenize */
    ndb_delete_note_relay_indexes (txn, note_key);                 /* cursor NOTE_RELAYS dups */
    mdb_del(txn, NOTE_BLOCKS, note_key, NULL);
    mdb_del(txn, NOTE,        note_key, NULL);
    /* NDB_DB_META (id-keyed aggregate counters) is intentionally retained:
       tiny, and preserves counts if the note is later re-ingested. GC separately. */
    return 1;
}
```

`ndb_foreach_*_key` are extracted from the existing `ndb_write_note_*_index()` functions and
then **called by both write and delete**. This refactor (WI-4) lands *before* any deletion
code and is verified by byte-for-byte key-equality tests. It is the single most important
correctness control in this design.

`NDB_DB_NOTE_RELAY_KIND` keys embed `note_key` as their **leading** field
(`nostrdb.c:1876`, `838`), so its entries for a note form a contiguous cursor range — a
range scan, not a reconstruction.

### 6.5 Batching

- **500 notes per write txn** (~5,000 `mdb_del`s). Tunable.
- Yield ~50 ms between batches so the writer thread stays responsive to live ingest.
- Re-measure usage between batches; stop at the low watermark.
- Cap one pass at `ndb-retention-max-notes-per-pass` (default 20,000) even if still over
  budget — the next tick continues. Bounds worst-case latency and the crash window.
- Every batch is a complete LMDB transaction, so a crash mid-pass leaves a consistent DB
  with a partially-completed eviction. That is fine and self-healing: the next pass resumes.

### 6.6 Compaction (tier 6)

Deletion moves `live_bytes` but not `used_bytes` (C5). When
`used_bytes - live_bytes > max(ndb-compact-min-reclaim-mb, 25% of used_bytes)`, schedule
compaction. Compaction runs **offline** — at shutdown, or at next startup before
`storage_ndb_init()`:

```
1. statvfs(dbdir); require free >= live_bytes * 1.1, else skip and log
2. ndb_snapshot(ndb, "<dbdir>/data.mdb.compacting", MDB_CP_COMPACT)   # nostrdb.h:596
3. fsync the new file and the directory
4. write "<dbdir>/.compact-pending" marker naming both paths, fsync
5. close env
6. rename data.mdb -> data.mdb.old   (atomic)
7. rename data.mdb.compacting -> data.mdb   (atomic)
8. unlink data.mdb.old, unlink marker, fsync dir
```

Startup recovery inspects the marker and finishes or rolls back whichever rename pair is
half-done. `lock.mdb` is regenerated by LMDB and needs no special handling.

Compaction is opt-outable (`ndb-compact-enabled`) and rate-limited (at most once per
`ndb-compact-min-interval-days`, default 7) — it is O(live data) I/O.

---

## 7. Configuration

### 7.1 GSettings (`org.gnostr.Client`)

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `ndb-cache-max-mb` | i | 1024 | **Redefined**: ceiling on LMDB pages in use, not `data.mdb` file size. 0 = unlimited. Gschema description must be corrected. |
| `ndb-retention-enabled` | b | true | Master switch |
| `ndb-retention-high-watermark-pct` | i | 90 | Start eviction above this % of budget |
| `ndb-retention-low-watermark-pct` | i | 75 | Stop eviction at this % of budget |
| `ndb-retention-min-age-days` | i | 2 | Hard floor — nothing newer is ever evictable |
| `ndb-retention-note-ttl-days` | i | 90 | Tier 4 general notes. 0 = never by age |
| `ndb-retention-reaction-ttl-days` | i | 14 | Tier 3 reactions/reposts/zaps |
| `ndb-retention-fulltext-ttl-days` | i | 30 | Tier 2 fulltext index |
| `ndb-retention-blocks-ttl-days` | i | 7 | Tier 1 parsed blocks |
| `ndb-retention-interval-mins` | i | 60 | Periodic probe interval |
| `ndb-retention-max-notes-per-pass` | i | 20000 | Latency bound per pass |
| `ndb-retention-keep-recent-keys` | i | 10000 | Tail protection against `note_key` reuse |
| `ndb-retention-protect-follows` | b | true | Pin notes authored by follows |
| `ndb-compact-enabled` | b | true | Allow offline compaction |
| `ndb-compact-min-reclaim-mb` | i | 256 | Minimum reclaimable before compacting |
| `ndb-compact-min-interval-days` | i | 7 | Compaction rate limit |

Reuse the existing `cache-prune-on-startup` (`gschema.xml:314`) as the startup-check gate;
do not add a second startup switch.

### 7.2 Environment overrides (debug / CI)

- `GNOSTR_NDB_RETENTION=0|1` — force off/on regardless of GSettings
- `GNOSTR_NDB_RETENTION_DRY_RUN=1` — select and log candidates, delete nothing
- `GNOSTR_NDB_RETENTION_FORCE=1` — run a pass immediately at startup, ignoring watermarks
- `GRELAY_NDB_MAPSIZE_MB` — already honoured at `components/nostrdb/src/nostrdb_storage.c:62`

### 7.3 Layering decision

**Policy lives in gnostr; primitives live in nostrdb.** nostrdb gains only mechanism
(`ndb_usage`, `ndb_evict_*`, the key-builder refactor, the persistent key counter) — all of
which is generically useful and upstreamable to `damus-io/nostrdb`. Kind tiers, TTLs, pin
sets and watermarks stay in `apps/gnostr/src/util/ndb_retention.c`. No policy is added to
`opts_json`, so `ln_store` consumers are unaffected.

---

## 8. Phased Implementation Plan

Suggested beads breakdown. All children of `nostrc-8rxk`; IDs assigned at creation time.

### Phase 0 — Tell the truth (no deletion) — *ship first, independently valuable*

| WI | Title | Notes |
|----|-------|-------|
| WI-1 | `ndb: add ndb_usage() page-accounting API` | §6.1. `mdb_env_info` + per-DBI `mdb_stat`. Upstreamable standalone. |
| WI-2 | `gnostr: fix ndb budget metric, ordering, and mapsize` | Fixes D1/D2/D4: replace file-`stat` with `ndb_usage`, split the ndb check out of `gnostr_cache_prune_init()` to run after `storage_ndb_init()`, raise mapsize to 8 GiB, correct the misleading gschema description. |
| WI-3 | `gnostr: report ndb live/hwm/mapsize + per-kind counts` | Extend `gnostr_cache_stats_string()` (`cache_prune.c:204`); debug CLI. Makes the problem measurable before it is solved. |

### Phase 1 — Delete primitive in nostrdb (fork + upstream PR)

| WI | Title | Notes |
|----|-------|-------|
| WI-4 | `ndb: extract shared index key builders from write path` | §6.4. Pure refactor of `ndb_write_note_*_index()` into `ndb_foreach_*_key()`. Tests assert emitted key bytes are unchanged. **Blocks WI-6.** |
| WI-5 | `ndb: persistent monotonic note_key/profile_key allocator` | C4. New `NDB_META_KEY_NEXT_NOTE_KEY` in `NDB_DB_NDB_META` (`nostrdb.c:202`), seeded from `MDB_LAST` on migration. **Blocks WI-6.** Fixes a latent upstream bug independent of retention. |
| WI-6 | `ndb: implement ndb_evict_notes/blocks/text` | §6.4, §6.5. New writer msgtypes, batched write txns, `ndb_evict_stats`. Depends on WI-4, WI-5. |
| WI-7 | `ndb: eviction correctness test suite` | Ingest N → evict M → assert: no orphan entries in any of the 11 DBIs; every query returns only live notes; `ndb_rebuild_note_indices()` (C8) yields a byte-identical index set. Fuzz with random kinds/tags/unicode content. Depends on WI-6. |

### Phase 2 — Policy engine in gnostr

| WI | Title | Notes |
|----|-------|-------|
| WI-8 | `gnostr: retention GSettings keys + config loader` | §7.1. Schema + `ndb_retention_config_load()`. |
| WI-9 | `gnostr: pin-set builder from nostrdb (headless-safe)` | §6.2. Query kinds 3/10000/10003/30000-series directly; UI accessors as fast path only. |
| WI-10 | `gnostr: tiered eviction pass (dry-run first)` | §6.3. New `apps/gnostr/src/util/ndb_retention.c`. Ship with dry-run default for one release. Depends on WI-6, WI-8, WI-9. |
| WI-11 | `gnostr: eviction cadence wiring` | §5.4. Deferred startup check + periodic timer + `GTask` offload. Depends on WI-10. |
| WI-12 | `gnostr: audit ndb read-txn lifetimes` | C7. Verify no `ndb_begin_query` txn is held across frames or async boundaries; without this, eviction frees pages that cannot be reused. Independently valuable. |

### Phase 3 — Reclaim & safety

| WI | Title | Notes |
|----|-------|-------|
| WI-13 | `gnostr: offline LMDB compaction with crash-safe swap` | §6.6. `ndb_snapshot(..., MDB_CP_COMPACT)`, marker file, startup recovery, disk precheck. |
| WI-14 | `ndb+gnostr: eviction generation & app-side note_key invalidation` | C4 belt-and-braces. Generation counter in `NDB_DB_NDB_META`; `GnNostrEventModel` drops `note_key_set`/`insertion_key_set`/`cache_lru` on change; all `ndb_get_note_by_key` callers must tolerate NULL. |
| WI-15 | `ndb: surface MDB_MAP_FULL and write failures to callers` | D3. Independent pre-existing defect — writes are silently dropped today. Should be filed and fixed regardless of retention. |

### Phase 4 — Polish

| WI | Title | Notes |
|----|-------|-------|
| WI-16 | `gnostr: retention settings UI` | Fold into the existing settings-UI issue `nostrc-uexn`. |
| WI-17 | `docs: update STORAGE.md / MAINTENANCE.md; upstream nostrdb PR` | Upstream WI-1/4/5/6 to `damus-io/nostrdb` to limit fork divergence. |
| WI-18 | `ndb: ingest-time filtering for never-wanted kinds` | Complementary and cheap — `ndb_config_set_ingest_filter()` (`nostrdb.h:575`) already exists. Drop ephemeral kinds 20000–29999 and reactions from muted pubkeys at the door. Reduces how often eviction must run. |

**Suggested shipping order:** Phase 0 alone converts a misleading warning into an accurate
one and fixes the mapsize footgun. Phase 1 WI-5 fixes a real latent bug. Tier-1 blocks-only
eviction (a subset of WI-10) is the safest first *deletion* to ship, since it is
zero-information-loss and touches one DBI.

---

## 9. Risks

| # | Risk | Severity | Mitigation |
|---|------|----------|-----------|
| R1 | **Index/note drift** — a delete path that misses an index entry leaves an orphan pointing at a freed `note_key`; queries return garbage or crash | Critical | Shared key builders (WI-4) as a hard architectural rule; orphan-detection test suite (WI-7); `ndb_rebuild_note_indices()` as the repair path (C8); dry-run first release |
| R2 | **`note_key` reuse** renders the wrong note in the timeline | Critical | Persistent counter (WI-5) *plus* tail protection (`keep-recent-keys`) *plus* eviction generation invalidation (WI-14) — three independent layers |
| R3 | **Evicting unrecoverable data** (DMs, own posts, content no relay still carries) | High | Pin set (§4.1) is deny-by-default for the precious kinds; conservative TTL defaults; `min-age-days` floor; dry-run telemetry before enabling by default |
| R4 | **"Pruning ran but the file didn't shrink"** bug reports | Medium | Report `live` and `used` as distinct numbers everywhere (§6.1); compaction phase (WI-13); explicit wording in the settings description |
| R5 | **Compaction data loss** on crash or full disk | High | Copy-then-atomic-rename, marker file, startup recovery, `statvfs` precheck, skip rather than risk (§6.6) |
| R6 | **Fork divergence** from `damus-io/nostrdb` (already 7 commits behind with local edits) | Medium | Keep eviction in a new `src/nostrdb_evict.c` with minimal edits to `nostrdb.c`; upstream PR early (WI-17); rebase before starting Phase 1 |
| R7 | **Writer-thread stalls** delaying live ingest during a pass | Medium | 500-note batches, inter-batch yield, per-pass cap, run only when actually over watermark |
| R8 | **Freelist never recycles** because a long-lived reader pins old snapshots (C7) | Medium | WI-12 audit; log the reader high-water mark so this is diagnosable |
| R9 | **`MDB_MAP_FULL` still silently drops writes** even with retention (D3) | High | WI-15, tracked as an independent defect; retention reduces frequency but does not fix the swallow |
| R10 | Fulltext delete must re-tokenize identically to the writer | High | `ndb_foreach_fulltext_key()` shared by both paths (WI-4); alternatively run gnostr with `NDB_FLAG_NO_FULLTEXT` if ndb fulltext search proves unused (see Open Questions) |
| R11 | Pin set unavailable (no account logged in, models not loaded) | Medium | Pin set is DB-derived, not UI-derived (§6.2); if no accounts are known, run tiers 0–3 only and skip tier 4/5 |

---

## 10. Alternatives Considered

**A. Nuke and rebuild** — delete `data.mdb` when over budget (what the current warning tells
users to do). Trivially simple and guarantees the bound. **Rejected:** destroys DMs, own
posts, and all offline content. Unacceptable data loss for a system-of-record subset.

**B. Copy-forward rebuild** — query the notes worth keeping, re-ingest them into a fresh env,
atomically swap. **Uses only existing APIs** (`ndb_query` + `ndb_process_event` +
`ndb_snapshot`) and compacts for free — no per-index delete code, so R1 evaporates.
**Rejected as the primary design** because it invalidates every `note_key` at once (worst
case for C4/R2), loses relay provenance (`NOTE_RELAYS`/`NOTE_RELAY_KIND`) and `NDB_DB_META`
counters unless separately replayed, needs 2× disk, and requires the DB offline for the
duration. **Retained as Plan B** if WI-4/WI-6 prove too risky to land safely — it is a
credible fallback, and worth prototyping if the fulltext delete path (R10) becomes a blocker.

**C. Time-partitioned environments** — roll a new LMDB env monthly, drop the oldest env
wholesale. Deletion becomes an `unlink`, which is the cleanest possible reclaim.
**Rejected:** nostrdb is single-env throughout; every query would need a cross-env merge
layer, and replaceable events would span partitions. Far too invasive for the benefit.

**D. Tombstone-only** — set the existing unused `NDB_NOTE_FLAG_DELETED` (`nostrdb.h:17`) and
filter at query time. **Rejected:** reclaims no space, which is the entire objective. Might
still be worth wiring for NIP-09 handling, but that is a separate concern from retention.

---

## 11. Open Questions (verify during Phase 0)

1. **Does gnostr actually use nostrdb's fulltext index?** If search goes through a different
   path, opening with `NDB_FLAG_NO_FULLTEXT` removes the largest index, the hardest delete
   path (R10), and a meaningful share of the bytes — in one line. Check before doing WI-4's
   fulltext work.
2. **What is the real per-DBI byte breakdown on a mature profile?** Run `ndb stat`
   (`third_party/nostrdb/ndb.c:274`) against a large user DB. If `NOTE_TEXT` or `NOTE_BLOCKS`
   dominates, tiers 1–2 may be sufficient on their own and Phase 1 can be deferred.
3. **Are `note_key`s ever persisted to disk** by gnostr, or only held in memory? The probe
   found in-memory retention only (`gn-nostr-event-item.c:13`, `gn-nostr-event-model.c:133,175,375`).
   If any are persisted, WI-14 grows considerably.
4. **What are the real reader-txn lifetimes** (C7/WI-12)? If long readers exist, they must be
   fixed before eviction can reclaim anything.
5. **Should `NDB_DB_META` aggregate counters be GC'd?** They are `id[32]`-keyed and survive
   note deletion by design here. Measure their size before deciding.
6. **Multi-account:** nostrdb is app-global by prior decision
   (`docs/plans/gnostr-rich-media-timeline-2026-08-03.md:73`). Confirm the pin set unions
   across all local accounts rather than only the active one.

---

## 12. Acceptance Criteria (for the implementation issue)

1. A DB grown past `ndb-cache-max-mb` returns to below the low watermark within one
   retention pass, and stays there under continued ingest.
2. No note in the pin set is ever removed — asserted by a test that pins, floods, evicts, and
   verifies every pinned id is still retrievable.
3. After eviction, every secondary index is consistent with `NDB_DB_NOTE`: no query returns a
   `note_key` that `ndb_get_note_by_key()` cannot resolve, and `ndb_rebuild_note_indices()`
   produces an identical index set.
4. `note_key`s are never reused after eviction.
5. A retention pass never blocks the UI thread for more than one frame, and never stalls live
   ingest for more than the configured inter-batch yield.
6. After compaction, `data.mdb` on disk is within 10% of `live_bytes`.
7. Killing the app mid-pass or mid-compaction leaves a DB that opens cleanly and passes
   criterion 3.
8. Every pass logs a structured summary: measured live/used/budget, tiers run, notes and
   index entries removed, bytes reclaimed, duration, and count skipped as pinned.
