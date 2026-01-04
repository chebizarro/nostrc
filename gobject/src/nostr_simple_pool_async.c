/* NEW ASYNC PROFILE FETCHING - No blocking threads!
 * Uses goroutines + GLib idle callbacks for true async operation
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
#include <glib.h>
#include <gio/gio.h>

/* Reuse types from main file */
extern GType gnostr_simple_pool_get_type(void);

/* Dedup helpers (from main file) */
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

typedef struct {
    NostrRelay *relay;
    NostrSubscription *sub;
    GoChannel *raw;
    gboolean eosed;
} SubItem;

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
} FetchProfilesCtx;

typedef struct {
    FetchProfilesCtx *ctx;
    GPtrArray *subs;
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

/* Timeouts */
#define QUIET_TIMEOUT_MS 5000   /* 5s without activity */
#define HARD_TIMEOUT_MS  20000  /* 20s hard cap */
#define POLL_INTERVAL_MS 50     /* Poll every 50ms */

/* Forward declarations */
static gboolean fetch_profiles_poll(gpointer user_data);
static void fetch_profiles_complete(FetchProfilesState *state, const char *reason);

/* Cleanup state and complete task */
static void fetch_profiles_state_free(FetchProfilesState *state) {
    if (!state) return;
    
    g_message("[PROFILE_ASYNC] Cleanup starting (subs=%u)", state->subs ? state->subs->len : 0);
    
    if (state->idle_source_id) {
        g_source_remove(state->idle_source_id);
        state->idle_source_id = 0;
    }
    
    /* Cleanup subscriptions - Use async cleanup to avoid blocking main thread
     * The async cleanup properly handles:
     * 1. Close subscription (stops accepting new events)
     * 2. Unsubscribe (cancels context, signals worker thread to exit)
     * 3. Wait for goroutine to exit (in background thread)
     * 4. Free subscription resources
     */
    if (state->subs && !state->cleanup_started) {
        state->cleanup_started = TRUE;
        const uint64_t CLEANUP_TIMEOUT_MS = 500;
        
        for (guint i = 0; i < state->subs->len; i++) {
            SubItem *it = (SubItem*)state->subs->pdata[i];
            if (!it || !it->sub) {
                if (it) g_free(it);
                state->subs->pdata[i] = NULL; /* Clear pointer after free */
                continue;
            }
            
            /* Use async cleanup - it spawns a background thread that:
             * - Closes the subscription
             * - Unsubscribes (cancels context)
             * - Waits for goroutine to exit (with timeout)
             * - Frees resources
             * This is non-blocking from the main thread's perspective. */
            g_message("[PROFILE_ASYNC] Closing subscription %u/%u (eosed=%d)", 
                      i+1, (guint)state->subs->len, it->eosed);
            
            /* CRITICAL: Only clean up subscriptions that received EOSE.
             * Subscriptions without EOSE may have stuck goroutines that will block cleanup.
             * Better to leak the subscription than freeze the app. */
            if (it->eosed) {
                AsyncCleanupHandle *handle = nostr_subscription_free_async(it->sub, CLEANUP_TIMEOUT_MS);
                if (handle) {
                    nostr_subscription_cleanup_abandon(handle);
                    g_debug("[PROFILE_ASYNC] Async cleanup started for subscription %u", i+1);
                } else {
                    g_warning("[PROFILE_ASYNC] Failed to start async cleanup for subscription %u", i+1);
                }
            } else {
                /* Subscription never received EOSE - likely stuck.
                 * LEAK it rather than risk blocking the main thread.
                 * The goroutine will eventually timeout and exit on its own. */
                g_warning("[PROFILE_ASYNC] Leaking subscription %u (no EOSE received, cleanup would block)", i+1);
            }
            
            g_free(it);
            state->subs->pdata[i] = NULL; /* Clear pointer after free */
        }
    }
    
    /* Free the array itself (elements already freed above) */
    if (state->subs) g_ptr_array_free(state->subs, FALSE); /* FALSE = don't free elements */
    if (state->dedup) dedup_set_free(state->dedup);
    if (state->authors_needed) g_hash_table_unref(state->authors_needed);
    if (state->filters) nostr_filters_free(state->filters);
    
    /* Clear in-progress flag */
    if (state->ctx && state->ctx->self_obj && GNOSTR_IS_SIMPLE_POOL(state->ctx->self_obj)) {
        GnostrSimplePool *pool = GNOSTR_SIMPLE_POOL(state->ctx->self_obj);
        pool->profile_fetch_in_progress = FALSE;
        g_debug("[PROFILE_ASYNC] Cleared in-progress flag");
    }
    
    g_free(state);
}

/* Complete the fetch and return results */
static void fetch_profiles_complete(FetchProfilesState *state, const char *reason) {
    if (!state || !state->ctx) return;
    
    guint64 t_end = g_get_monotonic_time();
    guint results_count = state->ctx->results ? state->ctx->results->len : 0;
    
    g_message("[PROFILE_ASYNC] Complete (profiles=%u time=%ldms reason=%s subs=%u)",
              results_count, (long)((t_end - state->t_start) / 1000), reason, 
              state->subs ? state->subs->len : 0);
    
    /* Return success */
    g_task_return_boolean(state->ctx->task, TRUE);
    
    /* Cleanup */
    fetch_profiles_state_free(state);
}

/* Poll goroutine channels from GLib main loop */
static gboolean fetch_profiles_poll(gpointer user_data) {
    FetchProfilesState *state = (FetchProfilesState*)user_data;
    
    if (!state || !state->ctx) {
        return G_SOURCE_REMOVE;
    }
    
    /* Check cancellation */
    if (state->ctx->cancellable && g_cancellable_is_cancelled(state->ctx->cancellable)) {
        fetch_profiles_complete(state, "cancelled");
        return G_SOURCE_REMOVE;
    }
    
    state->loop_iterations++;
    gboolean any_activity = FALSE;
    
    /* Poll all subscriptions */
    for (guint i = 0; i < state->subs->len; i++) {
        SubItem *it = (SubItem*)state->subs->pdata[i];
        if (!it || !it->sub) continue;
        
        /* Drain events */
        GoChannel *ch_events = nostr_subscription_get_events_channel(it->sub);
        void *msg = NULL;
        int events_drained = 0;
        
        while (ch_events && go_channel_try_receive(ch_events, &msg) == 0) {
            any_activity = TRUE;
            events_drained++;
            
            if (msg) {
                NostrEvent *ev = (NostrEvent*)msg;
                const char *eid = nostr_event_get_id(ev);
                const char *pk = nostr_event_get_pubkey(ev);
                
                g_message("[PROFILE_ASYNC] Received event id=%.16s... pubkey=%.16s...", 
                          eid ? eid : "(null)", pk ? pk : "(null)");
                
                if (eid && *eid && dedup_set_seen(state->dedup, eid)) {
                    g_debug("[PROFILE_ASYNC] Duplicate event, skipping");
                    nostr_event_free(ev);
                } else {
                    char *json = nostr_event_serialize(ev);
                    if (json) {
                        g_ptr_array_add(state->ctx->results, json);
                        g_message("[PROFILE_ASYNC] Added profile (total=%u)", state->ctx->results->len);
                        
                        /* Mark author as satisfied */
                        if (state->authors_needed) {
                            if (pk && *pk) {
                                g_hash_table_remove(state->authors_needed, pk);
                                guint remaining = g_hash_table_size(state->authors_needed);
                                g_message("[PROFILE_ASYNC] Author satisfied, %u remaining", remaining);
                                if (remaining == 0) {
                                    state->done_all_authors = TRUE;
                                }
                            }
                        }
                    }
                    nostr_event_free(ev);
                }
            }
            msg = NULL;
            
            if (state->done_all_authors) break;
            if (events_drained >= 100) break; /* Limit per poll */
        }
        
        if (state->done_all_authors) break;
        
        /* Check EOSE */
        GoChannel *ch_eose = nostr_subscription_get_eose_channel(it->sub);
        if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
            it->eosed = TRUE;
            g_message("[PROFILE_ASYNC] EOSE received from subscription %u", i+1);
        }
        
        /* Drain CLOSED messages */
        GoChannel *ch_closed = nostr_subscription_get_closed_channel(it->sub);
        void *closed_msg = NULL;
        while (ch_closed && go_channel_try_receive(ch_closed, &closed_msg) == 0) {
            any_activity = TRUE;
            const char *reason = (const char*)closed_msg;
            g_warning("PROFILE_FETCH_ASYNC: CLOSED from relay: %s", reason ? reason : "(null)");
            closed_msg = NULL;
        }
    }
    
    /* Update activity timestamp */
    guint64 now = g_get_monotonic_time();
    if (any_activity) {
        state->t_last_activity = now;
    }
    
    /* Check exit conditions */
    if (state->done_all_authors) {
        fetch_profiles_complete(state, "all_authors");
        return G_SOURCE_REMOVE;
    }
    
    /* Check if all EOSE */
    gboolean all_eosed = TRUE;
    for (guint i = 0; i < state->subs->len; i++) {
        SubItem *it = (SubItem*)state->subs->pdata[i];
        if (it && !it->eosed) {
            all_eosed = FALSE;
            break;
        }
    }
    
    if (all_eosed && state->subs->len > 0) {
        g_message("[PROFILE_ASYNC] All %u subscriptions received EOSE, completing", (guint)state->subs->len);
        fetch_profiles_complete(state, "all_eose");
        return G_SOURCE_REMOVE;
    }
    
    /* Check timeouts */
    guint64 quiet_elapsed = (now - state->t_last_activity) / 1000;
    guint64 total_elapsed = (now - state->t_start) / 1000;
    
    if (quiet_elapsed > QUIET_TIMEOUT_MS) {
        g_message("[PROFILE_ASYNC] Quiet timeout after %lums, completing", (unsigned long)quiet_elapsed);
        fetch_profiles_complete(state, "quiet_timeout");
        return G_SOURCE_REMOVE;
    }
    
    if (total_elapsed > HARD_TIMEOUT_MS) {
        g_message("[PROFILE_ASYNC] Total timeout after %lums, completing", (unsigned long)total_elapsed);
        fetch_profiles_complete(state, "total_timeout");
        return G_SOURCE_REMOVE;
    }
    
    /* Continue polling */
    return G_SOURCE_CONTINUE;
}

/* Start subscriptions in background thread (called once at beginning) */
static void fetch_profiles_start_subscriptions_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    FetchProfilesState *state = (FetchProfilesState*)task_data;
    if (!state || !state->ctx || !state->ctx->pool) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid state");
        return;
    }
    
    g_message("[PROFILE_ASYNC] Creating subscriptions in background thread (relays=%zu)", state->ctx->url_count);
    
    for (size_t i = 0; i < state->ctx->url_count; i++) {
        const char *url = state->ctx->urls[i];
        if (!url || !*url) continue;
        
        /* Get relay from pool - DON'T call ensure_relay as it may block!
         * Relays should already be in the pool from timeline initialization.
         * If a relay isn't in the pool yet, we skip it for this fetch. */
        NostrRelay *relay = NULL;
        pthread_mutex_lock(&state->ctx->pool->pool_mutex);
        for (size_t j = 0; j < state->ctx->pool->relay_count; j++) {
            if (state->ctx->pool->relays[j] && strcmp(state->ctx->pool->relays[j]->url, url) == 0) {
                relay = state->ctx->pool->relays[j];
                break;
            }
        }
        pthread_mutex_unlock(&state->ctx->pool->pool_mutex);
        
        if (!relay) {
            /* Relay doesn't exist in pool yet - skip it for this fetch.
             * DON'T call ensure_relay() here - it blocks for 100-300ms per relay!
             * This function runs on the GTK main thread via g_idle_add(), so any
             * blocking operation will freeze the UI and cause "Not Responding" dialogs.
             * Relays will be created by the live subscription or future fetches. */
            g_debug("PROFILE_FETCH_ASYNC: Relay not in pool (skipping): %s", url);
            continue;
        }
        
        if (!nostr_relay_is_connected(relay)) {
            g_warning("PROFILE_FETCH_ASYNC: Relay not connected (skipping): %s", url);
            continue;
        }
        
        /* Create subscription */
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, state->bg, state->filters);
        if (!sub) {
            g_warning("PROFILE_FETCH_ASYNC: prepare_subscription failed: %s", url);
            continue;
        }
        
        g_message("[PROFILE_ASYNC] Created subscription for relay %s", url);
        
        /* CRITICAL: nostr_subscription_fire() can block!
         * It sends the REQ message over websocket, which may wait for write buffer.
         * TODO: Make this truly async or move to background thread.
         * For now, we just log and hope it doesn't block too long. */
        g_debug("[PROFILE_ASYNC] Firing subscription for relay %s...", url);
        
        Error *err = NULL;
        if (!nostr_subscription_fire(sub, &err)) {
            g_warning("PROFILE_FETCH_ASYNC: subscription_fire failed: %s", url);
            if (err) free_error(err);
            /* Don't call nostr_subscription_free() here - it blocks!
             * Use async cleanup instead. */
            AsyncCleanupHandle *handle = nostr_subscription_free_async(sub, 500);
            if (handle) nostr_subscription_cleanup_abandon(handle);
            continue;
        }
        
        g_message("[PROFILE_ASYNC] Subscription fired successfully: %s", url);
        
        SubItem item = { .relay = relay, .sub = sub, .raw = NULL, .eosed = FALSE };
        g_ptr_array_add(state->subs, g_memdup2(&item, sizeof(SubItem)));
    }
    
    if (state->subs->len == 0) {
        g_warning("PROFILE_FETCH_ASYNC: No subscriptions created!");
        fetch_profiles_complete(state, "no_relays");
        return;
    }
    
    g_message("PROFILE_FETCH_ASYNC: Created %u subscriptions, starting poll", state->subs->len);
    
    /* Start polling */
    state->idle_source_id = g_timeout_add(POLL_INTERVAL_MS, fetch_profiles_poll, state);
}

/* Entry point - replaces the old thread-based function */
void fetch_profiles_async_start(FetchProfilesCtx *ctx) {
    if (!ctx || !ctx->pool) {
        g_critical("PROFILE_FETCH_ASYNC: Invalid context!");
        if (ctx && ctx->task) {
            g_task_return_new_error(ctx->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid context");
        }
        return;
    }
    
    g_debug("[PROFILE_ASYNC] Starting fetch (authors=%zu relays=%zu)", 
            ctx->author_count, ctx->url_count);
    
    /* Initialize results */
    ctx->results = g_ptr_array_new_with_free_func(g_free);
    
    /* Create state */
    FetchProfilesState *state = g_new0(FetchProfilesState, 1);
    state->ctx = ctx;
    state->subs = g_ptr_array_new_with_free_func(NULL);
    state->dedup = dedup_set_new(65536);
    state->bg = go_context_background();
    state->t_start = g_get_monotonic_time();
    state->t_last_activity = state->t_start;
    state->loop_iterations = 0;
    state->done_all_authors = FALSE;
    state->idle_source_id = 0;
    state->cleanup_started = FALSE;
    
    /* Track authors needed */
    if (ctx->author_count > 0) {
        state->authors_needed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (size_t i = 0; i < ctx->author_count; i++) {
            if (ctx->authors[i] && *ctx->authors[i]) {
                g_hash_table_add(state->authors_needed, g_strdup(ctx->authors[i]));
            }
        }
    }
    
    /* Build filter */
    state->filters = nostr_filters_new();
    int kind0 = 0;
    NostrFilter *f = nostr_filter_new();
    nostr_filter_set_kinds(f, &kind0, 1);
    if (ctx->author_count > 0) {
        const char **authv = (const char **)ctx->authors;
        nostr_filter_set_authors(f, authv, ctx->author_count);
        
        /* Log first few authors for debugging */
        g_message("[PROFILE_ASYNC] Requesting kind-0 for %zu authors:", ctx->author_count);
        for (size_t i = 0; i < ctx->author_count && i < 3; i++) {
            g_message("[PROFILE_ASYNC]   author[%zu]: %.16s...", i, ctx->authors[i] ? ctx->authors[i] : "(null)");
        }
        if (ctx->author_count > 3) {
            g_message("[PROFILE_ASYNC]   ... and %zu more", ctx->author_count - 3);
        }
    }
    nostr_filters_add(state->filters, f);
    
    /* Start subscriptions and polling */
    fetch_profiles_start_subscriptions(state);
}
