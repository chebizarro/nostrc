#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "go.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"

/* The final parent-context destructor proves every subscription and the relay
 * released ownership. Counting cleanup requests or waiting a guessed duration
 * does not prove destruction, especially after abandoning a cleanup handle. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool parent_destroyed;
static void (*destroy_background)(void *);

static void parent_destroy(void *ctx) {
    destroy_background(ctx);
    pthread_mutex_lock(&mutex);
    parent_destroyed = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
}

static NostrRelay *make_relay(void) {
    GoContext *parent = go_context_background();
    assert(parent);
    destroy_background = parent->vtable->free;
    parent->vtable->free = parent_destroy;
    parent_destroyed = false;
    Error *error = NULL;
    NostrRelay *relay = nostr_relay_new(parent, "wss://test.invalid", &error);
    assert(relay && !error);
    go_context_unref(parent); /* the relay's child context retains it */
    return relay;
}

static void wait_for_destruction(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5; /* failure watchdog, not the completion condition */
    pthread_mutex_lock(&mutex);
    while (!parent_destroyed) {
        int rc = pthread_cond_timedwait(&changed, &mutex, &deadline);
        if (rc != 0) {
            fprintf(stderr, "parent destruction wait failed: %d\n", rc);
            abort();
        }
    }
    pthread_mutex_unlock(&mutex);
}

static NostrFilters *make_filters(void) {
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *filter = nostr_filter_new();
    assert(filters && filter);
    filter->limit = 10;
    assert(nostr_filters_add(filters, filter));
    nostr_filter_free(filter); /* add moved its contents, not its allocation */
    return filters;
}

static void basic_lifecycle(void) {
    NostrRelay *relay = make_relay();
    NostrFilters *filters = make_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    assert(sub);
    nostr_subscription_close(sub, NULL);
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_unref(relay);
    wait_for_destruction();
}

static void abandon_cleanup(unsigned count, bool close_first) {
    NostrRelay *relay = make_relay();
    for (unsigned i = 0; i < count; i++) {
        NostrFilters *filters = make_filters();
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
        assert(sub);
        if (close_first) {
            nostr_subscription_close(sub, NULL);
            nostr_subscription_unsubscribe(sub);
        }
        AsyncCleanupHandle *handle = nostr_subscription_free_async(sub, 500);
        assert(handle);
        nostr_subscription_cleanup_abandon(handle);
        /* free_async revokes refires synchronously; no caller-owned filters
         * are retained while the worker finishes physical destruction. */
        nostr_filters_free(filters);
    }
    nostr_relay_unref(relay);
    wait_for_destruction();
}

static void shutdown_cancels_lifecycle_waiter(void) {
    NostrRelay *relay = make_relay();
    NostrFilters *filters = make_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    assert(sub);
    /* Cancellation and free must join a worker whether it reached its wait
     * already or is still starting. No sleep makes either ordering safe. */
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_unref(relay);
    wait_for_destruction();
}

static void wait_then_abandon_cleanup(void) {
    NostrRelay *relay = make_relay();
    NostrFilters *filters = make_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    assert(sub);
    AsyncCleanupHandle *handle = nostr_subscription_free_async(sub, 500);
    assert(handle && nostr_subscription_cleanup_wait(handle, 5000));
    nostr_subscription_cleanup_abandon(handle);
    nostr_filters_free(filters);
    nostr_relay_unref(relay);
    wait_for_destruction();
}

int main(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    basic_lifecycle();
    abandon_cleanup(1, false);
    abandon_cleanup(10, true);
    abandon_cleanup(10, false);
    shutdown_cancels_lifecycle_waiter();
    wait_then_abandon_cleanup();
    puts("subscription shutdown: actual ownership released in all cases");
    return 0;
}
