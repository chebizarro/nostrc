/* NIP-CAS-0010: bounded seen-event-id cache used for replay rejection.
 *
 * Telemetry events are ephemeral and validly signed, so without a seen-id
 * cache a captured event can be re-published at a consumer indefinitely:
 * log lines and spans are duplicated in the backends and delta metrics are
 * double-counted (security review G4). The cache is an LRU over event ids with
 * a TTL, bounded by a configured maximum, so a flood of unique ids cannot grow
 * it without limit. It mirrors the relay-side defence (G1).
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "otel_internal.h"

/* Hex event ids are 64 characters. */
#define OTEL_SEEN_ID_LEN 64

typedef struct OtelSeenNode {
    char id[OTEL_SEEN_ID_LEN + 1];
    int64_t expires;              /* unix seconds */
    struct OtelSeenNode *lru_prev; /* towards the newest entry */
    struct OtelSeenNode *lru_next; /* towards the oldest entry */
    struct OtelSeenNode *bucket_next;
} OtelSeenNode;

struct NostrOtelSeenCache {
    pthread_mutex_t mu;
    size_t max;
    int64_t ttl;
    size_t count;
    size_t bucket_count;
    OtelSeenNode **buckets;
    OtelSeenNode *newest;
    OtelSeenNode *oldest;
};

static size_t seen_hash(const char *id) {
    /* FNV-1a over the id string. */
    size_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        h ^= (size_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

static void seen_lru_unlink(NostrOtelSeenCache *cache, OtelSeenNode *node) {
    if (node->lru_prev) {
        node->lru_prev->lru_next = node->lru_next;
    } else {
        cache->newest = node->lru_next;
    }
    if (node->lru_next) {
        node->lru_next->lru_prev = node->lru_prev;
    } else {
        cache->oldest = node->lru_prev;
    }
    node->lru_prev = NULL;
    node->lru_next = NULL;
}

static void seen_lru_push_front(NostrOtelSeenCache *cache, OtelSeenNode *node) {
    node->lru_prev = NULL;
    node->lru_next = cache->newest;
    if (cache->newest) cache->newest->lru_prev = node;
    cache->newest = node;
    if (!cache->oldest) cache->oldest = node;
}

/* Removes @node from its bucket chain and frees it. */
static void seen_drop(NostrOtelSeenCache *cache, OtelSeenNode *node) {
    size_t slot = seen_hash(node->id) % cache->bucket_count;
    OtelSeenNode **link = &cache->buckets[slot];
    while (*link) {
        if (*link == node) {
            *link = node->bucket_next;
            break;
        }
        link = &(*link)->bucket_next;
    }
    seen_lru_unlink(cache, node);
    cache->count--;
    free(node);
}

NostrOtelSeenCache *nostr_otel_seen_cache_new(size_t max_entries, int64_t ttl_seconds) {
    if (max_entries == 0) max_entries = NOSTR_OTEL_DEFAULT_SEEN_CACHE_SIZE;
    if (ttl_seconds <= 0) ttl_seconds = 2 * NOSTR_OTEL_DEFAULT_MAX_CLOCK_SKEW_SECONDS;

    NostrOtelSeenCache *cache = calloc(1, sizeof(*cache));
    if (!cache) return NULL;
    cache->max = max_entries;
    cache->ttl = ttl_seconds;
    /* Keep the load factor near 1 without over-allocating for huge caps. */
    cache->bucket_count = max_entries < 1024 ? max_entries + 1 : 1024;
    cache->buckets = calloc(cache->bucket_count, sizeof(*cache->buckets));
    if (!cache->buckets) {
        free(cache);
        return NULL;
    }
    if (pthread_mutex_init(&cache->mu, NULL) != 0) {
        free(cache->buckets);
        free(cache);
        return NULL;
    }
    return cache;
}

void nostr_otel_seen_cache_free(NostrOtelSeenCache *cache) {
    if (!cache) return;
    OtelSeenNode *node = cache->newest;
    while (node) {
        OtelSeenNode *next = node->lru_next;
        free(node);
        node = next;
    }
    pthread_mutex_destroy(&cache->mu);
    free(cache->buckets);
    free(cache);
}

size_t nostr_otel_seen_cache_count(NostrOtelSeenCache *cache) {
    if (!cache) return 0;
    pthread_mutex_lock(&cache->mu);
    size_t count = cache->count;
    pthread_mutex_unlock(&cache->mu);
    return count;
}

int nostr_otel_seen_cache_observe(NostrOtelSeenCache *cache, const char *id, int64_t now,
                                  bool *out_seen) {
    if (!cache || !id || !out_seen) return NOSTR_OTEL_ERR_INVALID_ARG;
    size_t len = strlen(id);
    if (len == 0 || len > OTEL_SEEN_ID_LEN) return NOSTR_OTEL_ERR_INVALID_ARG;
    *out_seen = false;

    pthread_mutex_lock(&cache->mu);

    /* Expire from the oldest end first; every entry shares one TTL, so the LRU
     * order is also the expiry order. */
    while (cache->oldest && cache->oldest->expires <= now) {
        seen_drop(cache, cache->oldest);
    }

    size_t slot = seen_hash(id) % cache->bucket_count;
    for (OtelSeenNode *node = cache->buckets[slot]; node; node = node->bucket_next) {
        if (strcmp(node->id, id) != 0) continue;
        node->expires = now + cache->ttl;
        seen_lru_unlink(cache, node);
        seen_lru_push_front(cache, node);
        *out_seen = true;
        pthread_mutex_unlock(&cache->mu);
        return NOSTR_OTEL_OK;
    }

    /* Bounded: evict the oldest ids until the new one fits. */
    while (cache->count >= cache->max && cache->oldest) {
        seen_drop(cache, cache->oldest);
    }

    OtelSeenNode *node = calloc(1, sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&cache->mu);
        return NOSTR_OTEL_ERR_NOMEM;
    }
    memcpy(node->id, id, len);
    node->id[len] = '\0';
    node->expires = now + cache->ttl;
    node->bucket_next = cache->buckets[slot];
    cache->buckets[slot] = node;
    seen_lru_push_front(cache, node);
    cache->count++;

    pthread_mutex_unlock(&cache->mu);
    return NOSTR_OTEL_OK;
}
