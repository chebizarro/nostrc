/* GOROUTINE-BASED PROFILE FETCHING
 * Uses libgo goroutines instead of GLib threads for true async operation
 */

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

/* Cleanup timeout */
#define CLEANUP_TIMEOUT_MS 500

/* Dedup helpers */
typedef struct {
    GHashTable *set;
    GQueue *order;
    gsize cap;
} DedupSet;

static DedupSet *dedup_set_new(gsize cap) {
    DedupSet *d = g_new0(DedupSet, 1);
    d->set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    d->order = g_queue_new();
    d->cap = cap;
    return d;
}

static void dedup_set_free(DedupSet *d) {
    if (!d) return;
    g_hash_table_unref(d->set);
    g_queue_free(d->order);
    g_free(d);
}

static gboolean dedup_set_seen(DedupSet *d, const char *id) {
    if (!d || !id || !*id) return FALSE;
    if (g_hash_table_contains(d->set, id)) return TRUE;
    
    g_hash_table_add(d->set, g_strdup(id));
    g_queue_push_tail(d->order, (gpointer)id);
    
    while (g_hash_table_size(d->set) > d->cap) {
        const char *old = (const char*)g_queue_pop_head(d->order);
        if (old) g_hash_table_remove(d->set, old);
    }
    return FALSE;
}

/* Subscription item */
typedef struct {
    NostrRelay *relay;
    NostrSubscription *sub;
    char *relay_url; // For logging
    gboolean eosed;  // Track if EOSE received
} SubItem;

/* Context matches the one in nostr_simple_pool.c */
typedef struct {
    GObject *self_obj;
    NostrSimplePool *pool;
    char **urls;
    size_t url_count;
    char **authors;
    size_t author_count;
    int limit;
    GCancellable *cancellable;
    GTask *task;
    GPtrArray *results;
    
    /* Goroutine-specific fields */
    void *wg;            /* GoWaitGroup* */
    GPtrArray *subs;
    void *dedup;         /* DedupSet* */
    void *results_mutex; /* GMutex* */
} FetchProfilesCtx;

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
    g_message("[GOROUTINE] Starting subscription for relay %s", item->relay_url);
    
    /* Fire subscription - this may block, but we're in a goroutine so it's OK */
    Error *err = NULL;
    if (!nostr_subscription_fire(item->sub, &err)) {
        g_warning("[GOROUTINE] subscription_fire failed for %s: %s", 
                  item->relay_url, err ? err->message : "unknown");
        if (err) free_error(err);
    } else {
        g_message("[GOROUTINE] Subscription fired for %s", item->relay_url);
    }
    
    /* Signal completion */
    go_wait_group_done(ctx->wg);
    g_free(ctx);
    return NULL;
}

/* Main goroutine that creates subscriptions and polls for events */
static void *fetch_profiles_goroutine(void *arg) {
    FetchProfilesCtx *ctx = (FetchProfilesCtx*)arg;
    if (!ctx || !ctx->pool) {
        g_critical("[GOROUTINE] Invalid context");
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
    int kind0 = 0;
    NostrFilter *f = nostr_filter_new();
    nostr_filter_set_kinds(f, &kind0, 1);
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
        
        /* Get relay from pool */
        NostrRelay *relay = NULL;
        pthread_mutex_lock(&ctx->pool->pool_mutex);
        for (size_t j = 0; j < ctx->pool->relay_count; j++) {
            if (ctx->pool->relays[j] && strcmp(ctx->pool->relays[j]->url, url) == 0) {
                relay = ctx->pool->relays[j];
                break;
            }
        }
        pthread_mutex_unlock(&ctx->pool->pool_mutex);
        
        if (!relay) {
            g_debug("[GOROUTINE] Relay not in pool (skipping): %s", url);
            continue;
        }
        
        if (!nostr_relay_is_connected(relay)) {
            g_warning("[GOROUTINE] Relay not connected (skipping): %s", url);
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
        
        /* Launch goroutine to fire subscription */
        SubGoroutineCtx *sub_ctx = g_new0(SubGoroutineCtx, 1);
        sub_ctx->item = item;
        sub_ctx->wg = (GoWaitGroup*)ctx->wg;
        
        go_wait_group_add((GoWaitGroup*)ctx->wg, 1);
        go(subscription_goroutine, sub_ctx);
    }
    
    g_message("[GOROUTINE] Created %u subscriptions, waiting for fire completion", ctx->subs->len);
    
    /* Wait for all subscriptions to fire */
    go_wait_group_wait((GoWaitGroup*)ctx->wg);
    
    g_message("[GOROUTINE] All subscriptions fired, polling for events");
    
    /* Poll for events with timeout */
    guint64 t_start = g_get_monotonic_time();
    guint64 t_last_activity = t_start;
    guint64 t_last_eose = t_start; // Track last EOSE separately
    guint64 t_last_event = t_start; // Track last event separately
    const guint64 QUIET_TIMEOUT_US = 3000000; // 3 seconds of no activity
    const guint64 EOSE_TIMEOUT_US = 2000000;  // 2 seconds after last EVENT to wait for EOSE
    const guint64 HARD_TIMEOUT_US = 10000000; // 10 seconds absolute max
    
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
                    t_last_event = now; // Update event timestamp
                }
                msg = NULL;
            }
            
            /* Check EOSE */
            if (!item->eosed) {
                GoChannel *ch_eose = nostr_subscription_get_eose_channel(item->sub);
                if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
                    g_message("[GOROUTINE] EOSE received from %s", item->relay_url);
                    item->eosed = TRUE;
                    any_activity = TRUE;
                    t_last_eose = now; // Update EOSE timestamp
                }
            }
        }
        
        /* Check if all relays have sent EOSE */
        guint eosed_count = 0;
        for (guint i = 0; i < ctx->subs->len; i++) {
            SubItem *item = (SubItem*)ctx->subs->pdata[i];
            if (item && item->eosed) eosed_count++;
        }
        
        if (eosed_count == ctx->subs->len) {
            g_message("[GOROUTINE] All %u relays sent EOSE, exiting", eosed_count);
            break;
        }
        
        if (any_activity) {
            t_last_activity = now;
        }
        
        /* Check timeouts */
        guint64 quiet_elapsed = now - t_last_activity;
        guint64 event_elapsed = now - t_last_event;
        guint64 eose_elapsed = now - t_last_eose;
        guint64 total_elapsed = now - t_start;
        
        /* Wait for EOSE after events arrive - relays may send EOSE up to 2s after last event */
        if (event_elapsed < EOSE_TIMEOUT_US) {
            /* Still within EOSE grace period after last event - keep waiting */
        }
        /* Exit if we haven't seen any EOSE in a while (some relays may be slow/broken) */
        else if (eose_elapsed > EOSE_TIMEOUT_US && eosed_count > 0) {
            g_message("[GOROUTINE] EOSE timeout after %lums since last EOSE (eosed=%u/%u, giving up on slow relays)", 
                      (unsigned long)(eose_elapsed / 1000), eosed_count, ctx->subs->len);
            break;
        }
        /* Exit if completely quiet (no events or EOSE) */
        else if (quiet_elapsed > QUIET_TIMEOUT_US) {
            g_message("[GOROUTINE] Quiet timeout after %lums (eosed=%u/%u)", 
                      (unsigned long)(quiet_elapsed / 1000), eosed_count, ctx->subs->len);
            break;
        }
        
        if (total_elapsed > HARD_TIMEOUT_US) {
            g_message("[GOROUTINE] Hard timeout after %lums", (unsigned long)(total_elapsed / 1000));
            break;
        }
        
        /* Sleep briefly to avoid tight loop */
        g_usleep(10000); // 10ms (reduced from 50ms for faster EOSE detection)
    }
    
    guint64 t_end = g_get_monotonic_time();
    g_mutex_lock((GMutex*)ctx->results_mutex);
    guint results_count = ctx->results->len;
    g_mutex_unlock((GMutex*)ctx->results_mutex);
    
    /* Log which relays sent EOSE */
    for (guint i = 0; i < ctx->subs->len; i++) {
        SubItem *item = (SubItem*)ctx->subs->pdata[i];
        if (item) {
            g_message("[GOROUTINE] Relay %s: %s", item->relay_url, 
                      item->eosed ? "EOSE received" : "NO EOSE (timeout)");
        }
    }
    
    g_message("[GOROUTINE] Complete (profiles=%u time=%ldms)", 
              results_count, (long)((t_end - t_start) / 1000));
    
    /* Cleanup subscriptions */
    g_message("[GOROUTINE] Cleaning up %u subscriptions", ctx->subs->len);
    for (guint i = 0; i < ctx->subs->len; i++) {
        SubItem *item = (SubItem*)ctx->subs->pdata[i];
        if (item && item->sub) {
            /* Use async cleanup to avoid blocking */
            AsyncCleanupHandle *handle = nostr_subscription_free_async(item->sub, CLEANUP_TIMEOUT_MS);
            if (handle) {
                nostr_subscription_cleanup_abandon(handle);
            }
        }
        if (item) {
            g_free(item->relay_url);
            g_free(item);
        }
    }
    
    /* Cleanup */
    nostr_filters_free(filters);
    if (cancel) cancel(bg);
    
    /* Return success to main thread via GTask */
    g_message("[GOROUTINE] Scheduling completion callback on main thread");
    g_idle_add(fetch_profiles_complete_ok, ctx->task);
    
    return NULL;
}

/* Completion callback - runs on main thread */
static gboolean fetch_profiles_complete_ok(gpointer data) {
    GTask *task = G_TASK(data);
    g_message("[GOROUTINE] Completion callback firing on main thread");
    g_task_return_boolean(task, TRUE);
    return G_SOURCE_REMOVE;
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
    if (!ctx || !ctx->pool) {
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
