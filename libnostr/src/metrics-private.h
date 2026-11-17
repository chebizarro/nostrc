#ifndef NOSTR_METRICS_PRIVATE_H
#define NOSTR_METRICS_PRIVATE_H

#include "nostr/metrics.h"
#include <stdatomic.h>

/* Registry handles are process-lived and lookup is idempotent. Concurrent cold
 * callers may all resolve the name, but only publish fully initialized handles.
 * Keep NULL retryable (allocation failure or the metrics-disabled backend). */
static inline nostr_metric_histogram *nostr_metric_histogram_cached(
    _Atomic(nostr_metric_histogram *) *cache, const char *name)
{
    nostr_metric_histogram *h = atomic_load_explicit(cache, memory_order_acquire);
    if (!h) {
        h = nostr_metric_histogram_get(name);
        if (h) atomic_store_explicit(cache, h, memory_order_release);
    }
    return h;
}

/* Block-local handle with a synchronized, call-site-local static cache. */
#define NOSTR_METRIC_HISTOGRAM(handle, name) \
    static _Atomic(nostr_metric_histogram *) handle##_cache = NULL; \
    nostr_metric_histogram *handle = \
        nostr_metric_histogram_cached(&handle##_cache, (name))

#endif
