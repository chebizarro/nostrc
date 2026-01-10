#include "nostr_simple_pool.h"
#include "nostr_relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "json.h"
#include "channel.h"
#include "context.h"
#include "error.h"
#include "go.h"
#include "wait_group.h"
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>

/* GnostrSimplePool GObject implementation */
G_DEFINE_TYPE(GnostrSimplePool, gnostr_simple_pool, G_TYPE_OBJECT)

enum {
    SIGNAL_EVENTS,
    N_SIGNALS
};

static guint pool_signals[N_SIGNALS] = {0};

/* Forward decl for URL helpers used below */
static void free_urls(char **urls, size_t count);

 

static void nostr_simple_pool_finalize(GObject *object) {
    GnostrSimplePool *self = GNOSTR_SIMPLE_POOL(object);
    if (self->pool) {
        nostr_simple_pool_free(self->pool);
    }
    G_OBJECT_CLASS(gnostr_simple_pool_parent_class)->finalize(object);
}

/* paginator code moved below after dedup helpers */

static void gnostr_simple_pool_class_init(GnostrSimplePoolClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_simple_pool_finalize;

    /* events: emitted with (GPtrArray* batch) where elements are strings or boxed events depending on emitter */
    pool_signals[SIGNAL_EVENTS] = g_signal_new(
        "events",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, /* class offset */
        NULL, NULL,
        NULL, /* default C marshaller for (POINTER) is fine */
        G_TYPE_NONE,
        1,
        G_TYPE_POINTER /* GPtrArray* */);
}

static void gnostr_simple_pool_init(GnostrSimplePool *self) {
    /* Force robust JSON backend to avoid inline compact parser issues */
    nostr_json_force_fallback(true);
    self->pool = nostr_simple_pool_new();
}

GnostrSimplePool *gnostr_simple_pool_new(void) {
    return g_object_new(GNOSTR_TYPE_SIMPLE_POOL, NULL);
}

void gnostr_simple_pool_add_relay(GnostrSimplePool *self, NostrRelay *relay) {
    if (!self || !self->pool || !relay) return;
    const char *url = nostr_relay_get_url_const(relay);
    if (url) nostr_simple_pool_ensure_relay(self->pool, url);
}

GPtrArray *gnostr_simple_pool_query_sync(GnostrSimplePool *self, NostrFilter *filter, GError **error) {
    (void)self; (void)filter;
    if (error) *error = g_error_new_literal(g_quark_from_static_string("nostr-simple-pool"), 1, "gnostr_simple_pool_query_sync is not implemented in this wrapper");
    return NULL;
}

/* Async scaffolding: subscribe-many (persistent live) */
typedef struct {
    GTask *task;
} SubscribeCtx;

static void subscribe_ctx_free(SubscribeCtx *c) {
    if (!c) return;
    if (c->task) g_object_unref(c->task);
    g_free(c);
}

typedef struct {
    GObject *self_obj; /* GnostrSimplePool* as GObject */
    char **urls;       /* owned deep copy */
    size_t url_count;
    NostrFilters *filters;    /* owned deep copy for thread lifetime */
    GCancellable *cancellable;/* borrowed */
} SubscribeManyCtx;

/* Helpers to deep-copy and free URL arrays */
static char **dup_urls(const char **urls, size_t count) {
    if (!urls || count == 0) return NULL;
    char **out = g_new0(char*, count);
    for (size_t i = 0; i < count; i++) {
        const char *u = urls[i];
        out[i] = u ? g_strdup(u) : NULL;
    }
    return out;
}

static void free_urls(char **urls, size_t count) {
    if (!urls) return;
    for (size_t i = 0; i < count; i++) {
        g_free(urls[i]);
    }
    g_free(urls);
}

static gboolean emit_events_on_main(gpointer data) {
    /* Takes ownership of arr */
    typedef struct { GObject *obj; GPtrArray *arr; } EmitCtx;
    EmitCtx *e = (EmitCtx *)data;
    g_signal_emit(e->obj, pool_signals[SIGNAL_EVENTS], 0, e->arr);
    g_ptr_array_unref(e->arr);
    g_object_unref(e->obj);
    g_free(e);
    return G_SOURCE_REMOVE;
}

/* Bounded de-dup set for event ids */
typedef struct {
    GHashTable *set; /* key: char* (id), value: gpointer(1) */
    GQueue *order;   /* holds same char* pointers in insertion order */
    gsize cap;
} DedupSet;

static DedupSet *dedup_set_new(gsize cap) {
    DedupSet *d = g_new0(DedupSet, 1);
    d->set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    d->order = g_queue_new();
    d->cap = cap > 0 ? cap : 65536;
    return d;
}

static void dedup_set_free(DedupSet *d) {
    if (!d) return;
    if (d->order) {
        /* order nodes' data are same pointers as keys in set; do not free here */
        g_queue_free(d->order);
    }
    if (d->set) g_hash_table_destroy(d->set);
    g_free(d);
}

static gboolean dedup_set_seen(DedupSet *d, const char *id) {
    if (!d || !id || !*id) return FALSE;
    if (g_hash_table_contains(d->set, id)) return TRUE;
    /* Insert */
    char *key = g_strdup(id);
    g_hash_table_insert(d->set, key, GINT_TO_POINTER(1));
    g_queue_push_tail(d->order, key);
    /* Evict if over cap */
    while (g_hash_table_size(d->set) > d->cap) {
        char *old = g_queue_pop_head(d->order);
        if (!old) break;
        g_hash_table_remove(d->set, old); /* frees old via key destroy */
    }
    return FALSE;
}

/* Background paginator with interval */
typedef struct {
    GObject *self_obj;      /* GnostrSimplePool* as GObject */
    char **urls;            /* owned deep copy */
    size_t url_count;
    NostrFilter *base_filter; /* owned copy */
    guint interval_ms;
    GCancellable *cancellable; /* optional, ref-held */
    GTask *task;            /* for immediate async completion */
} PaginateCtx;

static void paginate_ctx_free(PaginateCtx *ctx) {
    if (!ctx) return;
    if (ctx->urls) {
        for (size_t i = 0; i < ctx->url_count; i++) g_free(ctx->urls[i]);
        g_free(ctx->urls);
    }
    if (ctx->base_filter) nostr_filter_free(ctx->base_filter);
    if (ctx->task) g_object_unref(ctx->task);
    if (ctx->cancellable) g_object_unref(ctx->cancellable);
    if (ctx->self_obj) g_object_unref(ctx->self_obj);
    g_free(ctx);
}

static gpointer paginate_with_interval_thread(gpointer user_data) {
    PaginateCtx *ctx = (PaginateCtx *)user_data;
    if (!ctx) return NULL;

    /* Session-level de-dup for event IDs across pages */
    DedupSet *seen = dedup_set_new(65536);

    /* Initialize until cursor from base filter */
    NostrTimestamp next_until = 0;
    if (ctx->base_filter) {
        next_until = (NostrTimestamp)nostr_filter_get_until_i64(ctx->base_filter);
    }

    const char *exit_reason = "";
    while (!(ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable))) {
        /* Build per-page filters: copy base and set until */
        NostrFilters *filters = nostr_filters_new();
        NostrFilter *f = nostr_filter_copy(ctx->base_filter);
        if (next_until > 0) {
            nostr_filter_set_until_i64(f, (int64_t)next_until);
        }
        nostr_filters_add(filters, f); /* moves f */

        /* Prepare subs per URL */
        typedef struct { NostrRelay *relay; NostrSubscription *sub; gboolean eosed; } SubItem;
        GPtrArray *subs = g_ptr_array_new_with_free_func(NULL);
        GoContext *bg = go_context_background();
        for (size_t i = 0; i < ctx->url_count; i++) {
            if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) break;
            const char *url = ctx->urls[i];
            if (!url || !*url) continue;
            Error *err = NULL;
            NostrRelay *relay = nostr_relay_new(bg, url, &err);
            if (!relay) {
                if (err) { free_error(err); }
                continue;
            }
            if (!nostr_relay_connect(relay, &err)) {
                if (err) { free_error(err); }
                nostr_relay_free(relay);
                continue;
            }
            /* Prepare subscription using shared filters (no ownership transfer) */
            NostrSubscription *sub = nostr_relay_prepare_subscription(relay, bg, filters);
            if (!sub || !nostr_subscription_fire(sub, &err)) {
                if (err) { free_error(err); }
                if (sub) { nostr_subscription_free(sub); }
                nostr_relay_disconnect(relay);
                nostr_relay_free(relay);
                continue;
            }
            SubItem item = { .relay = relay, .sub = sub, .eosed = FALSE };
            g_ptr_array_add(subs, g_memdup2(&item, sizeof(SubItem)));
        }

        /* Drain page */
        gboolean page_has_new = FALSE;
        gint64 min_created_at = -1;
        GPtrArray *batch = g_ptr_array_new();
        for (;;) {
            gboolean any = FALSE;
            for (guint i = 0; i < subs->len; i++) {
                SubItem *it = (SubItem *)subs->pdata[i];
                if (!it || !it->sub) continue;
                void *msg = NULL;
                GoChannel *ch_events = nostr_subscription_get_events_channel(it->sub);
                while (ch_events && go_channel_try_receive(ch_events, &msg) == 0) {
                    any = TRUE;
                    if (msg) {
                        NostrEvent *ev = (NostrEvent*)msg;
                        const char *eid = nostr_event_get_id(ev);
                        if (eid && *eid && dedup_set_seen(seen, eid)) {
                            nostr_event_free(ev);
                        } else {
                            page_has_new = TRUE;
                            int64_t ca = nostr_event_get_created_at(ev);
                            if (min_created_at < 0 || (ca > 0 && ca < min_created_at)) {
                                min_created_at = ca;
                            }
                            g_ptr_array_add(batch, ev);
                        }
                    }
                    msg = NULL;
                }
                GoChannel *ch_eose = nostr_subscription_get_eose_channel(it->sub);
                if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
                    it->eosed = TRUE;
                }
            }

            /* Stop when all EOSE or cancelled */
            gboolean all_eosed = TRUE;
            for (guint i = 0; i < subs->len; i++) {
                SubItem *it = (SubItem *)subs->pdata[i];
                if (it && !it->eosed) { all_eosed = FALSE; break; }
            }
            if (all_eosed || (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable))) break;
            if (!any) g_usleep(1000); /* 1ms */
        }

        /* Emit batch if any */
        if (batch->len > 0) {
            typedef struct { GObject *obj; GPtrArray *arr; } EmitCtx;
            EmitCtx *e = g_new0(EmitCtx, 1);
            e->obj = g_object_ref(ctx->self_obj);
            e->arr = batch; /* transfer */
            g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, emit_events_on_main, e, NULL);
        } else {
            g_ptr_array_unref(batch);
        }

        /* Cleanup subs */
        for (guint i = 0; i < subs->len; i++) {
            SubItem *it = (SubItem*)subs->pdata[i];
            if (!it) continue;
            if (it->sub) { nostr_subscription_close(it->sub, NULL); nostr_subscription_free(it->sub); }
            if (it->relay) { nostr_relay_disconnect(it->relay); nostr_relay_free(it->relay); }
            g_free(it);
        }
        g_ptr_array_unref(subs);
        nostr_filters_free(filters);

        if (!page_has_new) break; /* stop if page had only repeats */
        if (min_created_at > 0) {
            next_until = (NostrTimestamp)min_created_at;
        }

        /* Sleep interval before next page */
        if (ctx->interval_ms > 0) g_usleep((gulong)ctx->interval_ms * 1000);
    }

    dedup_set_free(seen);
    paginate_ctx_free(ctx); /* ctx->task already completed in async setup */
    return NULL;
}

void gnostr_simple_pool_paginate_with_interval_async(GnostrSimplePool *self,
                                                     const char **urls,
                                                     size_t url_count,
                                                     const NostrFilter *filter,
                                                     guint interval_ms,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback cb,
                                                     gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_SIMPLE_POOL(self));
    g_return_if_fail(urls != NULL || url_count == 0);
    g_return_if_fail(filter != NULL);

    PaginateCtx *ctx = g_new0(PaginateCtx, 1);
    ctx->self_obj = g_object_ref(G_OBJECT(self));
    ctx->urls = g_new0(char*, url_count);
    ctx->url_count = url_count;
    for (size_t i = 0; i < url_count; i++) ctx->urls[i] = g_strdup(urls[i]);
    ctx->base_filter = nostr_filter_copy(filter);
    ctx->interval_ms = interval_ms;
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    ctx->task = g_task_new(G_OBJECT(self), cancellable, cb, user_data);

    /* Spawn worker */
    GThread *thr = g_thread_new("nostr-paginate", paginate_with_interval_thread, ctx);
    g_thread_unref(thr);

    /* Immediate async success */
    g_task_return_boolean(ctx->task, TRUE);
    g_object_unref(ctx->task);
    ctx->task = NULL;
}

gboolean gnostr_simple_pool_paginate_with_interval_finish(GnostrSimplePool *self,
                                                          GAsyncResult *res,
                                                          GError **error) {
    (void)self;
    return g_task_propagate_boolean(G_TASK(res), error);
}

static gpointer subscribe_many_thread(gpointer user_data) {
    SubscribeManyCtx *ctx = (SubscribeManyCtx *)user_data;
    /* For each URL, set up a subscription */
    typedef struct { NostrRelay *relay; NostrSubscription *sub; gboolean eosed; guint64 emitted; gint64 start_us; gint64 eose_us; } SubItem;
    GPtrArray *subs = g_ptr_array_new_with_free_func(NULL);

    GoContext *bg = go_context_background();
    GnostrSimplePool *gobj_pool = GNOSTR_SIMPLE_POOL(ctx->self_obj);
    
    for (size_t i = 0; i < ctx->url_count; i++) {
        if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) break;
        const char *url = ctx->urls[i];
        if (!url || !*url) continue; /* skip empty */
        
        /* CRITICAL: Check if relay already exists in pool - reuse it instead of creating duplicate! */
        NostrRelay *relay = NULL;
        gboolean relay_is_from_pool = FALSE;
        
        if (gobj_pool && gobj_pool->pool) {
            pthread_mutex_lock(&gobj_pool->pool->pool_mutex);
            for (size_t j = 0; j < gobj_pool->pool->relay_count; j++) {
                if (gobj_pool->pool->relays[j] && 
                    gobj_pool->pool->relays[j]->url && 
                    strcmp(gobj_pool->pool->relays[j]->url, url) == 0) {
                    relay = gobj_pool->pool->relays[j];
                    relay_is_from_pool = TRUE;
                    g_message("simple_pool: Reusing existing relay %s from pool", url);
                    break;
                }
            }
            pthread_mutex_unlock(&gobj_pool->pool->pool_mutex);
        }
        
        /* Only create new relay if not found in pool */
        if (!relay) {
            Error *err = NULL;
            relay = nostr_relay_new(NULL, url, &err);
            if (!relay) {
                g_warning("simple_pool: failed to create relay for %s: %s", url, (err && err->message) ? err->message : "(no detail)");
                if (err) free_error(err);
                continue;
            }
            if (!nostr_relay_connect(relay, &err)) {
                g_warning("simple_pool: connect failed for %s: %s", url, (err && err->message) ? err->message : "(no detail)");
                if (err) free_error(err);
                nostr_relay_free(relay);
                continue;
            }
            
            /* Add new relay to pool */
            if (gobj_pool && gobj_pool->pool) {
                nostr_simple_pool_add_relay(gobj_pool->pool, relay);
                relay_is_from_pool = TRUE; /* Now it's in the pool */
                g_message("simple_pool: Added NEW relay %s to pool (now has %zu relays)", url, gobj_pool->pool->relay_count);
            }
        }
        
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, bg, ctx->filters);
        if (!sub) {
            g_warning("simple_pool: prepare_subscription failed for %s", url);
            /* DON'T free relay if it's in the pool! */
            if (!relay_is_from_pool) {
                nostr_relay_disconnect(relay);
                nostr_relay_free(relay);
            }
            continue;
        }
        Error *err = NULL;
        if (!nostr_subscription_fire(sub, &err)) {
            g_warning("simple_pool: subscription_fire failed for %s: %s", url, (err && err->message) ? err->message : "(no detail)");
            if (err) free_error(err);
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
            /* DON'T free relay if it's in the pool! */
            if (!relay_is_from_pool) {
                nostr_relay_disconnect(relay);
                nostr_relay_free(relay);
            }
            continue;
        }
        SubItem item = { .relay = relay, .sub = sub, .eosed = FALSE, .emitted = 0, .start_us = g_get_monotonic_time(), .eose_us = -1 };
        g_ptr_array_add(subs, g_memdup2(&item, sizeof(SubItem)));
    }

    /* Streaming loop */
    DedupSet *dedup = dedup_set_new(65536);
    gboolean bootstrap_emitted = FALSE;
    while (!(ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable))) {
        GPtrArray *batch = g_ptr_array_new();
        gboolean any = FALSE;
        /* Collect from all subscriptions non-blocking, drain until empty */
        for (guint i = 0; i < subs->len; i++) {
            SubItem *it = (SubItem *)subs->pdata[i];
            if (!it || !it->sub) continue;
            GoChannel *ch_events = nostr_subscription_get_events_channel(it->sub);
            void *msg = NULL;
            while (ch_events && go_channel_try_receive(ch_events, &msg) == 0) {
                any = TRUE;
                if (msg) {
                    NostrEvent *ev = (NostrEvent*)msg;
                    const char *eid = nostr_event_get_id(ev);
                    if (eid && *eid && dedup_set_seen(dedup, eid)) {
                        /* duplicate: drop */
                        nostr_event_free(ev);
                    } else {
                        g_ptr_array_add(batch, ev);
                        it->emitted++;
                    }
                }
                msg = NULL;
            }
            /* Drain EOSE signals */
            GoChannel *ch_eose = nostr_subscription_get_eose_channel(it->sub);
            if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
                it->eosed = TRUE;
                if (it->eose_us < 0 && it->start_us > 0) {
                    it->eose_us = g_get_monotonic_time() - it->start_us;
                }
            }
        }

        if (batch->len > 0) {
            /* Emit on main loop */
            typedef struct { GObject *obj; GPtrArray *arr; } EmitCtx;
            EmitCtx *e = g_new0(EmitCtx, 1);
            e->obj = g_object_ref(ctx->self_obj);
            e->arr = batch; /* transfer */
            g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, emit_events_on_main, e, NULL);
        } else {
            g_ptr_array_unref(batch);
        }
        /* If all subs have signaled EOSE once, mark bootstrap complete (log only once) */
        if (!bootstrap_emitted && subs->len > 0) {
            gboolean all_eosed = TRUE;
            for (guint i = 0; i < subs->len; i++) {
                SubItem *it = (SubItem *)subs->pdata[i];
                if (it && !it->eosed) { all_eosed = FALSE; break; }
            }
            if (all_eosed) {
                bootstrap_emitted = TRUE;
                g_message("simple_pool: bootstrap complete (EOSE from all relays)");
            }
        }
        /* Adaptive sleep: if nothing was read, back off a bit; otherwise, immediately iterate */
        if (!any) g_usleep(1000 * 5); /* 5ms idle sleep to reduce CPU */
    }

    /* Print per-subscription stats */
    for (guint i = 0; i < subs->len; i++) {
        SubItem *it = (SubItem*)subs->pdata[i];
        if (!it || !it->sub) continue;
        const char *url = it->relay ? nostr_relay_get_url_const(it->relay) : "<no-relay>";
        unsigned long long enq = nostr_subscription_events_enqueued(it->sub);
        unsigned long long drop = nostr_subscription_events_dropped(it->sub);
        double eose_ms = (it->eose_us >= 0) ? (it->eose_us / 1000.0) : -1.0;
        g_message("simple_pool: stats url=%s enqueued=%llu emitted=%" G_GUINT64_FORMAT " dropped=%llu eose_ms=%.3f",
                  url ? url : "<null>", enq, it->emitted, drop, eose_ms);
    }

    /* Cleanup */
    dedup_set_free(dedup);
    for (guint i = 0; i < subs->len; i++) {
        SubItem *it = (SubItem*)subs->pdata[i];
        if (it) {
            if (it->sub) { nostr_subscription_close(it->sub, NULL); nostr_subscription_free(it->sub); }
            if (it->relay) { nostr_relay_disconnect(it->relay); nostr_relay_free(it->relay); }
            g_free(it);
        }
    }
    g_ptr_array_unref(subs);
    free_urls(ctx->urls, ctx->url_count);
    if (ctx->filters) {
        /* Free thread-owned filters after all subscriptions are cleaned up */
        nostr_filters_free(ctx->filters);
        ctx->filters = NULL;
    }
    g_object_unref(ctx->self_obj);
    g_free(ctx);
    return NULL;
}

void gnostr_simple_pool_subscribe_many_async(GnostrSimplePool *self,
                                             const char **urls,
                                             size_t url_count,
                                             NostrFilters *filters,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback cb,
                                             gpointer user_data) {
    /* Start background streaming thread */
    SubscribeManyCtx *ctx = g_new0(SubscribeManyCtx, 1);
    ctx->self_obj = G_OBJECT(g_object_ref(self));
    /* Deep copy URLs for thread lifetime */
    ctx->urls = dup_urls(urls, url_count);
    ctx->url_count = url_count;
    /* Deep-copy filters to decouple from caller lifetime */
    if (filters) {
        NostrFilters *copy = nostr_filters_new();
        if (filters->count > 0 && filters->filters) {
            for (size_t i = 0; i < filters->count; i++) {
                NostrFilter *fc = nostr_filter_copy(&filters->filters[i]);
                if (fc) {
                    /* nostr_filters_add takes ownership and zeros fc */
                    nostr_filters_add(copy, fc);
                }
            }
        }
        ctx->filters = copy;
    } else {
        ctx->filters = NULL;
    }
    ctx->cancellable = cancellable;
    g_thread_new("nostr-subscribe-many", subscribe_many_thread, ctx);

    /* Immediate success for async setup */
    GTask *task = g_task_new(G_OBJECT(self), cancellable, cb, user_data);
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
}

gboolean gnostr_simple_pool_subscribe_many_finish(GnostrSimplePool *self,
                                                  GAsyncResult *res,
                                                  GError **error) {
    (void)self;
    return g_task_propagate_boolean(G_TASK(res), error);
}

typedef struct {
    GObject *self_obj;  /* GnostrSimplePool* as GObject */
    char **urls;        /* owned deep copy */
    size_t url_count;
    NostrFilter *filter; /* owned copy of filter */
    GCancellable *cancellable;  /* borrowed */
    GTask *task;        /* owned ref to GTask for async completion */
    GPtrArray *results; /* collected results */
} QuerySingleCtx;

/* Helper to free query context */
static void query_single_ctx_free(QuerySingleCtx *ctx) {
    if (!ctx) return;
    if (ctx->urls) {
        for (size_t i = 0; i < ctx->url_count; i++) {
            g_free(ctx->urls[i]);
        }
        g_free(ctx->urls);
    }
    if (ctx->filter) {
        nostr_filter_free(ctx->filter);
    }
    if (ctx->task) g_object_unref(ctx->task);
    if (ctx->results) g_ptr_array_unref(ctx->results);
    if (ctx->cancellable) g_object_unref(ctx->cancellable);
    g_free(ctx);
}

static gboolean query_single_complete_ok(gpointer data) {
    GTask *task = G_TASK(data);
    g_task_return_boolean(task, TRUE);
    /* Unref provided by destroy notify in invoke_full if set; be safe */
    return G_SOURCE_REMOVE;
}

static gpointer query_single_thread(gpointer user_data) {
    QuerySingleCtx *ctx = (QuerySingleCtx *)user_data;
    GnostrSimplePool *self = GNOSTR_SIMPLE_POOL(ctx->self_obj);
    
    // Create results array
    ctx->results = g_ptr_array_new_with_free_func(g_free);
    
    // Create a temporary subscription for each URL until we get a result
    for (size_t i = 0; i < ctx->url_count; i++) {
        if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) {
            break;
        }

        const char *url = ctx->urls[i];
        if (!url || !*url) continue;

        // Create a new relay connection
        Error *err = NULL;
        GoContext *gctx = go_context_background();
        NostrRelay *relay = nostr_relay_new(gctx, url, &err);
        if (!relay) {
            g_warning("Failed to create relay for %s: %s", url, 
                     err ? err->message : "unknown error");
            if (err) { free_error(err); err = NULL; }
            continue;
        }

        // Connect to the relay
        if (!nostr_relay_connect(relay, &err)) {
            g_warning("Failed to connect to %s: %s", url, 
                     err ? err->message : "unknown error");
            if (err) { free_error(err); err = NULL; }
            nostr_relay_free(relay);
            continue;
        }

        // Create a subscription with the filter
        NostrFilters *filters = nostr_filters_new();
        NostrFilter *fcopy = nostr_filter_copy(ctx->filter);
        nostr_filters_add(filters, fcopy); /* moves fcopy contents */
        NostrSubscription *sub = nostr_subscription_new(relay, filters); /* takes ownership of filters */

        // Fire the subscription
        if (!nostr_subscription_fire(sub, &err)) {
            g_warning("Failed to fire subscription to %s: %s", url, 
                     err ? err->message : "unknown error");
            if (err) { free_error(err); err = NULL; }
            nostr_subscription_free(sub);
            nostr_relay_free(relay);
            continue;
        }

        // Wait for first event or EOSE
        bool got_event = false;
        while (!got_event) {
            if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) {
                break;
            }

            // Check for event
            NostrEvent *evt = NULL;
            GoChannel *ch_events = nostr_subscription_get_events_channel(sub);
            if (ch_events && go_channel_try_receive(ch_events, (void**)&evt) == 0) {
                char *json = nostr_event_serialize(evt);
                if (json) {
                    g_ptr_array_add(ctx->results, json);
                    got_event = true;
                }
                nostr_event_free(evt);
            }

            // Check for EOSE
            GoChannel *ch_eose = nostr_subscription_get_eose_channel(sub);
            if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
                break;
            }

            // Small sleep to prevent busy waiting
            g_usleep(1000);  // 1ms
        }

        // Cleanup
        nostr_subscription_free(sub);
        nostr_relay_free(relay);

        if (got_event) {
            break;  // Got our event, no need to try other relays
        }
    }

    // Schedule completion on the main thread; task owns ctx via task_data
    g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, query_single_complete_ok,
                               g_object_ref(ctx->task), (GDestroyNotify)g_object_unref);
    return NULL;
}

void gnostr_simple_pool_query_single_async(GnostrSimplePool *self,
                                          const char **urls,
                                          size_t url_count,
                                          const NostrFilter *filter,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_SIMPLE_POOL(self));
    g_return_if_fail(urls != NULL || url_count == 0);
    g_return_if_fail(filter != NULL);

    // Create and populate context
    QuerySingleCtx *ctx = g_new0(QuerySingleCtx, 1);
    ctx->self_obj = g_object_ref(G_OBJECT(self));
    ctx->urls = g_new0(char *, url_count);
    ctx->url_count = url_count;
    ctx->filter = nostr_filter_copy(filter);
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    ctx->task = g_task_new(G_OBJECT(self), cancellable, callback, user_data);
    g_task_set_task_data(ctx->task, ctx, (GDestroyNotify)query_single_ctx_free);

    // Deep copy URLs
    for (size_t i = 0; i < url_count; i++) {
        ctx->urls[i] = g_strdup(urls[i]);
    }

    // Start worker thread
    GThread *thread = g_thread_new("nostr-query-single", query_single_thread, g_steal_pointer(&ctx));
    g_thread_unref(thread);  // we don't need to join
}

GPtrArray *gnostr_simple_pool_query_single_finish(GnostrSimplePool *self,
                                                 GAsyncResult *res,
                                                 GError **error) {
    g_return_val_if_fail(GNOSTR_IS_SIMPLE_POOL(self), NULL);
    g_return_val_if_fail(g_task_is_valid(res, self), NULL);

    // Get the task data
    gpointer data = g_task_get_task_data(G_TASK(res));
    if (!data) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Invalid task data in query_single_finish");
        return NULL;
    }
    QuerySingleCtx *ctx = data;

    // Propagate any error from the task
    if (g_task_propagate_boolean(G_TASK(res), error)) {
        // On success, return the results (transfer full ownership to caller)
        return g_steal_pointer(&ctx->results);
    } else {
        // On failure, ensure we have an error set
        if (error && *error == NULL) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Failed to complete query_single operation");
        }
        return NULL;
    }
}

/* ================= Batch fetch profiles by authors (kind 0) ================= */
typedef struct {
    GObject *self_obj;      /* GnostrSimplePool* as GObject */
    char **urls;            /* deep copy */
    size_t url_count;
    char **authors;         /* deep copy */
    size_t author_count;
    int limit;              /* optional per-relay limit */
    GCancellable *cancellable; /* optional */
    GTask *task;            /* async completion */
    GPtrArray *results;     /* char* JSON strings */
    
    /* Goroutine-specific fields (only used by goroutine implementation) */
    void *wg;               /* GoWaitGroup* - opaque pointer */
    GPtrArray *subs;        /* SubItem* array */
    void *dedup;            /* DedupSet* - opaque pointer */
    void *results_mutex;    /* GMutex* - opaque pointer */
} FetchProfilesCtx;

/* Made non-static so goroutine implementation can call it */
void fetch_profiles_ctx_free_full(FetchProfilesCtx *ctx) {
    if (!ctx) return;
    if (ctx->urls) { for (size_t i = 0; i < ctx->url_count; i++) g_free(ctx->urls[i]); g_free(ctx->urls); }
    if (ctx->authors) { for (size_t i = 0; i < ctx->author_count; i++) g_free(ctx->authors[i]); g_free(ctx->authors); }
    if (ctx->results) g_ptr_array_unref(ctx->results);
    if (ctx->task) g_object_unref(ctx->task);
    if (ctx->cancellable) g_object_unref(ctx->cancellable);
    if (ctx->self_obj) g_object_unref(ctx->self_obj);
    g_free(ctx);
}

static gboolean fetch_profiles_complete_ok(gpointer data) {
    GTask *task = G_TASK(data);
    g_task_return_boolean(task, TRUE);
    return G_SOURCE_REMOVE;
}

/* NEW ASYNC ARCHITECTURE: No blocking threads, use goroutines + GLib idle callbacks */

typedef struct {
    NostrRelay *relay;
    NostrSubscription *sub;
    GoChannel *raw;        /* Used by async implementation */
    char *relay_url;       /* Used by goroutine implementation for logging */
    gboolean eosed;
} SubItem;

typedef struct {
    FetchProfilesCtx *ctx;
    GPtrArray *subs;  /* SubItem* */
    DedupSet *dedup;
    GHashTable *authors_needed;
    GoContext *bg;
    NostrFilters *filters;
    guint64 t_start;
    guint64 t_last_activity;
    guint loop_iterations;
    gboolean done_all_authors;
    guint idle_source_id;
    gboolean cleanup_started;
} FetchProfilesState;

/* Forward declaration for goroutine implementation */
extern void fetch_profiles_goroutine_start(FetchProfilesCtx *ctx);

static void fetch_profiles_state_free(FetchProfilesState *state) {
    if (!state) return;
    
    g_message("PROFILE_FETCH: Freeing state (subs=%u)", state->subs ? state->subs->len : 0);
    
    if (state->idle_source_id) {
        g_source_remove(state->idle_source_id);
        state->idle_source_id = 0;
    }
    
    /* Cleanup subscriptions asynchronously */
    if (state->subs && !state->cleanup_started) {
        state->cleanup_started = TRUE;
        const uint64_t CLEANUP_TIMEOUT_MS = 500;
        GPtrArray *cleanup_handles = g_ptr_array_new();
        
        for (guint i = 0; i < state->subs->len; i++) {
            SubItem *it = (SubItem*)state->subs->pdata[i];
            if (!it || !it->sub) {
                if (it) g_free(it);
                continue;
            }
            
            AsyncCleanupHandle *handle = nostr_subscription_free_async(it->sub, CLEANUP_TIMEOUT_MS);
            if (handle) {
                g_ptr_array_add(cleanup_handles, handle);
            }
            g_free(it);
        }
        
        /* Wait for cleanups - this is OK because we're in cleanup phase */
        guint timeout_count = 0;
        for (guint i = 0; i < cleanup_handles->len; i++) {
            AsyncCleanupHandle *handle = (AsyncCleanupHandle*)cleanup_handles->pdata[i];
            if (!handle) continue;
            
            if (!nostr_subscription_cleanup_wait(handle, CLEANUP_TIMEOUT_MS + 500)) {
                timeout_count++;
            }
            nostr_subscription_cleanup_abandon(handle);
        }
        
        if (timeout_count > 0) {
            g_warning("PROFILE_FETCH: cleanup leaked %u subscription(s)", timeout_count);
        }
        
        g_ptr_array_free(cleanup_handles, TRUE);
    }
    
    if (state->subs) g_ptr_array_free(state->subs, TRUE);
    if (state->dedup) dedup_set_free(state->dedup);
    if (state->authors_needed) g_hash_table_unref(state->authors_needed);
    if (state->filters) nostr_filters_free(state->filters);
    
    /* No cleanup needed - goroutines are lightweight */
    
    g_free(state);
}

/* ========================================================================
 * GOROUTINE-BASED PROFILE FETCHING
 * Consolidated from nostr_simple_pool_goroutine.c
 * Note: DedupSet helpers already defined above, reusing them here
 * ======================================================================== */

/* Cleanup timeout */
#define CLEANUP_TIMEOUT_MS 500

/* Note: SubItem and FetchProfilesCtx are already defined above at file scope */

/* Context for subscription goroutine */
typedef struct {
    SubItem *item;
    GoWaitGroup *wg;
} SubGoroutineCtx;

/* Forward declarations */
static gboolean fetch_profiles_complete_ok(gpointer data);

/* Goroutine function to fire a single subscription */
static void *subscription_goroutine(void *arg) {
    SubGoroutineCtx *ctx = (SubGoroutineCtx*)arg;
    if (!ctx || !ctx->item || !ctx->item->sub) {
        if (ctx && ctx->wg) go_wait_group_done(ctx->wg);
        g_free(ctx);
        return NULL;
    }
    
    SubItem *item = ctx->item;
    g_message("[GOROUTINE] Starting subscription for relay %s (sub=%p)", item->relay_url, (void*)item->sub);
    
    /* Verify subscription has channels */
    GoChannel *ch_events = nostr_subscription_get_events_channel(item->sub);
    GoChannel *ch_eose = nostr_subscription_get_eose_channel(item->sub);
    g_message("[GOROUTINE] Subscription channels: events=%p eose=%p", (void*)ch_events, (void*)ch_eose);
    
    /* Fire subscription - this may block, but we're in a goroutine so it's OK */
    Error *err = NULL;
    bool fire_result = nostr_subscription_fire(item->sub, &err);
    if (!fire_result) {
        g_critical("[GOROUTINE] subscription_fire FAILED for %s: %s", 
                  item->relay_url, err ? err->message : "unknown");
        if (err) free_error(err);
    } else {
        g_message("[GOROUTINE] Subscription fired successfully for %s", item->relay_url);
    }
    
    /* Signal completion */
    go_wait_group_done(ctx->wg);
    g_free(ctx);
    return NULL;
}

/* Main goroutine that creates subscriptions and polls for events */
static void *fetch_profiles_goroutine(void *arg) {
    FetchProfilesCtx *ctx = (FetchProfilesCtx*)arg;
    if (!ctx) {
        g_critical("[GOROUTINE] ctx is NULL!");
        return NULL;
    }
    if (!ctx->self_obj) {
        g_critical("[GOROUTINE] ctx->self_obj is NULL!");
        return NULL;
    }
    
    g_message("[GOROUTINE] Profile fetch starting (authors=%zu relays=%zu)", 
              ctx->author_count, ctx->url_count);
    
    /* Create background context with cancel */
    CancelContextResult cancel_ctx = go_context_with_cancel(go_context_background());
    GoContext *bg = cancel_ctx.context;
    CancelFunc cancel = cancel_ctx.cancel;
    
    /* Build filter */
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();
    int kinds[] = {0};
    nostr_filter_set_kinds(f, kinds, 1);
    
    if (ctx->author_count > 0) {
        const char **authv = (const char **)ctx->authors;
        nostr_filter_set_authors(f, authv, ctx->author_count);
        
        g_message("[GOROUTINE] Requesting kind-0 for %zu authors", ctx->author_count);
        for (size_t i = 0; i < ctx->author_count && i < 3; i++) {
            g_message("[GOROUTINE]   author[%zu]: %.16s...", i, ctx->authors[i] ? ctx->authors[i] : "(null)");
        }
        if (ctx->author_count > 3) {
            g_message("[GOROUTINE]   ... and %zu more", ctx->author_count - 3);
        }
    }
    nostr_filters_add(filters, f);
    
    /* Create subscriptions for each relay */
    for (size_t i = 0; i < ctx->url_count; i++) {
        const char *url = ctx->urls[i];
        if (!url || !*url) continue;
        
        /* Get relay from pool - CRITICAL: Access pool through GObject to ensure it's still valid */
        GnostrSimplePool *gobj_pool = GNOSTR_SIMPLE_POOL(ctx->self_obj);
        if (!gobj_pool || !gobj_pool->pool) {
            g_critical("[GOROUTINE] Pool GObject or underlying pool is NULL!");
            continue;
        }
        NostrSimplePool *pool = gobj_pool->pool;
        g_message("[GOROUTINE] Accessing pool=%p (has %zu relays)", (void*)pool, pool->relay_count);
        
        NostrRelay *relay = NULL;
        pthread_mutex_lock(&pool->pool_mutex);
        for (size_t j = 0; j < pool->relay_count; j++) {
            if (pool->relays[j] && strcmp(pool->relays[j]->url, url) == 0) {
                relay = pool->relays[j];
                break;
            }
        }
        pthread_mutex_unlock(&pool->pool_mutex);
        
        if (!relay) {
            g_warning("[GOROUTINE] CRITICAL: Relay not in pool (skipping): %s", url);
            g_warning("[GOROUTINE] Pool has %zu relays, but '%s' not found!", pool->relay_count, url);
            continue;
        }
        
        if (!nostr_relay_is_connected(relay)) {
            g_warning("[GOROUTINE] CRITICAL: Relay not connected (skipping): %s", url);
            continue;
        }
        
        /* Create subscription */
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, bg, filters);
        if (!sub) {
            g_warning("[GOROUTINE] prepare_subscription failed: %s", url);
            continue;
        }
        
        /* Create SubItem */
        SubItem *item = g_new0(SubItem, 1);
        item->relay = relay;
        item->sub = sub;
        item->relay_url = g_strdup(url);
        item->eosed = FALSE;
        g_ptr_array_add(ctx->subs, item);
        
        /* Debug: log subscription ID */
        const char *sid = nostr_subscription_get_id(sub);
        g_message("[GOROUTINE] Created subscription sid=%s for relay %s", sid ? sid : "null", url);
        
        /* Launch goroutine to fire subscription */
        SubGoroutineCtx *sub_ctx = g_new0(SubGoroutineCtx, 1);
        sub_ctx->item = item;
        sub_ctx->wg = (GoWaitGroup*)ctx->wg;
        
        go_wait_group_add((GoWaitGroup*)ctx->wg, 1);
        if (go(subscription_goroutine, sub_ctx) != 0) {
            g_critical("[GOROUTINE] Failed to launch subscription goroutine for %s", url);
            go_wait_group_done((GoWaitGroup*)ctx->wg);  /* Decrement since goroutine didn't start */
            g_free(sub_ctx);
            /* Mark subscription as failed so we don't wait for it */
            item->sub = NULL;
        }
    }
    
    if (ctx->subs->len == 0) {
        g_critical("[GOROUTINE] CRITICAL: Created ZERO subscriptions! No profiles will be fetched from relays!");
        g_critical("[GOROUTINE] Requested %zu relays, but none were available", ctx->url_count);
    } else {
        g_message("[GOROUTINE] Created %u subscriptions (requested %zu relays), waiting for fire completion", 
                  ctx->subs->len, ctx->url_count);
    }
    
    /* Wait for all subscriptions to fire (with timeout to prevent infinite hang) */
    g_message("[GOROUTINE] Waiting for subscriptions to fire...");
    
    /* Poll WaitGroup with timeout instead of blocking forever */
    guint64 wait_start = g_get_monotonic_time();
    const guint64 FIRE_TIMEOUT_US = 3000000; // 3 seconds max wait for subscriptions to fire
    
    while (TRUE) {
        /* Check if WaitGroup counter is zero (all done) */
        GoWaitGroup *wg = (GoWaitGroup*)ctx->wg;
        int count = atomic_load(&wg->counter);
        
        if (count == 0) {
            g_message("[GOROUTINE] All subscriptions fired, polling for events");
            break;
        }
        
        /* Check timeout */
        guint64 now = g_get_monotonic_time();
        if ((now - wait_start) > FIRE_TIMEOUT_US) {
            g_warning("[GOROUTINE] Timeout waiting for subscriptions to fire (counter=%d), proceeding anyway", count);
            break;
        }
        
        /* Sleep briefly */
        g_usleep(10000); // 10ms
    }
    
    /* Poll for events with timeout */
    guint64 t_start = g_get_monotonic_time();
    guint64 t_last_activity = t_start;
    /* Timeout configuration:
     * - QUIET: 1 second of no activity (events or EOSE)
     * - HARD: 5 seconds absolute maximum (MUST be enabled to prevent infinite hang)
     * Relays typically send events in 100-500ms, EOSE within 500ms-1s after that. */
    const guint64 QUIET_TIMEOUT_US = 1000000;  // 1 second of no activity (currently unused)
    const guint64 HARD_TIMEOUT_US = 5000000;   // 5 seconds absolute maximum
    
    while (TRUE) {
        guint64 now = g_get_monotonic_time();
        gboolean any_activity = FALSE;
        
        /* Check cancellation */
        if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) {
            g_message("[GOROUTINE] Cancelled");
            break;
        }
        
        /* Poll all subscriptions */
        for (guint i = 0; i < ctx->subs->len; i++) {
            SubItem *item = (SubItem*)ctx->subs->pdata[i];
            if (!item || !item->sub) continue;
            
            /* Drain events */
            GoChannel *ch_events = nostr_subscription_get_events_channel(item->sub);
            void *msg = NULL;
            
            while (ch_events && go_channel_try_receive(ch_events, &msg) == 0) {
                any_activity = TRUE;
                
                if (msg) {
                    NostrEvent *ev = (NostrEvent*)msg;
                    const char *eid = nostr_event_get_id(ev);
                    const char *pk = nostr_event_get_pubkey(ev);
                    
                    g_message("[GOROUTINE] Received event id=%.16s... pubkey=%.16s...", 
                              eid ? eid : "(null)", pk ? pk : "(null)");
                    
                    if (eid && *eid && !dedup_set_seen((DedupSet*)ctx->dedup, eid)) {
                        char *json = nostr_event_serialize(ev);
                        if (json) {
                            g_mutex_lock((GMutex*)ctx->results_mutex);
                            g_ptr_array_add(ctx->results, json);
                            guint total = ctx->results->len;
                            g_mutex_unlock((GMutex*)ctx->results_mutex);
                            
                            g_message("[GOROUTINE] Added profile (total=%u)", total);
                        }
                    }
                    nostr_event_free(ev);
                    any_activity = TRUE;
                }
                msg = NULL;
            }
            
            /* Check CLOSED (relay rejected/killed subscription) */
            if (!item->eosed) {
                GoChannel *ch_closed = nostr_subscription_get_closed_channel(item->sub);
                char *closed_msg = NULL;
                if (ch_closed && go_channel_try_receive(ch_closed, (void**)&closed_msg) == 0) {
                    g_warning("[GOROUTINE] CLOSED received from %s: %s", 
                              item->relay_url, closed_msg ? closed_msg : "(no reason)");
                    /* Mark as EOSED so we don't wait for it */
                    item->eosed = TRUE;
                    any_activity = TRUE;
                    if (closed_msg) g_free(closed_msg);
                }
            }
            
            /* Check EOSE */
            if (!item->eosed) {
                GoChannel *ch_eose = nostr_subscription_get_eose_channel(item->sub);
                if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
                    g_message("[GOROUTINE] EOSE received from %s", item->relay_url);
                    item->eosed = TRUE;
                    any_activity = TRUE;
                }
            }
        }
        
        /* Check if all SUCCESSFULLY FIRED relays have sent EOSE
         * Note: Failed subscriptions will have NULL sub pointer */
        guint eosed_count = 0;
        guint fired_count = 0;
        for (guint i = 0; i < ctx->subs->len; i++) {
            SubItem *item = (SubItem*)ctx->subs->pdata[i];
            if (item && item->sub) {  /* Only count subscriptions that fired successfully */
                fired_count++;
                if (item->eosed) eosed_count++;
            }
        }
        
        /* Exit if all FIRED subscriptions have sent EOSE (ignore failed ones) */
        if (fired_count > 0 && eosed_count == fired_count) {
            g_message("[GOROUTINE] All %u fired relays sent EOSE (out of %u total), exiting", 
                      eosed_count, ctx->subs->len);
            break;
        }
        
        /* Safety: If no subscriptions fired at all, exit after 1 second */
        if (fired_count == 0 && (now - t_start) > 1000000) {
            g_warning("[GOROUTINE] No subscriptions fired successfully, giving up");
            break;
        }
        
        if (any_activity) {
            t_last_activity = now;
        }
        
        /* Check timeouts - SIMPLIFIED LOGIC */
        guint64 total_elapsed = now - t_start;

        /* Exit condition: hard timeout of 3 seconds (absolute max).
         * Note: All subscriptions hitting EOSE exit earlier.
         */

        if (HARD_TIMEOUT_US > 0 && total_elapsed > HARD_TIMEOUT_US) {
            /* Hard timeout (3s) - give up */
            g_message("[GOROUTINE] Hard timeout after %lums (eosed=%u/%u)", 
                      (unsigned long)(total_elapsed / 1000), eosed_count, ctx->subs->len);
            break;
        }
        
        /* Sleep briefly to avoid tight loop */
        g_usleep(10000); // 10ms (reduced from 50ms for faster EOSE detection)
    }
    
    /* Cleanup filters */
    if (filters) {
        nostr_filters_free(filters);
    }
    
    /* CRITICAL: Async subscription cleanup to prevent goroutine blocking
     * Use nostr_subscription_free_async() with timeout to avoid deadlocks.
     * If cleanup times out, abandon the handle - memory will leak but app won't hang.
     */
    
    /* Step 1: Cancel the background context - signals all subscription workers to exit */
    if (cancel) {
        cancel(bg);
    }
    
    /* Step 2: Free each subscription asynchronously with timeout
     * CRITICAL: Must WAIT for cleanup to complete, not abandon!
     * Abandoning causes thread accumulation as worker threads never get joined.
     */
    for (guint i = 0; i < ctx->subs->len; i++) {
        SubItem *item = (SubItem*)ctx->subs->pdata[i];
        if (item && item->sub) {
            const char *sid = nostr_subscription_get_id(item->sub);
            g_message("[GOROUTINE] Cleanup begin sid=%s relay=%s", sid ? sid : "null", item->relay_url ? item->relay_url : "(null)");

            /* CORRECT ORDER (per MEMORY[5d97ad44]):
             * 1. Close (sends CLOSE message to relay)
             * 2. Unsubscribe (cancels context, signals worker to exit)
             * 3. Wait (ensures worker thread exits)
             * 4. Free (releases resources) */
            
            g_message("[GOROUTINE] Cleanup step: close sid=%s", sid ? sid : "null");
            nostr_subscription_close(item->sub, NULL);
            g_message("[GOROUTINE] Cleanup step: close complete sid=%s", sid ? sid : "null");

            g_message("[GOROUTINE] Cleanup step: unsubscribe sid=%s", sid ? sid : "null");
            nostr_subscription_unsubscribe(item->sub);
            g_message("[GOROUTINE] Cleanup step: unsubscribe complete sid=%s", sid ? sid : "null");

            g_message("[GOROUTINE] Cleanup step: wait sid=%s", sid ? sid : "null");
            nostr_subscription_wait(item->sub);
            g_message("[GOROUTINE] Cleanup step: wait complete sid=%s", sid ? sid : "null");

            g_message("[GOROUTINE] Cleanup step: free sid=%s", sid ? sid : "null");
            nostr_subscription_free(item->sub);
            g_message("[GOROUTINE] Cleanup step: free complete sid=%s", sid ? sid : "null");

            g_message("[GOROUTINE] Cleanup success sid=%s", sid ? sid : "null");
            item->sub = NULL;
        }
        if (item) {
            g_free(item->relay_url);
            g_free(item);
        }
    }
    
    /* Complete the task immediately - cleanup continues in background */
    g_task_return_boolean(ctx->task, TRUE);
    return NULL;
}

/* Cleanup context */
static void fetch_profiles_ctx_free(FetchProfilesCtx *ctx) {
    if (!ctx) return;
    
    /* FALSE because elements were already freed in goroutine cleanup */
    if (ctx->subs) g_ptr_array_free(ctx->subs, FALSE);
    if (ctx->dedup) dedup_set_free((DedupSet*)ctx->dedup);
    if (ctx->results_mutex) {
        g_mutex_clear((GMutex*)ctx->results_mutex);
        g_free(ctx->results_mutex);
    }
    if (ctx->wg) {
        go_wait_group_destroy((GoWaitGroup*)ctx->wg);
        g_free(ctx->wg);
    }
    
    /* Note: results, urls, authors, task are owned by caller */
}

/* Entry point - called from GObject wrapper */
void fetch_profiles_goroutine_start(FetchProfilesCtx *ctx) {
    if (!ctx || !ctx->self_obj) {
        g_critical("PROFILE_FETCH_GOROUTINE: Invalid context!");
        if (ctx && ctx->task) {
            g_task_return_new_error(ctx->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid context");
        }
        return;
    }
    
    g_message("PROFILE_FETCH_GOROUTINE: Starting (authors=%zu relays=%zu)", 
              ctx->author_count, ctx->url_count);
    
    /* Initialize goroutine-specific fields */
    ctx->results = g_ptr_array_new_with_free_func(g_free);
    ctx->subs = g_ptr_array_new();
    ctx->dedup = (void*)dedup_set_new(65536);
    ctx->results_mutex = g_new0(GMutex, 1);
    g_mutex_init((GMutex*)ctx->results_mutex);
    ctx->wg = g_new0(GoWaitGroup, 1);
    go_wait_group_init((GoWaitGroup*)ctx->wg);
    
    /* Launch main goroutine */
    if (go(fetch_profiles_goroutine, ctx) != 0) {
        g_critical("PROFILE_FETCH_GOROUTINE: Failed to launch goroutine!");
        g_task_return_new_error(ctx->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to launch goroutine");
        return;
    }
    
    g_message("PROFILE_FETCH_GOROUTINE: Goroutine launched");
}


void gnostr_simple_pool_fetch_profiles_by_authors_async(GnostrSimplePool *self,
                                                        const char **urls,
                                                        size_t url_count,
                                                        const char *const *authors,
                                                        size_t author_count,
                                                        int limit,
                                                        GCancellable *cancellable,
                                                        GAsyncReadyCallback cb,
                                                        gpointer user_data) {
    g_message("PROFILE_FETCH_GOROUTINE: ENTRY (authors=%zu relays=%zu)", author_count, url_count);
    
    /* DEBUG: Check if self is valid BEFORE g_return_if_fail */
    if (!self) {
        g_critical("PROFILE_FETCH_GOROUTINE: self is NULL!");
        return;
    }
    if (!GNOSTR_IS_SIMPLE_POOL(self)) {
        g_critical("PROFILE_FETCH_GOROUTINE: self is NOT a valid GnostrSimplePool! (type check failed)");
        return;
    }
    
    /* Goroutines are lightweight - no need to serialize fetches */
    g_message("PROFILE_FETCH_GOROUTINE: Starting (authors=%zu relays=%zu)", author_count, url_count);
    
    FetchProfilesCtx *ctx = g_new0(FetchProfilesCtx, 1);
    ctx->self_obj = g_object_ref(G_OBJECT(self));
    
    ctx->urls = g_new0(char*, url_count);
    ctx->url_count = url_count;
    for (size_t i = 0; i < url_count; i++) ctx->urls[i] = g_strdup(urls ? urls[i] : NULL);
    ctx->authors = g_new0(char*, author_count);
    ctx->author_count = author_count;
    for (size_t i = 0; i < author_count; i++) ctx->authors[i] = g_strdup(authors ? authors[i] : NULL);
    ctx->limit = limit;
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    ctx->task = g_task_new(G_OBJECT(self), cancellable, cb, user_data);
    
    /* Set task data WITHOUT destroy notify!
     * The callback must manually free the context after extracting results.
     * If we set a destroy notify, it runs immediately when task completes,
     * freeing the context before the callback can access ctx->results! */
    g_task_set_task_data(ctx->task, ctx, NULL);

    /* NEW: Use goroutine implementation - lightweight, non-blocking */
    fetch_profiles_goroutine_start(ctx);
}

GPtrArray *gnostr_simple_pool_fetch_profiles_by_authors_finish(GnostrSimplePool *self,
                                                               GAsyncResult *res,
                                                               GError **error) {
    g_return_val_if_fail(GNOSTR_IS_SIMPLE_POOL(self), NULL);
    g_return_val_if_fail(g_task_is_valid(res, self), NULL);
    FetchProfilesCtx *ctx = g_task_get_task_data(G_TASK(res));
    if (!ctx) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid task data for fetch_profiles_finish");
        return NULL;
    }
    if (!g_task_propagate_boolean(G_TASK(res), error)) {
        /* Task failed - free context and return NULL */
        fetch_profiles_ctx_free_full(ctx);
        return NULL;
    }
    
    /* Extract results before freeing context */
    GPtrArray *results = g_steal_pointer(&ctx->results);
    
    /* Now free the context - we're done with it */
    fetch_profiles_ctx_free_full(ctx);
    
    return results;
}

/* Check if a relay is connected */
gboolean gnostr_simple_pool_is_relay_connected(GnostrSimplePool *self, const char *url) {
    g_return_val_if_fail(GNOSTR_IS_SIMPLE_POOL(self), FALSE);
    g_return_val_if_fail(url != NULL, FALSE);
    
    if (!self->pool) return FALSE;
    
    /* Check if relay exists in pool by iterating through relays */
    NostrSimplePool *pool = self->pool;
    pthread_mutex_lock(&pool->pool_mutex);
    
    gboolean is_connected = FALSE;
    for (size_t i = 0; i < pool->relay_count; i++) {
        if (pool->relays[i] && pool->relays[i]->url && 
            strcmp(pool->relays[i]->url, url) == 0) {
            /* Found the relay, check if it's connected */
            is_connected = nostr_relay_is_connected(pool->relays[i]);
            break;
        }
    }
    
    pthread_mutex_unlock(&pool->pool_mutex);
    return is_connected;
}

/* Get list of relay URLs currently in the pool */
GPtrArray *gnostr_simple_pool_get_relay_urls(GnostrSimplePool *self) {
    g_return_val_if_fail(GNOSTR_IS_SIMPLE_POOL(self), NULL);
    
    GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
    
    if (!self->pool) return urls;
    
    /* Access the pool's relay list */
    NostrSimplePool *pool = self->pool;
    pthread_mutex_lock(&pool->pool_mutex);
    
    for (size_t i = 0; i < pool->relay_count; i++) {
        if (pool->relays[i] && pool->relays[i]->url) {
            g_ptr_array_add(urls, g_strdup(pool->relays[i]->url));
        }
    }
    
    pthread_mutex_unlock(&pool->pool_mutex);
    
    return urls;
}
