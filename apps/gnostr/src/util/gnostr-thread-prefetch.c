/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * gnostr-thread-prefetch.c - Eager thread ancestor prefetching
 *
 * nostrc-4bk: Watches live kind:1/1111 events, extracts NIP-10 root/parent
 * references, checks nostrdb, and batches relay queries for missing ancestors.
 * Fetched events are fed back through the normal ingest queue so they appear
 * in nostrdb before the user opens the thread panel.
 */

#define G_LOG_DOMAIN "thread-prefetch"

#include "gnostr-thread-prefetch.h"

#include <nostr-gobject-1.0/gnostr-nip10-thread-manager.h>
#include <nostr-gobject-1.0/nostr_filter.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_event.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include "nostr-filter.h"
#include "utils.h"

#include <string.h>

/* ========== Policy constants ========== */

#define PREFETCH_MAX_BATCH_SIZE      20
#define PREFETCH_DEBOUNCE_MS         250
#define PREFETCH_MAX_ANCESTOR_DEPTH  16
#define PREFETCH_RETRY_COOLDOWN_US   (30LL * G_USEC_PER_SEC)

/* ========== Internal types ========== */

typedef struct {
    char      *event_id;        /* 64-char hex, owned */
    guint      remaining_depth; /* how many more ancestors to follow */
    GPtrArray *relay_hints;     /* (element-type utf8) owned strings, deduped */
} PrefetchTarget;

typedef struct {
    GPtrArray  *ids;            /* (element-type utf8) event IDs in this batch */
    GHashTable *depth_by_id;    /* char* id -> GUINT_TO_POINTER(depth) */
    GnostrThreadPrefetch *owner; /* strong ref via refcount */
} PrefetchBatch;

/**
 * Ancestor follow-up work item, collected during batch iteration
 * and processed after iteration completes (to avoid mutating the
 * queued_targets hash table during GHashTableIter walk).
 */
typedef struct {
    char *event_json;    /* owned */
    guint remaining_depth;
} AncestorFollowUp;

struct _GnostrThreadPrefetch {
    volatile gint ref_count;              /* atomic refcount for safe async lifecycle */
    GMainContext *owner_context;          /* ref'd, all mutable state lives here */

    GHashTable   *queued_targets;         /* char* id -> PrefetchTarget* */
    GHashTable   *inflight_ids;           /* char* id -> TRUE */
    GHashTable   *resolved_ids;           /* char* id -> TRUE (already local or ingested) */
    GHashTable   *recent_misses_us;       /* char* id -> GSIZE_TO_POINTER(µs timestamp) */

    guint         batch_source_id;        /* debounce timer, 0 if none */
    GCancellable *cancellable;            /* current relay query */
    gboolean      query_in_flight;        /* TRUE while relay query active */

    GnostrThreadPrefetchIngestFunc ingest_func;
    gpointer                       ingest_user_data;

    gboolean disposed;
};

/* ========== Refcount management ========== */

static GnostrThreadPrefetch *
prefetch_ref(GnostrThreadPrefetch *self)
{
    if (self)
        g_atomic_int_inc(&self->ref_count);
    return self;
}

static void
prefetch_unref(GnostrThreadPrefetch *self)
{
    if (!self) return;
    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    /* Final release: free all resources */
    g_clear_pointer(&self->queued_targets, g_hash_table_unref);
    g_clear_pointer(&self->inflight_ids, g_hash_table_unref);
    g_clear_pointer(&self->resolved_ids, g_hash_table_unref);
    g_clear_pointer(&self->recent_misses_us, g_hash_table_unref);
    g_clear_pointer(&self->owner_context, g_main_context_unref);
    g_clear_object(&self->cancellable);

    g_debug("[PREFETCH] Thread prefetch service destroyed (final unref)");
    g_free(self);
}

/* ========== PrefetchTarget helpers ========== */

static void
prefetch_target_free(PrefetchTarget *t)
{
    if (!t) return;
    g_free(t->event_id);
    if (t->relay_hints)
        g_ptr_array_unref(t->relay_hints);
    g_free(t);
}

static PrefetchTarget *
prefetch_target_new(const char *event_id, guint depth, const char *relay_hint)
{
    PrefetchTarget *t = g_new0(PrefetchTarget, 1);
    t->event_id = g_strdup(event_id);
    t->remaining_depth = depth;
    t->relay_hints = g_ptr_array_new_with_free_func(g_free);
    if (relay_hint && *relay_hint)
        g_ptr_array_add(t->relay_hints, g_strdup(relay_hint));
    return t;
}

static void
prefetch_target_add_hint(PrefetchTarget *t, const char *hint)
{
    if (!t || !hint || !*hint) return;
    for (guint i = 0; i < t->relay_hints->len; i++) {
        if (strcmp(g_ptr_array_index(t->relay_hints, i), hint) == 0)
            return; /* already present */
    }
    g_ptr_array_add(t->relay_hints, g_strdup(hint));
}

/* ========== PrefetchBatch helpers ========== */

static PrefetchBatch *
prefetch_batch_new(GnostrThreadPrefetch *owner)
{
    PrefetchBatch *b = g_new0(PrefetchBatch, 1);
    b->ids = g_ptr_array_new_with_free_func(g_free);
    b->depth_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    b->owner = prefetch_ref(owner); /* strong ref */
    return b;
}

static void
prefetch_batch_free(PrefetchBatch *b)
{
    if (!b) return;
    g_ptr_array_unref(b->ids);
    g_hash_table_unref(b->depth_by_id);
    prefetch_unref(b->owner);
    g_free(b);
}

/* ========== Forward declarations ========== */

static void schedule_batch_dispatch(GnostrThreadPrefetch *self);
static void dispatch_relay_query(GnostrThreadPrefetch *self, PrefetchBatch *batch,
                                 GPtrArray *relay_urls);
static void enqueue_target(GnostrThreadPrefetch *self, const char *event_id,
                           guint depth, const char *relay_hint);
static void process_fetched_ancestor(GnostrThreadPrefetch *self,
                                     const char *event_json, guint remaining_depth);

/* ========== ID validation ========== */

static gboolean
is_valid_hex64(const char *s)
{
    if (!s) return FALSE;
    size_t len = strlen(s);
    if (len != 64) return FALSE;
    for (size_t i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return FALSE;
    }
    return TRUE;
}

/* ========== nostrdb local check ========== */

/**
 * Load event JSON from nostrdb by ID if it exists.
 * Returns owned JSON string or NULL if not found.
 * Combines existence check and load into one query.
 */
static char *
load_event_json_from_ndb(const char *event_id_hex)
{
    if (!event_id_hex) return NULL;

    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn)
        return NULL;

    char filter_json[256];
    snprintf(filter_json, sizeof(filter_json),
             "[{\"ids\":[\"%s\"],\"limit\":1}]", event_id_hex);

    char **results = NULL;
    int count = 0;
    char *json = NULL;

    if (storage_ndb_query(txn, filter_json, &results, &count, NULL) == 0 &&
        count > 0 && results && results[0]) {
        json = g_strdup(results[0]);
    }

    if (results)
        storage_ndb_free_results(results, count);

    storage_ndb_end_query(txn);
    return json;
}

/* ========== Extract event ID from JSON ========== */

/**
 * Extract the event ID from a JSON event string using GNostrEvent.
 * Returns an owned 64-char hex string, or NULL on failure.
 */
static char *
extract_event_id_from_json(const char *event_json)
{
    if (!event_json) return NULL;

    GNostrEvent *ev = gnostr_event_new_from_json(event_json, NULL);
    if (!ev) return NULL;

    char *result = NULL;
    const char *id = gnostr_event_get_id(ev);
    if (id && strlen(id) == 64)
        result = g_strdup(id);

    g_object_unref(ev);
    return result;
}

/* ========== Source management helpers ========== */

/**
 * Attach a timeout source to the owner context (not the default context).
 * This ensures the callback runs on the correct thread.
 */
static guint
attach_timeout_to_owner(GnostrThreadPrefetch *self, guint interval_ms,
                        GSourceFunc callback, gpointer user_data)
{
    GSource *source = g_timeout_source_new(interval_ms);
    g_source_set_callback(source, callback, user_data, NULL);
    guint id = g_source_attach(source, self->owner_context);
    g_source_unref(source);
    return id;
}

/**
 * Attach an idle source to the owner context.
 */
static guint
attach_idle_to_owner(GnostrThreadPrefetch *self, GSourceFunc callback,
                     gpointer user_data)
{
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, callback, user_data, NULL);
    guint id = g_source_attach(source, self->owner_context);
    g_source_unref(source);
    return id;
}

/* ========== Debounce timer callback ========== */

static gboolean
on_batch_timer(gpointer user_data)
{
    GnostrThreadPrefetch *self = user_data;
    self->batch_source_id = 0;

    if (self->disposed)
        return G_SOURCE_REMOVE;

    /* Serialize: only one relay query at a time. If a query is in flight,
     * the completion handler will re-schedule us. */
    if (self->query_in_flight)
        return G_SOURCE_REMOVE;

    if (g_hash_table_size(self->queued_targets) == 0)
        return G_SOURCE_REMOVE;

    /* Collect up to PREFETCH_MAX_BATCH_SIZE targets that are not inflight,
     * not resolved, and not inside retry cooldown. */
    PrefetchBatch *batch = prefetch_batch_new(self);
    GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);

    /* Start with configured read relays */
    gnostr_get_read_relay_urls_into(relay_urls);

    gint64 now_us = g_get_monotonic_time();

    GHashTableIter iter;
    gpointer key, value;
    GPtrArray *ids_to_remove = g_ptr_array_new();

    /* Collect local-hit follow-up work separately to avoid mutating
     * queued_targets during iteration (P0 fix). */
    GPtrArray *follow_ups = g_ptr_array_new(); /* (element-type AncestorFollowUp*) */

    g_hash_table_iter_init(&iter, self->queued_targets);
    while (g_hash_table_iter_next(&iter, &key, &value) &&
           batch->ids->len < PREFETCH_MAX_BATCH_SIZE) {
        PrefetchTarget *target = value;
        const char *id = target->event_id;

        /* Skip already resolved */
        if (g_hash_table_contains(self->resolved_ids, id)) {
            g_ptr_array_add(ids_to_remove, (gpointer)id);
            continue;
        }

        /* Skip inflight */
        if (g_hash_table_contains(self->inflight_ids, id))
            continue;

        /* Skip if in retry cooldown */
        gpointer miss_ptr = g_hash_table_lookup(self->recent_misses_us, id);
        if (miss_ptr) {
            gint64 last_attempt = (gint64)GPOINTER_TO_SIZE(miss_ptr);
            if ((now_us - last_attempt) < PREFETCH_RETRY_COOLDOWN_US)
                continue;
        }

        /* Check nostrdb first (single query combines existence check + load) */
        g_autofree char *local_json = load_event_json_from_ndb(id);
        if (local_json) {
            g_hash_table_insert(self->resolved_ids, g_strdup(id), GINT_TO_POINTER(TRUE));
            g_ptr_array_add(ids_to_remove, (gpointer)id);

            /* Defer ancestor recursion until after iteration completes */
            if (target->remaining_depth > 0) {
                AncestorFollowUp *fu = g_new0(AncestorFollowUp, 1);
                fu->event_json = g_steal_pointer(&local_json);
                fu->remaining_depth = target->remaining_depth - 1;
                g_ptr_array_add(follow_ups, fu);
            }
            continue;
        }

        /* Add to relay batch */
        g_ptr_array_add(batch->ids, g_strdup(id));
        g_hash_table_insert(batch->depth_by_id, g_strdup(id),
                            GUINT_TO_POINTER(target->remaining_depth));
        g_hash_table_insert(self->inflight_ids, g_strdup(id), GINT_TO_POINTER(TRUE));

        /* Merge relay hints */
        for (guint h = 0; h < target->relay_hints->len; h++) {
            const char *hint = g_ptr_array_index(target->relay_hints, h);
            gboolean dup = FALSE;
            for (guint r = 0; r < relay_urls->len; r++) {
                if (strcmp(g_ptr_array_index(relay_urls, r), hint) == 0) {
                    dup = TRUE;
                    break;
                }
            }
            if (!dup)
                g_ptr_array_add(relay_urls, g_strdup(hint));
        }

        g_ptr_array_add(ids_to_remove, (gpointer)id);
    }

    /* Remove processed targets from queue (safe: iteration is complete) */
    for (guint i = 0; i < ids_to_remove->len; i++) {
        g_hash_table_remove(self->queued_targets, g_ptr_array_index(ids_to_remove, i));
    }
    g_ptr_array_unref(ids_to_remove);

    /* Now process deferred ancestor follow-ups (safe: no longer iterating) */
    for (guint i = 0; i < follow_ups->len; i++) {
        AncestorFollowUp *fu = g_ptr_array_index(follow_ups, i);
        process_fetched_ancestor(self, fu->event_json, fu->remaining_depth);
        g_free(fu->event_json);
        g_free(fu);
    }
    g_ptr_array_unref(follow_ups);

    if (batch->ids->len == 0) {
        /* Nothing to fetch remotely - check if more queued work remains */
        prefetch_batch_free(batch);
        g_ptr_array_unref(relay_urls);

        if (g_hash_table_size(self->queued_targets) > 0)
            schedule_batch_dispatch(self);
        return G_SOURCE_REMOVE;
    }

    g_debug("[PREFETCH] Dispatching relay query for %u ancestor IDs across %u relays",
            batch->ids->len, relay_urls->len);

    dispatch_relay_query(self, batch, relay_urls);
    g_ptr_array_unref(relay_urls);

    return G_SOURCE_REMOVE;
}

static void
schedule_batch_dispatch(GnostrThreadPrefetch *self)
{
    if (self->disposed) return;
    if (self->batch_source_id > 0) return; /* already scheduled */

    self->batch_source_id = attach_timeout_to_owner(self, PREFETCH_DEBOUNCE_MS,
                                                     on_batch_timer, self);
}

/* ========== Relay query completion ========== */

static void
on_prefetch_query_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    PrefetchBatch *batch = user_data;
    if (!batch) return;

    GnostrThreadPrefetch *self = batch->owner;

    GError *error = NULL;
    GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

    if (self->disposed) {
        if (results) g_ptr_array_unref(results);
        g_clear_error(&error);
        prefetch_batch_free(batch);
        return;
    }

    self->query_in_flight = FALSE;
    g_clear_object(&self->cancellable);

    /* Track which requested IDs were returned */
    GHashTable *returned_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (results) {
        g_debug("[PREFETCH] Relay query returned %u events", results->len);

        for (guint i = 0; i < results->len; i++) {
            const char *event_json = g_ptr_array_index(results, i);
            if (!event_json) continue;

            /* Extract event ID using proper parser */
            g_autofree char *event_id = extract_event_id_from_json(event_json);
            if (!event_id) continue;

            /* Dedup within this batch callback */
            if (g_hash_table_contains(returned_ids, event_id))
                continue;
            g_hash_table_insert(returned_ids, g_strdup(event_id), GINT_TO_POINTER(TRUE));

            /* Remove from inflight */
            g_hash_table_remove(self->inflight_ids, event_id);

            /* Push through ingest pipeline */
            gchar *json_copy = g_strdup(event_json);
            gboolean accepted = self->ingest_func(self->ingest_user_data, json_copy);

            if (accepted) {
                g_hash_table_insert(self->resolved_ids, g_strdup(event_id),
                                    GINT_TO_POINTER(TRUE));

                /* Recursive ancestor follow-up */
                gpointer depth_ptr = g_hash_table_lookup(batch->depth_by_id, event_id);
                guint depth = depth_ptr ? GPOINTER_TO_UINT(depth_ptr) : 0;
                if (depth > 0) {
                    process_fetched_ancestor(self, event_json, depth - 1);
                }

                g_debug("[PREFETCH] Ingested ancestor %.16s...", event_id);
            } else {
                /* Ingest queue rejected (backpressure) - record miss for cooldown */
                g_hash_table_insert(self->recent_misses_us, g_strdup(event_id),
                                    GSIZE_TO_POINTER((gsize)g_get_monotonic_time()));
                g_free(json_copy);
                g_debug("[PREFETCH] Ingest rejected %.16s... (backpressure)", event_id);
            }
        }

        g_ptr_array_unref(results);
    } else {
        if (error) {
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                g_debug("[PREFETCH] Relay query failed: %s", error->message);
            g_clear_error(&error);
        }
    }

    /* Mark unreturned inflight IDs as misses */
    for (guint i = 0; i < batch->ids->len; i++) {
        const char *bid = g_ptr_array_index(batch->ids, i);
        if (!g_hash_table_contains(returned_ids, bid)) {
            g_hash_table_remove(self->inflight_ids, bid);
            g_hash_table_insert(self->recent_misses_us, g_strdup(bid),
                                GSIZE_TO_POINTER((gsize)g_get_monotonic_time()));
        }
    }

    g_hash_table_unref(returned_ids);
    prefetch_batch_free(batch);

    /* Schedule next batch if there's still queued work. Use immediate
     * dispatch since debounce already happened for these items. */
    if (!self->disposed && g_hash_table_size(self->queued_targets) > 0) {
        if (self->batch_source_id == 0)
            self->batch_source_id = attach_idle_to_owner(self, on_batch_timer, self);
    }
}

/* ========== Relay query dispatch ========== */

static void
dispatch_relay_query(GnostrThreadPrefetch *self, PrefetchBatch *batch,
                     GPtrArray *relay_urls)
{
    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    self->cancellable = g_cancellable_new();
    self->query_in_flight = TRUE;

    GNostrPool *pool = gnostr_get_shared_query_pool();
    if (!pool) {
        g_debug("[PREFETCH] No shared query pool available");
        /* Clear inflight state */
        for (guint i = 0; i < batch->ids->len; i++)
            g_hash_table_remove(self->inflight_ids, g_ptr_array_index(batch->ids, i));
        self->query_in_flight = FALSE;
        prefetch_batch_free(batch);
        return;
    }

    /* Sync relays */
    const char **urls = g_new0(const char*, relay_urls->len);
    for (guint i = 0; i < relay_urls->len; i++)
        urls[i] = g_ptr_array_index(relay_urls, i);
    gnostr_pool_sync_relays(pool, (const gchar **)urls, relay_urls->len);

    /* Build filter: fetch by IDs */
    GNostrFilter *gf = gnostr_filter_new();
    for (guint i = 0; i < batch->ids->len; i++)
        gnostr_filter_add_id(gf, g_ptr_array_index(batch->ids, i));
    gnostr_filter_set_limit(gf, (gint)batch->ids->len);
    NostrFilter *filter = gnostr_filter_build(gf);
    g_object_unref(gf);

    NostrFilters *filters = nostr_filters_new();
    nostr_filters_add(filters, filter);
    nostr_filter_free(filter);

    /* batch ownership passes to callback (strong ref keeps self alive) */
    gnostr_pool_query_async(pool, filters, self->cancellable,
                            on_prefetch_query_done, batch);

    g_free(urls);
}

/* ========== Ancestor recursion ========== */

/**
 * After fetching or finding an ancestor event locally, parse its NIP-10
 * tags and enqueue further ancestors if depth budget remains.
 */
static void
process_fetched_ancestor(GnostrThreadPrefetch *self,
                         const char           *event_json,
                         guint                 remaining_depth)
{
    if (remaining_depth == 0 || self->disposed) return;

    GnostrNip10ThreadInfo info;
    if (!gnostr_nip10_parse_thread(event_json, &info))
        return;

    if (info.root_id && is_valid_hex64(info.root_id) &&
        !g_hash_table_contains(self->resolved_ids, info.root_id)) {
        enqueue_target(self, info.root_id, remaining_depth, info.root_relay_hint);
    }

    if (info.reply_id && is_valid_hex64(info.reply_id) &&
        g_strcmp0(info.reply_id, info.root_id) != 0 &&
        !g_hash_table_contains(self->resolved_ids, info.reply_id)) {
        enqueue_target(self, info.reply_id, remaining_depth, info.reply_relay_hint);
    }
}

/* ========== Target enqueueing ========== */

static void
enqueue_target(GnostrThreadPrefetch *self, const char *event_id,
               guint depth, const char *relay_hint)
{
    if (!event_id || !is_valid_hex64(event_id) || self->disposed) return;

    /* Skip already resolved */
    if (g_hash_table_contains(self->resolved_ids, event_id))
        return;

    /* Skip already inflight */
    if (g_hash_table_contains(self->inflight_ids, event_id))
        return;

    /* Merge with existing queued target or create new */
    PrefetchTarget *existing = g_hash_table_lookup(self->queued_targets, event_id);
    if (existing) {
        if (depth > existing->remaining_depth)
            existing->remaining_depth = depth;
        prefetch_target_add_hint(existing, relay_hint);
    } else {
        PrefetchTarget *t = prefetch_target_new(event_id, depth, relay_hint);
        g_hash_table_insert(self->queued_targets, g_strdup(event_id), t);
    }

    schedule_batch_dispatch(self);
}

/* ========== Main-thread marshaling for observe_event ========== */

typedef struct {
    GnostrThreadPrefetch *self; /* strong ref via refcount */
    char *root_id;
    char *reply_id;
    char *root_relay_hint;
    char *reply_relay_hint;
    char *source_relay_url;
} ObserveCtx;

static void
observe_ctx_free(ObserveCtx *ctx)
{
    if (!ctx) return;
    prefetch_unref(ctx->self);
    g_free(ctx->root_id);
    g_free(ctx->reply_id);
    g_free(ctx->root_relay_hint);
    g_free(ctx->reply_relay_hint);
    g_free(ctx->source_relay_url);
    g_free(ctx);
}

static gboolean
observe_event_on_owner_context(gpointer user_data)
{
    ObserveCtx *ctx = user_data;
    GnostrThreadPrefetch *self = ctx->self;

    if (self->disposed) {
        observe_ctx_free(ctx);
        return G_SOURCE_REMOVE;
    }

    const char *fallback_hint = ctx->source_relay_url;

    if (ctx->root_id && is_valid_hex64(ctx->root_id)) {
        const char *hint = (ctx->root_relay_hint && *ctx->root_relay_hint)
            ? ctx->root_relay_hint : fallback_hint;
        enqueue_target(self, ctx->root_id, PREFETCH_MAX_ANCESTOR_DEPTH, hint);
    }

    if (ctx->reply_id && is_valid_hex64(ctx->reply_id) &&
        g_strcmp0(ctx->reply_id, ctx->root_id) != 0) {
        const char *hint = (ctx->reply_relay_hint && *ctx->reply_relay_hint)
            ? ctx->reply_relay_hint : fallback_hint;
        enqueue_target(self, ctx->reply_id, PREFETCH_MAX_ANCESTOR_DEPTH, hint);
    }

    observe_ctx_free(ctx);
    return G_SOURCE_REMOVE;
}

/* ========== Public API ========== */

GnostrThreadPrefetch *
gnostr_thread_prefetch_new(GnostrThreadPrefetchIngestFunc  ingest_func,
                           gpointer                        ingest_user_data)
{
    g_return_val_if_fail(ingest_func != NULL, NULL);

    GnostrThreadPrefetch *self = g_new0(GnostrThreadPrefetch, 1);
    self->ref_count = 1; /* initial ref held by caller */

    self->owner_context = g_main_context_ref_thread_default();

    self->queued_targets = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free,
                                                  (GDestroyNotify)prefetch_target_free);
    self->inflight_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->resolved_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->recent_misses_us = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    self->ingest_func = ingest_func;
    self->ingest_user_data = ingest_user_data;

    self->disposed = FALSE;
    self->query_in_flight = FALSE;

    g_debug("[PREFETCH] Thread prefetch service created");

    return self;
}

void
gnostr_thread_prefetch_free(GnostrThreadPrefetch *self)
{
    if (!self) return;

    self->disposed = TRUE;

    /* Cancel pending relay query */
    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        /* Don't clear cancellable here — the completion callback needs
         * to see it was cancelled. It will be freed on final unref. */
    }

    /* Remove debounce timer */
    if (self->batch_source_id > 0) {
        g_source_remove(self->batch_source_id);
        self->batch_source_id = 0;
    }

    g_debug("[PREFETCH] Thread prefetch service disposed");

    /* Release the caller's reference. If async callbacks still hold refs,
     * actual destruction is deferred until they complete and unref. */
    prefetch_unref(self);
}

void
gnostr_thread_prefetch_observe_event(GnostrThreadPrefetch *self,
                                     const char           *event_json,
                                     const char           *source_relay_url)
{
    if (!self || self->disposed || !event_json) return;

    /* Parse NIP-10 thread info - this is the canonical parsing API.
     * It caches results by event ID so repeated calls are cheap. */
    GnostrNip10ThreadInfo info;
    if (!gnostr_nip10_parse_thread(event_json, &info))
        return;

    /* Nothing to prefetch if no thread references */
    if (!info.root_id && !info.reply_id)
        return;

    /* Marshal onto owner context.
     * Copy all strings since info pointers are cache-owned and may be
     * invalidated by concurrent cache operations. */
    ObserveCtx *ctx = g_new0(ObserveCtx, 1);
    ctx->self = prefetch_ref(self); /* strong ref keeps self alive */
    ctx->root_id = info.root_id ? g_strdup(info.root_id) : NULL;
    ctx->reply_id = info.reply_id ? g_strdup(info.reply_id) : NULL;
    ctx->root_relay_hint = info.root_relay_hint ? g_strdup(info.root_relay_hint) : NULL;
    ctx->reply_relay_hint = info.reply_relay_hint ? g_strdup(info.reply_relay_hint) : NULL;
    ctx->source_relay_url = source_relay_url ? g_strdup(source_relay_url) : NULL;

    /* If we're already on the owner context, run directly.
     * Otherwise, schedule via idle. In practice on_multi_sub_event
     * runs on the main thread, which is the owner context. */
    if (g_main_context_is_owner(self->owner_context)) {
        observe_event_on_owner_context(ctx);
    } else {
        g_main_context_invoke(self->owner_context,
                              observe_event_on_owner_context, ctx);
    }
}
