#include "nostr-simple-pool.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "channel.h"
#include "context.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Function to create a SimplePool
NostrSimplePool *nostr_simple_pool_new(void) {
    NostrSimplePool *pool = (NostrSimplePool *)malloc(sizeof(NostrSimplePool));
    if (!pool)
        return NULL;

    pool->relays = NULL;
    pool->relay_count = 0;
    pthread_mutex_init(&pool->pool_mutex, NULL);
    pool->auth_handler = NULL;
    pool->event_middleware = NULL;
    pool->signature_checker = NULL;
    pool->running = false;
    pool->subs = NULL;
    pool->subs_count = 0;
    pool->dedup_unique = false;
    pool->dedup_cap = 65536; /* align with GObject reference scale */
    pool->dedup_ring = NULL;
    pool->dedup_len = 0;
    pool->dedup_head = 0;

    return pool;
}

// Function to free a SimplePool
void nostr_simple_pool_free(NostrSimplePool *pool) {
    if (pool) {
        /* Ensure stopped */
        if (pool->running) {
            pool->running = false;
            pthread_join(pool->thread, NULL);
        }
        /* Close subscriptions */
        if (pool->subs) {
            for (size_t i = 0; i < pool->subs_count; i++) {
                NostrSubscription *sub = pool->subs[i];
                if (sub) {
                    nostr_subscription_close(sub, NULL);
                    nostr_subscription_free(sub);
                }
            }
            free(pool->subs);
        }
        /* Free dedup ring */
        if (pool->dedup_ring) {
            for (size_t i = 0; i < pool->dedup_len; i++) free(pool->dedup_ring[i]);
            free(pool->dedup_ring);
        }
        if (pool->filters_shared) {
            nostr_filters_free(pool->filters_shared);
            pool->filters_shared = NULL;
        }
        for (size_t i = 0; i < pool->relay_count; i++) {
            nostr_relay_free(pool->relays[i]);
        }
        free(pool->relays);
        pthread_mutex_destroy(&pool->pool_mutex);
        free(pool);
    }
}

// Function to ensure a relay connection
void nostr_simple_pool_ensure_relay(NostrSimplePool *pool, const char *url) {
    pthread_mutex_lock(&pool->pool_mutex);

    for (size_t i = 0; i < pool->relay_count; i++) {
        if (strcmp(pool->relays[i]->url, url) == 0) {
            if (nostr_relay_is_connected(pool->relays[i])) {
                pthread_mutex_unlock(&pool->pool_mutex);
                return;
            } else {
                // reconnect if not connected
                nostr_relay_disconnect(pool->relays[i]);
                Error *err = NULL;
                (void)nostr_relay_connect(pool->relays[i], &err);
                if (err) free_error(err);
                pthread_mutex_unlock(&pool->pool_mutex);
                return;
            }
        }
    }

    // If relay not found, create and connect a new one
    GoContext *ctx = go_context_background();
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, url, &err);
    if (!relay) {
        if (err) free_error(err);
        pthread_mutex_unlock(&pool->pool_mutex);
        return;
    }
    (void)nostr_relay_connect(relay, &err);
    if (err) free_error(err);

    pool->relays = (NostrRelay **)realloc(pool->relays, (pool->relay_count + 1) * sizeof(NostrRelay *));
    pool->relays[pool->relay_count++] = relay;

    pthread_mutex_unlock(&pool->pool_mutex);
}

// Thread function for SimplePool
static int pool_seen(NostrSimplePool *pool, const char *id) {
    if (!pool->dedup_unique || !id || !*id) return 0;
    /* Linear scan in ring buffer (small cap) */
    for (size_t i = 0; i < pool->dedup_len; i++) {
        size_t idx = (pool->dedup_head + pool->dedup_len - 1 - i) % (pool->dedup_cap ? pool->dedup_cap : 1);
        const char *v = pool->dedup_ring ? pool->dedup_ring[idx] : NULL;
        if (v && strcmp(v, id) == 0) return 1;
    }
    /* Insert */
    if (!pool->dedup_ring && pool->dedup_cap > 0) {
        pool->dedup_ring = (char **)calloc(pool->dedup_cap, sizeof(char *));
        pool->dedup_len = 0;
        pool->dedup_head = 0;
    }
    if (pool->dedup_cap > 0) {
        if (pool->dedup_len < pool->dedup_cap) {
            size_t pos = (pool->dedup_head + pool->dedup_len) % pool->dedup_cap;
            pool->dedup_ring[pos] = strdup(id);
            pool->dedup_len++;
        } else {
            /* evict head */
            if (pool->dedup_ring[pool->dedup_head]) free(pool->dedup_ring[pool->dedup_head]);
            pool->dedup_ring[pool->dedup_head] = strdup(id);
            pool->dedup_head = (pool->dedup_head + 1) % pool->dedup_cap;
        }
    }
    return 0;
}

void *simple_pool_thread_func(void *arg) {
    NostrSimplePool *pool = (NostrSimplePool *)arg;

    // Adaptive backoff between 2ms and 50ms
    unsigned backoff_us = 2000;
    const unsigned backoff_min = 2000;
    const unsigned backoff_max = 50000;
    size_t rr_start = 0; // round-robin start index for fairness

    while (pool->running) {
        // Snapshot subs under lock to avoid races while iterating
        pthread_mutex_lock(&pool->pool_mutex);
        size_t local_count = pool->subs_count;
        NostrSubscription **local_subs = NULL;
        if (local_count > 0 && pool->subs) {
            local_subs = (NostrSubscription **)malloc(local_count * sizeof(NostrSubscription *));
            if (local_subs) memcpy(local_subs, pool->subs, local_count * sizeof(NostrSubscription *));
        }
        pthread_mutex_unlock(&pool->pool_mutex);

        // Batch to deliver to middleware after scan
        typedef struct { NostrEvent *event; NostrRelay *relay; } BatchItem;
        BatchItem *batch = NULL; size_t batch_len = 0; size_t batch_cap = 0;

        int did_work = 0; // any events processed or subs pruned

        if (local_subs && local_count > 0) {
            // Fairness: start at rotating index
            size_t start = rr_start % local_count;
            for (size_t ofs = 0; ofs < local_count; ofs++) {
                size_t i = (start + ofs) % local_count;
                NostrSubscription *sub = local_subs[i];
                if (!sub) continue;
                GoChannel *ch = nostr_subscription_get_events_channel(sub);
                if (!ch) continue;
                void *msg = NULL;
                int spins = 0;
                while (go_channel_try_receive(ch, &msg) == 0 && spins++ < 32) {
                    if (!msg) break;
                    NostrEvent *ev = (NostrEvent *)msg;
                    const char *eid = nostr_event_get_id(ev);
                    if (pool_seen(pool, eid)) {
                        nostr_event_free(ev);
                    } else {
                        // Add to batch (or free if no middleware)
                        if (pool->event_middleware) {
                            if (batch_len == batch_cap) {
                                size_t new_cap = batch_cap ? batch_cap * 2 : 64;
                                BatchItem *nb = (BatchItem *)realloc(batch, new_cap * sizeof(BatchItem));
                                if (!nb) { nostr_event_free(ev); break; }
                                batch = nb; batch_cap = new_cap;
                            }
                            batch[batch_len++] = (BatchItem){ .event = ev, .relay = nostr_subscription_get_relay(sub) };
                        } else {
                            // default: free to avoid leak; real users should set middleware
                            nostr_event_free(ev);
                        }
                        did_work = 1;
                    }
                    msg = NULL;
                }

                // Opportunistically prune CLOSED subscriptions (non-blocking)
                GoChannel *ch_closed = nostr_subscription_get_closed_channel(sub);
                void *closed_msg = NULL;
                if (ch_closed && go_channel_try_receive(ch_closed, &closed_msg) == 0) {
                    pthread_mutex_lock(&pool->pool_mutex);
                    for (size_t j = 0; j < pool->subs_count; j++) {
                        if (pool->subs[j] == sub) {
                            nostr_subscription_close(sub, NULL);
                            nostr_subscription_free(sub);
                            for (size_t k = j + 1; k < pool->subs_count; k++) pool->subs[k - 1] = pool->subs[k];
                            pool->subs_count--;
                            if (pool->subs_count == 0) { free(pool->subs); pool->subs = NULL; }
                            break;
                        }
                    }
                    pthread_mutex_unlock(&pool->pool_mutex);
                    did_work = 1;
                }
            }
            rr_start = (start + 1) % local_count;
        }

        // Deliver batch to middleware outside of any locks
        if (batch_len > 0 && pool->event_middleware) {
            for (size_t i = 0; i < batch_len; i++) {
                NostrIncomingEvent incoming = { .event = batch[i].event, .relay = batch[i].relay };
                pool->event_middleware(&incoming);
            }
        }
        // If no middleware, events were already freed on receipt
        free(batch);
        free(local_subs);

        // Adaptive sleep/backoff
        if (did_work) {
            backoff_us = backoff_min;
        } else {
            backoff_us = (backoff_us < backoff_max) ? (backoff_us * 2) : backoff_max;
        }
        usleep(backoff_us);
    }

    return NULL;
}

// Function to start the SimplePool
void nostr_simple_pool_start(NostrSimplePool *pool) {
    pool->running = true;
    pthread_create(&pool->thread, NULL, simple_pool_thread_func, (void *)pool);
}

// Function to stop the SimplePool
void nostr_simple_pool_stop(NostrSimplePool *pool) {
    if (!pool) return;
    pool->running = false;
    if (pool->thread) pthread_join(pool->thread, NULL);
    // On stop: unsubscribe/close/free any active subs and clear list
    pthread_mutex_lock(&pool->pool_mutex);
    if (pool->subs) {
        for (size_t i = 0; i < pool->subs_count; i++) {
            NostrSubscription *sub = pool->subs[i];
            if (!sub) continue;
            nostr_subscription_unsubscribe(sub);
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
        }
        free(pool->subs);
        pool->subs = NULL;
        pool->subs_count = 0;
    }
    // Optionally disconnect relays on stop when NOSTR_SIMPLE_POOL_DISCONNECT=1
    const char *disc = getenv("NOSTR_SIMPLE_POOL_DISCONNECT");
    int do_disc = (disc && *disc && strcmp(disc, "0") != 0) ? 1 : 0;
    if (do_disc && pool->relays) {
        for (size_t i = 0; i < pool->relay_count; i++) {
            if (pool->relays[i]) nostr_relay_disconnect(pool->relays[i]);
        }
    }
    pthread_mutex_unlock(&pool->pool_mutex);
}

// Function to subscribe to multiple relays
void nostr_simple_pool_subscribe(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilters filters, bool unique) {
    if (!pool || !urls || url_count == 0) return;
    pool->dedup_unique = unique;
    // Ensure relays exist/connected
    for (size_t i = 0; i < url_count; i++) {
        if (urls[i] && *urls[i]) nostr_simple_pool_ensure_relay(pool, urls[i]);
    }
    // Build deep-copied shared filters object for subscriptions
    NostrFilters *owned = nostr_filters_new();
    if (owned) {
        for (size_t i = 0; i < filters.count; i++) {
            NostrFilter *dup = nostr_filter_copy(&filters.filters[i]);
            if (dup) {
                /* Move contents out of dup into vector, then free shell */
                NostrFilter tmp = *dup;
                free(dup);
                (void)nostr_filters_add(owned, &tmp);
            }
        }
    }
    // Replace pool->filters_shared
    if (pool->filters_shared) {
        nostr_filters_free(pool->filters_shared);
    }
    pool->filters_shared = owned;
    // Create and fire subscriptions per relay
    GoContext *bg = go_context_background();
    pthread_mutex_lock(&pool->pool_mutex);
    for (size_t i = 0; i < pool->relay_count; i++) {
        NostrRelay *relay = pool->relays[i];
        if (!relay) continue;
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, bg, pool->filters_shared);
        if (!sub) continue;
        Error *err = NULL;
        if (!nostr_subscription_fire(sub, &err)) {
            if (err) free_error(err);
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
            continue;
        }
        pool->subs = (NostrSubscription **)realloc(pool->subs, (pool->subs_count + 1) * sizeof(NostrSubscription *));
        pool->subs[pool->subs_count++] = sub;
    }
    pthread_mutex_unlock(&pool->pool_mutex);
}

// Function to query a single event from multiple relays
void nostr_simple_pool_query_single(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilter filter) {
    if (!pool || !urls || url_count == 0) return;

    // Feature gate: ONESHOT behavior (block briefly per URL until first event or EOSE), else delegate to subscribe
    const char *oneshot_env = getenv("NOSTR_SIMPLE_POOL_ONESHOT");
    int oneshot = (oneshot_env && *oneshot_env && strcmp(oneshot_env, "0") != 0) ? 1 : 0;
    unsigned timeout_ms = 1000; // default 1s per URL
    const char *to_env = getenv("NOSTR_SIMPLE_POOL_Q_TIMEOUT_MS");
    if (to_env && *to_env) { unsigned v = (unsigned)atoi(to_env); if (v > 0) timeout_ms = v; }

    if (!oneshot) {
        // Ensure relays exist/connected
        for (size_t i = 0; i < url_count; i++) {
            if (urls[i] && *urls[i]) nostr_simple_pool_ensure_relay(pool, urls[i]);
        }
        // Wrap the single filter into a Filters container and delegate to subscribe with de-dup enabled.
        NostrFilters one;
        memset(&one, 0, sizeof(one));
        one.count = 1;
        one.filters = (NostrFilter *)calloc(1, sizeof(NostrFilter));
        if (!one.filters) return;
        NostrFilter *dup = nostr_filter_copy(&filter);
        if (dup) { one.filters[0] = *dup; free(dup); } else { memset(&one.filters[0], 0, sizeof(NostrFilter)); }
        nostr_simple_pool_subscribe(pool, urls, url_count, one, true /* unique/dedup */);
        free(one.filters);
        return;
    }

    // ONESHOT path: create ephemeral subscriptions, deliver first event via middleware, then close.
    GoContext *bg = go_context_background();
    for (size_t i = 0; i < url_count; i++) {
        const char *url = urls[i];
        if (!url || !*url) continue;
        nostr_simple_pool_ensure_relay(pool, url);
        // Find the relay object
        NostrRelay *relay = NULL;
        pthread_mutex_lock(&pool->pool_mutex);
        for (size_t r = 0; r < pool->relay_count; r++) {
            if (pool->relays[r] && strcmp(pool->relays[r]->url, url) == 0) { relay = pool->relays[r]; break; }
        }
        pthread_mutex_unlock(&pool->pool_mutex);
        if (!relay) continue;

        // Build a filters object with the single filter
        NostrFilters *fs = nostr_filters_new();
        if (!fs) continue;
        NostrFilter *dup = nostr_filter_copy(&filter);
        if (dup) { NostrFilter tmp = *dup; free(dup); (void)nostr_filters_add(fs, &tmp); } else { NostrFilter tmp = {0}; (void)nostr_filters_add(fs, &tmp); }

        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, bg, fs);
        if (!sub) { nostr_filters_free(fs); continue; }
        Error *err = NULL;
        if (!nostr_subscription_fire(sub, &err)) {
            if (err) free_error(err);
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
            // fs is owned by sub now if fire succeeded; since it failed, free fs
            nostr_filters_free(fs);
            continue;
        }

        // Wait for first event or EOSE/timeout
        GoChannel *ch_ev = nostr_subscription_get_events_channel(sub);
        GoChannel *ch_eose = nostr_subscription_get_eose_channel(sub);
        const unsigned step_us = 5 * 1000; // 5ms polling
        unsigned waited = 0;
        int done = 0;
        while (!done && waited < timeout_ms) {
            void *msg = NULL;
            if (ch_ev && go_channel_try_receive(ch_ev, &msg) == 0 && msg) {
                NostrEvent *ev = (NostrEvent *)msg;
                if (!pool_seen(pool, nostr_event_get_id(ev))) {
                    if (pool->event_middleware) {
                        NostrIncomingEvent incoming = { .event = ev, .relay = relay };
                        pool->event_middleware(&incoming);
                    } else {
                        nostr_event_free(ev);
                    }
                } else {
                    nostr_event_free(ev);
                }
                done = 1; // first event consumed
                break;
            }
            // Check EOSE: stop if seen
            if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
                break;
            }
            usleep(step_us);
            waited += (step_us / 1000);
        }

        // Close/free ephemeral subscription
        nostr_subscription_close(sub, NULL);
        nostr_subscription_free(sub);
        // fs freed by sub free via nostr_subscription_set_filters ownership
    }
}
