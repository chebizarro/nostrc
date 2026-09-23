/* nip05_cache.c — broker-owned in-memory NIP-05 resolution cache.
 *
 * Lives in its own translation unit so the unit tests (which don't
 * link the fork/exec-heavy nip05_client.c) can build on any POSIX
 * host. Semantics: separate positive (nh_nip05_cache_put_positive)
 * and negative (put_negative) TTLs. On lookup the entry is dropped
 * lazily when it falls out of its TTL window; the caller sees the
 * miss-sentinel NH_NIP05_ERR_INTERNAL for a cold entry and can then
 * decide whether to fire a fresh resolve. See nostr_nip05.h for the
 * design rationale. */
#define _GNU_SOURCE
#include "nostr_nip05.h"

#include <stdlib.h>
#include <string.h>

typedef struct nip05_cache_entry {
    char address[NH_NIP05_ADDRESS_MAX + 1];
    int64_t stored_at;
    nh_nip05_rc rc;           /* NH_NIP05_OK for a positive hit. */
    nh_nip05_result result;   /* zero when rc != NH_NIP05_OK. */
} nip05_cache_entry;

struct nh_nip05_cache {
    nip05_cache_entry slots[NH_NIP05_CACHE_CAPACITY];
    size_t next_slot; /* round-robin eviction */
    int positive_ttl_seconds;
};

nh_nip05_cache *nh_nip05_cache_new(int positive_ttl_seconds) {
    nh_nip05_cache *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->positive_ttl_seconds = positive_ttl_seconds > 0
                                  ? positive_ttl_seconds
                                  : NH_NIP05_CACHE_POSITIVE_TTL_DEFAULT_SEC;
    return c;
}

void nh_nip05_cache_free(nh_nip05_cache *cache) {
    free(cache);
}

void nh_nip05_cache_set_positive_ttl(nh_nip05_cache *cache, int seconds) {
    if (!cache) return;
    cache->positive_ttl_seconds = seconds > 0
                                       ? seconds
                                       : NH_NIP05_CACHE_POSITIVE_TTL_DEFAULT_SEC;
}

static nip05_cache_entry *find_slot(nh_nip05_cache *cache,
                                    const char *address) {
    for (size_t i = 0; i < NH_NIP05_CACHE_CAPACITY; i++) {
        if (cache->slots[i].address[0] &&
            !strcmp(cache->slots[i].address, address))
            return &cache->slots[i];
    }
    return NULL;
}

static nip05_cache_entry *alloc_slot(nh_nip05_cache *cache,
                                     const char *address) {
    nip05_cache_entry *existing = find_slot(cache, address);
    if (existing) return existing;
    nip05_cache_entry *slot = &cache->slots[cache->next_slot];
    cache->next_slot = (cache->next_slot + 1) % NH_NIP05_CACHE_CAPACITY;
    memset(slot, 0, sizeof *slot);
    size_t n = strlen(address);
    if (n > NH_NIP05_ADDRESS_MAX) n = NH_NIP05_ADDRESS_MAX;
    memcpy(slot->address, address, n);
    slot->address[n] = '\0';
    return slot;
}

nh_nip05_rc nh_nip05_cache_lookup(nh_nip05_cache *cache, const char *address,
                                  int64_t now_seconds,
                                  nh_nip05_result *result_out) {
    if (!cache || !address || !result_out) return NH_NIP05_ERR_INTERNAL;
    memset(result_out, 0, sizeof *result_out);
    nip05_cache_entry *slot = find_slot(cache, address);
    if (!slot) return NH_NIP05_ERR_INTERNAL; /* miss sentinel */
    int64_t age = now_seconds - slot->stored_at;
    if (age < 0) age = 0;
    if (slot->rc == NH_NIP05_OK) {
        if (age > cache->positive_ttl_seconds) {
            memset(slot, 0, sizeof *slot);
            return NH_NIP05_ERR_INTERNAL;
        }
        *result_out = slot->result;
        return NH_NIP05_OK;
    }
    if (age > NH_NIP05_CACHE_NEGATIVE_TTL_SEC) {
        memset(slot, 0, sizeof *slot);
        return NH_NIP05_ERR_INTERNAL;
    }
    return slot->rc;
}

void nh_nip05_cache_put_positive(nh_nip05_cache *cache, const char *address,
                                 int64_t now_seconds,
                                 const nh_nip05_result *result) {
    if (!cache || !address || !result) return;
    nip05_cache_entry *slot = alloc_slot(cache, address);
    slot->stored_at = now_seconds;
    slot->rc = NH_NIP05_OK;
    slot->result = *result;
}

void nh_nip05_cache_put_negative(nh_nip05_cache *cache, const char *address,
                                 int64_t now_seconds, nh_nip05_rc rc) {
    if (!cache || !address) return;
    if (rc == NH_NIP05_OK) return;
    nip05_cache_entry *slot = alloc_slot(cache, address);
    slot->stored_at = now_seconds;
    slot->rc = rc;
    memset(&slot->result, 0, sizeof slot->result);
}
