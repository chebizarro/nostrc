/**
 * Profile Fetch Simulation Test
 * Simulates the exact conditions causing thread leaks in profile fetching
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <pthread.h>
#include <glib.h>

#include "go.h"
#include "wait_group.h"
#include "channel.h"
#include "context.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"
#include "nostr-simple-pool.h"

/* Metrics */
static atomic_int g_goroutines_started = 0;
static atomic_int g_goroutines_completed = 0;
static atomic_int g_subs_created = 0;
static atomic_int g_subs_freed = 0;
static atomic_int g_async_abandoned = 0;

#define TEST_LOG(fmt, ...) fprintf(stderr, "[TEST] " fmt "\n", ##__VA_ARGS__)

/* Subscription item */
typedef struct {
    NostrSubscription *sub;
    char *relay_url;
    gboolean eosed;
} SubItem;

/* Profile fetch context */
typedef struct {
    NostrSimplePool *pool;
    char **relay_urls;
    size_t relay_count;
    GoWaitGroup *wg;
    GPtrArray *subs;
    atomic_int *done;
} FetchCtx;

/* Sub-goroutine context */
typedef struct {
    SubItem *item;
    GoWaitGroup *wg;
} SubGoroutineCtx;

/* Goroutine: fire subscription */
static void *sub_goroutine(void *arg) {
    SubGoroutineCtx *ctx = (SubGoroutineCtx*)arg;
    if (!ctx || !ctx->item || !ctx->item->sub) {
        if (ctx && ctx->wg) go_wait_group_done(ctx->wg);
        free(ctx);
        return NULL;
    }
    
    /* In test mode, skip firing (would block waiting for response) */
    if (!getenv("NOSTR_TEST_MODE")) {
        Error *err = NULL;
        nostr_subscription_fire(ctx->item->sub, &err);
        if (err) free_error(err);
    }
    
    go_wait_group_done(ctx->wg);
    free(ctx);
    return NULL;
}

/* Main goroutine: profile fetch */
static void *fetch_goroutine(void *arg) {
    FetchCtx *ctx = (FetchCtx*)arg;
    atomic_fetch_add(&g_goroutines_started, 1);
    
    CancelContextResult cancel_ctx = go_context_with_cancel(go_context_background());
    GoContext *bg = cancel_ctx.context;
    CancelFunc cancel = cancel_ctx.cancel;
    
    /* Create subscriptions */
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();
    int kinds[] = {0};
    nostr_filter_set_kinds(f, kinds, 1);
    nostr_filters_add(filters, f);
    
    for (size_t i = 0; i < ctx->relay_count; i++) {
        NostrRelay *relay = ctx->pool->relays[i];
        if (!relay) continue;
        
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, bg, filters);
        if (!sub) continue;
        
        atomic_fetch_add(&g_subs_created, 1);
        
        SubItem *item = g_new0(SubItem, 1);
        item->sub = sub;
        item->relay_url = g_strdup(ctx->relay_urls[i]);
        item->eosed = FALSE;
        g_ptr_array_add(ctx->subs, item);
        
        /* Launch sub-goroutine to fire subscription */
        SubGoroutineCtx *sub_ctx = g_new0(SubGoroutineCtx, 1);
        sub_ctx->item = item;
        sub_ctx->wg = ctx->wg;
        
        go_wait_group_add(ctx->wg, 1);
        go(sub_goroutine, sub_ctx);
    }
    
    go_wait_group_wait(ctx->wg);
    
    /* In test mode, skip event polling (subscriptions not actually fired) */
    if (!getenv("NOSTR_TEST_MODE")) {
        /* Poll for events with timeout */
        guint64 t_start = g_get_monotonic_time();
        while (g_get_monotonic_time() - t_start < 1000000) { /* 1 second */
            for (guint i = 0; i < ctx->subs->len; i++) {
                SubItem *item = ctx->subs->pdata[i];
                if (!item || !item->sub) continue;
                
                GoChannel *ch = nostr_subscription_get_eose_channel(item->sub);
                if (ch && go_channel_try_receive(ch, NULL) == 0) {
                    item->eosed = TRUE;
                }
            }
            g_usleep(10000);
        }
    }
    
    /* Async cleanup - THIS IS THE CRITICAL SECTION */
    if (cancel) cancel(bg);
    
    for (guint i = 0; i < ctx->subs->len; i++) {
        SubItem *item = ctx->subs->pdata[i];
        if (item && item->sub) {
            AsyncCleanupHandle *h = nostr_subscription_free_async(item->sub, 500);
            if (h) {
                nostr_subscription_cleanup_abandon(h);
                atomic_fetch_add(&g_async_abandoned, 1);
            }
            item->sub = NULL;
            atomic_fetch_add(&g_subs_freed, 1);
        }
        if (item) {
            g_free(item->relay_url);
            g_free(item);
        }
    }
    
    nostr_filters_free(filters);
    atomic_store(ctx->done, 1);
    atomic_fetch_add(&g_goroutines_completed, 1);
    return NULL;
}

/* Test: Single fetch */
static void test_single_fetch(void) {
    printf("\n=== Single Profile Fetch ===\n");
    setenv("NOSTR_TEST_MODE", "1", 1);
    
    NostrSimplePool *pool = nostr_simple_pool_new();
    const char *urls[] = {"wss://test1.invalid", "wss://test2.invalid"};
    
    for (int i = 0; i < 2; i++) {
        Error *err = NULL;
        NostrRelay *r = nostr_relay_new(go_context_background(), urls[i], &err);
        if (r) nostr_simple_pool_add_relay(pool, r);
    }
    
    FetchCtx *ctx = g_new0(FetchCtx, 1);
    ctx->pool = pool;
    ctx->relay_urls = (char**)urls;
    ctx->relay_count = 2;
    ctx->subs = g_ptr_array_new();
    ctx->wg = g_new0(GoWaitGroup, 1);
    go_wait_group_init(ctx->wg);
    ctx->done = malloc(sizeof(atomic_int));
    atomic_init(ctx->done, 0);
    
    go(fetch_goroutine, ctx);
    
    /* Wait for completion */
    for (int i = 0; i < 50; i++) {
        if (atomic_load(ctx->done) == 1) break;
        usleep(100000);
    }
    
    assert(atomic_load(ctx->done) == 1);
    usleep(1000000); /* Wait for async cleanup */
    
    go_wait_group_destroy(ctx->wg);
    g_free(ctx->wg);
    g_ptr_array_free(ctx->subs, FALSE);
    free(ctx->done);
    g_free(ctx);
    nostr_simple_pool_free(pool);
    
    printf("  PASS\n");
}

/* Test: Multiple concurrent fetches */
static void test_concurrent_fetches(void) {
    printf("\n=== Concurrent Profile Fetches ===\n");
    setenv("NOSTR_TEST_MODE", "1", 1);
    
    NostrSimplePool *pool = nostr_simple_pool_new();
    const char *urls[] = {"wss://test1.invalid", "wss://test2.invalid"};
    
    for (int i = 0; i < 2; i++) {
        Error *err = NULL;
        NostrRelay *r = nostr_relay_new(go_context_background(), urls[i], &err);
        if (r) nostr_simple_pool_add_relay(pool, r);
    }
    
    /* Launch 5 concurrent fetches */
    FetchCtx *contexts[5];
    for (int b = 0; b < 5; b++) {
        FetchCtx *ctx = g_new0(FetchCtx, 1);
        ctx->pool = pool;
        ctx->relay_urls = (char**)urls;
        ctx->relay_count = 2;
        ctx->subs = g_ptr_array_new();
        ctx->wg = g_new0(GoWaitGroup, 1);
        go_wait_group_init(ctx->wg);
        ctx->done = malloc(sizeof(atomic_int));
        atomic_init(ctx->done, 0);
        contexts[b] = ctx;
        
        go(fetch_goroutine, ctx);
        usleep(50000); /* 50ms between launches */
    }
    
    /* Wait for all to complete */
    for (int b = 0; b < 5; b++) {
        for (int i = 0; i < 100; i++) {
            if (atomic_load(contexts[b]->done) == 1) break;
            usleep(100000);
        }
        assert(atomic_load(contexts[b]->done) == 1);
    }
    
    usleep(2000000); /* Wait for async cleanup */
    
    /* Cleanup */
    for (int b = 0; b < 5; b++) {
        go_wait_group_destroy(contexts[b]->wg);
        g_free(contexts[b]->wg);
        g_ptr_array_free(contexts[b]->subs, FALSE);
        free(contexts[b]->done);
        g_free(contexts[b]);
    }
    nostr_simple_pool_free(pool);
    
    printf("  PASS\n");
}

int main(void) {
    printf("=== Profile Fetch Simulation Tests ===\n");
    
    test_single_fetch();
    test_concurrent_fetches();
    
    printf("\n=== Results ===\n");
    printf("Goroutines: started=%d completed=%d\n", 
           atomic_load(&g_goroutines_started), atomic_load(&g_goroutines_completed));
    printf("Subscriptions: created=%d freed=%d\n",
           atomic_load(&g_subs_created), atomic_load(&g_subs_freed));
    printf("Async cleanups abandoned: %d\n", atomic_load(&g_async_abandoned));
    
    int started = atomic_load(&g_goroutines_started);
    int completed = atomic_load(&g_goroutines_completed);
    int created = atomic_load(&g_subs_created);
    int freed = atomic_load(&g_subs_freed);
    
    if (started != completed) {
        fprintf(stderr, "ERROR: Goroutine leak! (%d started, %d completed)\n", started, completed);
        return 1;
    }
    
    if (created != freed) {
        fprintf(stderr, "WARNING: Subscription leak! (%d created, %d freed)\n", created, freed);
        return 1;
    }
    
    printf("\nAll tests passed!\n");
    return 0;
}
