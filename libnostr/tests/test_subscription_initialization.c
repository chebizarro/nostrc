/* Exercise the real constructor with deterministic nonzero malloc contents. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "nostr-subscription.h"
#include "subscription-private.h"

static unsigned poisoned_allocations;
static void *poisoned_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr && size == sizeof(SubscriptionPrivate)) {
        memset(ptr, 0xbe, size);
        poisoned_allocations++;
    }
    return ptr;
}

/* Interpose only this translation unit, never the process allocator. The
 * internal archive supplies the other production objects, not subscription.o. */
#define malloc poisoned_malloc
#include "../src/subscription.c"
#undef malloc

static void assert_ordered_defaults(NostrSubscription *sub) {
    SubscriptionPrivate *p = sub->priv;
    assert(p->ordered_notify == NULL);
    assert(p->ordered_enqueued == 0);
    assert(p->ordered_consumed == 0);
    assert(p->ordered_generation == 0);
    assert(p->ordered_generation_start == 0);
    assert(p->ordered_eose_position == 0);
    assert(p->ordered_eose_time == 0);
    assert(p->ordered_eose_pending == false);
    assert(p->ordered_closed_position == 0);
    assert(p->ordered_closed_pending == false);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("NOSTR_TEST_MODE", "1", 1);
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://subscription-init.invalid", &err);
    assert(relay && !err);
    NostrFilters *filters = nostr_filters_new();
    assert(filters);
    bool prepared = strcmp(argv[1], "prepared") == 0;
    bool shutdown_only = strcmp(argv[1], "shutdown") == 0;
    assert(prepared || shutdown_only || strcmp(argv[1], "direct") == 0);
    NostrSubscription *sub = prepared
        ? nostr_relay_prepare_subscription(relay, NULL, filters)
        : nostr_subscription_new(relay, filters);
    assert(sub && poisoned_allocations == 1);
    if (!shutdown_only) assert_ordered_defaults(sub);
    /* No ordered consumer was enabled. This must not wake or unref a channel
     * fabricated from allocator fill, including on the lifecycle thread. */
    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_unref(relay);
    go_context_unref(ctx);
    return 0;
}
