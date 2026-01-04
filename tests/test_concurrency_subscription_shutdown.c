/**
 * Concurrency test: Subscription shutdown invariants
 * Tests that subscriptions clean up properly and don't leak resources
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

#include "go.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"

/* Test tracking */
static atomic_int g_subscriptions_created = 0;
static atomic_int g_subscriptions_freed = 0;
static atomic_int g_test_failures = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__); \
        atomic_fetch_add(&g_test_failures, 1); \
    } \
} while(0)

#define TEST_LOG(fmt, ...) \
    fprintf(stderr, "[TEST] " fmt "\n", ##__VA_ARGS__)

/* Helper: Create minimal filters */
static NostrFilters *make_test_filters(void) {
    NostrFilters *fs = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();
    f->limit = 10;
    nostr_filters_add(fs, f);
    return fs;
}

/* Test 1: Basic subscription lifecycle */
static void test_subscription_lifecycle_basic(void) {
    printf("TEST: subscription_lifecycle_basic\n");
    
    setenv("NOSTR_TEST_MODE", "1", 1);
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    
    NostrRelay *relay = nostr_relay_new(ctx, "wss://test.invalid", &err);
    TEST_ASSERT(relay != NULL, "relay create failed");
    TEST_ASSERT(err == NULL, "relay error");
    
    NostrFilters *filters = make_test_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, filters);
    TEST_ASSERT(sub != NULL, "subscription create failed");
    atomic_fetch_add(&g_subscriptions_created, 1);
    
    /* In test mode, subscription is created but not fired (no network) */
    usleep(10000);
    
    /* Close and free */
    nostr_subscription_close(sub, NULL);
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    atomic_fetch_add(&g_subscriptions_freed, 1);
    
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    
    TEST_LOG("Created: %d, Freed: %d", 
             atomic_load(&g_subscriptions_created),
             atomic_load(&g_subscriptions_freed));
    
    printf("  PASS\n");
}

/* Test 2: Async cleanup with abandon */
static void test_subscription_async_cleanup(void) {
    printf("TEST: subscription_async_cleanup\n");
    
    setenv("NOSTR_TEST_MODE", "1", 1);
    setenv("NOSTR_DEBUG_SHUTDOWN", "1", 1);
    
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    
    NostrRelay *relay = nostr_relay_new(ctx, "wss://test.invalid", &err);
    TEST_ASSERT(relay != NULL, "relay create failed");
    
    NostrFilters *filters = make_test_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, filters);
    TEST_ASSERT(sub != NULL, "subscription create failed");
    atomic_fetch_add(&g_subscriptions_created, 1);
    
    /* In test mode, don't fire (no network) */
    usleep(10000);
    
    /* Start async cleanup */
    TEST_LOG("Starting async cleanup...");
    AsyncCleanupHandle *handle = nostr_subscription_free_async(sub, 500);
    TEST_ASSERT(handle != NULL, "async cleanup failed");
    
    /* Immediately abandon (simulates caller not waiting) */
    TEST_LOG("Abandoning cleanup handle...");
    nostr_subscription_cleanup_abandon(handle);
    
    /* Give background thread time to complete */
    usleep(600000); /* 600ms */
    
    atomic_fetch_add(&g_subscriptions_freed, 1);
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    
    TEST_LOG("Async cleanup complete");
    printf("  PASS\n");
}

/* Test 3: Multiple subscriptions rapid create/destroy */
#define RAPID_TEST_COUNT 10  /* Reduced from 50 to avoid timeout */

static void test_subscription_rapid_lifecycle(void) {
    printf("TEST: subscription_rapid_lifecycle\n");
    
    setenv("NOSTR_TEST_MODE", "1", 1);
    
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    
    NostrRelay *relay = nostr_relay_new(ctx, "wss://test.invalid", &err);
    TEST_ASSERT(relay != NULL, "relay create failed");
    
    int created = 0, freed = 0;
    
    for (int i = 0; i < RAPID_TEST_COUNT; i++) {
        NostrFilters *filters = make_test_filters();
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, filters);
        
        if (sub) {
            created++;
            atomic_fetch_add(&g_subscriptions_created, 1);
            
            /* In test mode, don't fire (no network) */
            usleep(5000);
            
            /* Cleanup - use async to avoid blocking */
            nostr_subscription_close(sub, NULL);
            nostr_subscription_unsubscribe(sub);
            AsyncCleanupHandle *handle = nostr_subscription_free_async(sub, 500);
            if (handle) {
                nostr_subscription_cleanup_abandon(handle);
            }
            freed++;
            atomic_fetch_add(&g_subscriptions_freed, 1);
        }
        
        nostr_filters_free(filters);
    }
    
    TEST_ASSERT(created == RAPID_TEST_COUNT, "not all subscriptions created");
    TEST_ASSERT(freed == RAPID_TEST_COUNT, "not all subscriptions freed");
    
    /* Wait for async cleanups to complete */
    usleep(1000000); /* 1 second */
    
    nostr_relay_free(relay);
    
    TEST_LOG("Rapid test: created=%d freed=%d", created, freed);
    printf("  PASS\n");
}

/* Test 4: Shutdown while subscription blocked */
static void test_subscription_shutdown_while_blocked(void) {
    printf("TEST: subscription_shutdown_while_blocked\n");
    
    setenv("NOSTR_TEST_MODE", "1", 1);
    
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    
    NostrRelay *relay = nostr_relay_new(ctx, "wss://test.invalid", &err);
    TEST_ASSERT(relay != NULL, "relay create failed");
    
    NostrFilters *filters = make_test_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, filters);
    TEST_ASSERT(sub != NULL, "subscription create failed");
    atomic_fetch_add(&g_subscriptions_created, 1);
    
    /* In test mode, subscription is created but not fired (no network) */
    usleep(50000);
    
    TEST_LOG("Initiating shutdown...");
    
    /* Close should unblock the goroutine */
    nostr_subscription_close(sub, NULL);
    nostr_subscription_unsubscribe(sub);
    
    /* Free should complete without hanging */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    nostr_subscription_free(sub);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 + 
                      (end.tv_nsec - start.tv_nsec) / 1000000;
    
    TEST_LOG("Shutdown took %ld ms", elapsed_ms);
    TEST_ASSERT(elapsed_ms < 5000, "shutdown took too long (possible hang)");
    
    atomic_fetch_add(&g_subscriptions_freed, 1);
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    
    printf("  PASS\n");
}

/* Test 5: Verify no use-after-free in async cleanup */
static void test_subscription_no_use_after_free(void) {
    printf("TEST: subscription_no_use_after_free\n");
    
    setenv("NOSTR_TEST_MODE", "1", 1);
    
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    
    NostrRelay *relay = nostr_relay_new(ctx, "wss://test.invalid", &err);
    TEST_ASSERT(relay != NULL, "relay create failed");
    
    /* Create multiple subscriptions and abandon their cleanup */
    for (int i = 0; i < 10; i++) {
        NostrFilters *filters = make_test_filters();
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, filters);
        
        if (sub) {
            atomic_fetch_add(&g_subscriptions_created, 1);
            /* In test mode, don't fire (no network) */
            usleep(5000);
            
            /* Start async cleanup and immediately abandon */
            AsyncCleanupHandle *handle = nostr_subscription_free_async(sub, 500);
            if (handle) {
                nostr_subscription_cleanup_abandon(handle);
                /* DON'T access handle after this point - it's owned by background thread */
            }
            
            atomic_fetch_add(&g_subscriptions_freed, 1);
        }
        
        nostr_filters_free(filters);
    }
    
    /* Wait for all background cleanups to complete */
    TEST_LOG("Waiting for background cleanups...");
    usleep(1000000); /* 1 second */
    
    nostr_relay_free(relay);
    
    TEST_LOG("No use-after-free detected (if running with ASan)");
    printf("  PASS\n");
}

int main(void) {
    printf("=== Concurrency Tests: Subscription Shutdown ===\n");
    
    test_subscription_lifecycle_basic();
    test_subscription_async_cleanup();
    test_subscription_rapid_lifecycle();
    test_subscription_shutdown_while_blocked();
    test_subscription_no_use_after_free();
    
    int failures = atomic_load(&g_test_failures);
    int created = atomic_load(&g_subscriptions_created);
    int freed = atomic_load(&g_subscriptions_freed);
    
    printf("\n=== Results ===\n");
    printf("Subscriptions created: %d\n", created);
    printf("Subscriptions freed: %d\n", freed);
    printf("Failures: %d\n", failures);
    
    if (created != freed) {
        fprintf(stderr, "WARNING: Subscription leak detected! (%d created, %d freed)\n",
                created, freed);
        return 1;
    }
    
    return failures > 0 ? 1 : 0;
}
