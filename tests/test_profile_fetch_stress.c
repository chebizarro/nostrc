/**
 * Profile Fetch Stress Test - Monitors thread leaks
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <dirent.h>
#include <glib.h>
#include "go.h"
#include "wait_group.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"
#include "nostr-simple-pool.h"

#define NUM_ITERATIONS 20
#define NUM_RELAYS 4

static atomic_int g_subs_created = 0;
static atomic_int g_subs_freed = 0;

typedef struct {
    NostrSubscription *sub;
    char *url;
    gboolean eosed;
} SubItem;

typedef struct {
    NostrSimplePool *pool;
    char **urls;
    size_t count;
    GoWaitGroup *wg;
    GPtrArray *subs;
    atomic_int *done;
    int iter;
} FetchCtx;

static int get_thread_count(void) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/task", getpid());
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(dir))) if (e->d_name[0] != '.') count++;
    closedir(dir);
    return count;
}

static void *fetch_goroutine(void *arg) {
    FetchCtx *ctx = arg;
    CancelContextResult ccr = go_context_with_cancel(go_context_background());
    
    NostrFilters *fs = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();
    int kinds[] = {0};
    nostr_filter_set_kinds(f, kinds, 1);
    nostr_filters_add(fs, f);
    
    for (size_t i = 0; i < ctx->count; i++) {
        NostrRelay *r = ctx->pool->relays[i];
        if (!r) continue;
        NostrSubscription *s = nostr_relay_prepare_subscription(r, ccr.context, fs);
        if (!s) continue;
        atomic_fetch_add(&g_subs_created, 1);
        
        SubItem *item = g_new0(SubItem, 1);
        item->sub = s;
        item->url = g_strdup(ctx->urls[i]);
        g_ptr_array_add(ctx->subs, item);
    }
    
    usleep(500000);
    
    if (ccr.cancel) ccr.cancel(ccr.context);
    
    for (guint i = 0; i < ctx->subs->len; i++) {
        SubItem *item = ctx->subs->pdata[i];
        if (item && item->sub) {
            AsyncCleanupHandle *h = nostr_subscription_free_async(item->sub, 500);
            if (h) nostr_subscription_cleanup_abandon(h);
            atomic_fetch_add(&g_subs_freed, 1);
        }
        if (item) {
            g_free(item->url);
            g_free(item);
        }
    }
    
    nostr_filters_free(fs);
    atomic_store(ctx->done, 1);
    return NULL;
}

int main(void) {
    printf("=== Profile Fetch Stress Test ===\n");
    setenv("NOSTR_TEST_MODE", "1", 1);
    
    NostrSimplePool *pool = nostr_simple_pool_new();
    const char *urls[] = {"wss://t1.invalid", "wss://t2.invalid", "wss://t3.invalid", "wss://t4.invalid"};
    
    for (int i = 0; i < NUM_RELAYS; i++) {
        Error *e = NULL;
        NostrRelay *r = nostr_relay_new(go_context_background(), urls[i], &e);
        if (r) nostr_simple_pool_add_relay(pool, r);
    }
    
    int initial_threads = get_thread_count();
    printf("Initial threads: %d\n", initial_threads);
    
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        FetchCtx *ctx = g_new0(FetchCtx, 1);
        ctx->pool = pool;
        ctx->urls = (char**)urls;
        ctx->count = NUM_RELAYS;
        ctx->subs = g_ptr_array_new();
        ctx->wg = g_new0(GoWaitGroup, 1);
        go_wait_group_init(ctx->wg);
        ctx->done = malloc(sizeof(atomic_int));
        atomic_init(ctx->done, 0);
        ctx->iter = iter;
        
        go(fetch_goroutine, ctx);
        
        for (int i = 0; i < 100; i++) {
            if (atomic_load(ctx->done) == 1) break;
            usleep(100000);
        }
        
        usleep(600000);
        
        int threads = get_thread_count();
        if ((iter + 1) % 5 == 0) {
            printf("Iter %d/%d: threads=%d (+%d) subs=%d/%d\n", 
                   iter+1, NUM_ITERATIONS, threads, threads-initial_threads,
                   atomic_load(&g_subs_created), atomic_load(&g_subs_freed));
        }
        
        go_wait_group_destroy(ctx->wg);
        g_free(ctx->wg);
        g_ptr_array_free(ctx->subs, FALSE);
        free(ctx->done);
        g_free(ctx);
        usleep(200000);
    }
    
    sleep(2);
    int final_threads = get_thread_count();
    
    nostr_simple_pool_free(pool);
    
    printf("\n=== Results ===\n");
    printf("Threads: initial=%d final=%d delta=%+d\n", 
           initial_threads, final_threads, final_threads - initial_threads);
    printf("Subscriptions: created=%d freed=%d\n",
           atomic_load(&g_subs_created), atomic_load(&g_subs_freed));
    
    if (final_threads > initial_threads + 10) {
        printf("ERROR: Thread leak detected!\n");
        return 1;
    }
    
    return 0;
}
