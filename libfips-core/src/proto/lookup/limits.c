#include "internal.h"
#include <fips/proto/rate_limit.h>
static size_t backoff_find(const fips_lookup_backoff *b, const fips_node_addr *target) {
    for (size_t i = 0; i < b->len; i++) if (!lookup_addr_cmp(b->entries[i].target, *target)) return i;
    return b->len;
}
static void backoff_remove(fips_lookup_backoff *b, size_t i) {
    memmove(b->entries + i, b->entries + i + 1, (--b->len - i) * sizeof(*b->entries));
}
fips_status fips_lookup_backoff_init(fips_lookup_backoff *b, fips_lookup_backoff_entry *entries,
    size_t capacity, uint64_t base_secs, uint64_t max_secs) {
    if (!capacity || capacity > FIPS_MAX_BACKOFF_ENTRIES) return FIPS_ERR_RANGE;
    *b = (fips_lookup_backoff){.entries = entries, .capacity = capacity,
        .base_ms = lookup_millis(base_secs), .max_ms = lookup_millis(max_secs)};
    return FIPS_OK;
}
bool fips_lookup_backoff_is_enabled(const fips_lookup_backoff *b) { return b->base_ms && b->max_ms; }
bool fips_lookup_backoff_is_suppressed(const fips_lookup_backoff *b, const fips_node_addr *target, uint64_t now) {
    size_t i = backoff_find(b, target);
    return i < b->len && now < b->entries[i].suppress_until_ms;
}
uint32_t fips_lookup_backoff_failure_count(const fips_lookup_backoff *b, const fips_node_addr *target) {
    size_t i = backoff_find(b, target);
    return i < b->len ? b->entries[i].failures : 0;
}
void fips_lookup_backoff_prune(fips_lookup_backoff *b, uint64_t now) {
    for (size_t i = 0; i < b->len;)
        if (now >= lookup_add(b->entries[i].suppress_until_ms, b->max_ms)) backoff_remove(b, i);
        else i++;
}
void fips_lookup_backoff_record_failure(fips_lookup_backoff *b, const fips_node_addr *target, uint64_t now) {
    if (!fips_lookup_backoff_is_enabled(b)) return;
    size_t i = backoff_find(b, target);
    if (i == b->len && b->len == b->capacity) {
        fips_lookup_backoff_prune(b, now);
        if (b->len == b->capacity) {
            size_t victim = 0;
            for (size_t j = 1; j < b->len; j++) {
                const fips_lookup_backoff_entry *a = &b->entries[j], *v = &b->entries[victim];
                if (a->suppress_until_ms < v->suppress_until_ms ||
                    (a->suppress_until_ms == v->suppress_until_ms && lookup_addr_cmp(a->target, v->target) < 0)) victim = j;
            }
            backoff_remove(b, victim);
            b->evicted = lookup_add(b->evicted, 1);
        }
        i = b->len;
    }
    if (i == b->len) b->entries[b->len++] = (fips_lookup_backoff_entry){.target = *target};
    fips_lookup_backoff_entry *entry = &b->entries[i];
    if (entry->failures != UINT32_MAX) entry->failures++;
    entry->suppress_until_ms = lookup_add(now, fips_backoff_ms(entry->failures - 1, b->base_ms, b->max_ms));
}
void fips_lookup_backoff_record_success(fips_lookup_backoff *b, const fips_node_addr *target) {
    size_t i = backoff_find(b, target);
    if (i < b->len) backoff_remove(b, i);
}
void fips_lookup_backoff_reset_all(fips_lookup_backoff *b) { b->len = 0; }
uint64_t fips_lookup_backoff_take_evicted(fips_lookup_backoff *b) { uint64_t n = b->evicted; b->evicted = 0; return n; }
fips_status fips_lookup_forward_rate_limiter_init(fips_lookup_forward_rate_limiter *l,
    fips_lookup_forward_entry *entries, size_t capacity, uint64_t min_interval_ms) {
    if (!capacity || capacity > FIPS_MAX_FORWARD_ENTRIES) return FIPS_ERR_RANGE;
    *l = (fips_lookup_forward_rate_limiter){.entries = entries, .capacity = capacity, .min_interval_ms = min_interval_ms};
    return FIPS_OK;
}
static void forward_remove(fips_lookup_forward_rate_limiter *l, size_t i) {
    memmove(l->entries + i, l->entries + i + 1, (--l->len - i) * sizeof(*l->entries));
}
void fips_lookup_forward_prune(fips_lookup_forward_rate_limiter *l, uint64_t now) {
    if (l->has_sweep && now >= l->last_sweep_ms && now - l->last_sweep_ms < FIPS_LOOKUP_FORWARD_SWEEP_INTERVAL_MS) return;
    l->last_sweep_ms = now; l->has_sweep = true;
    size_t kept = 0;
    for (size_t i = 0; i < l->len; i++)
        if (lookup_elapsed(now, l->entries[i].at_ms) < l->min_interval_ms) l->entries[kept++] = l->entries[i];
    l->len = kept;
}
bool fips_lookup_forward_should_forward(fips_lookup_forward_rate_limiter *l,
    const fips_node_addr *from, const fips_node_addr *target, uint64_t now) {
    for (size_t i = 0; i < l->len; i++)
        if (!lookup_addr_cmp(l->entries[i].from, *from) && !lookup_addr_cmp(l->entries[i].target, *target)) {
            if (lookup_elapsed(now, l->entries[i].at_ms) < l->min_interval_ms) return false;
            break;
        }
    fips_lookup_forward_prune(l, now);
    /* Purge this peer's expired entries on admission. No stale index elements
     * are needed in C: insertion order lives in the bounded table itself. */
    for (size_t i = 0; i < l->len;) {
        fips_lookup_forward_entry *entry = &l->entries[i];
        if (!lookup_addr_cmp(entry->from, *from) &&
            (!lookup_addr_cmp(entry->target, *target) || lookup_elapsed(now, entry->at_ms) >= l->min_interval_ms)) forward_remove(l, i);
        else i++;
    }
    if (l->len == l->capacity) {
        size_t best_count = 0, victim = 0;
        for (size_t i = 0; i < l->len; i++) {
            bool seen = false;
            for (size_t j = 0; j < i; j++) if (!lookup_addr_cmp(l->entries[i].from, l->entries[j].from)) { seen = true; break; }
            if (seen) continue;
            size_t n = 0;
            for (size_t j = i; j < l->len; j++) if (!lookup_addr_cmp(l->entries[i].from, l->entries[j].from)) n++;
            if (n > best_count || (n == best_count && lookup_addr_cmp(l->entries[i].from, l->entries[victim].from) > 0)) {
                best_count = n; victim = i;
            }
        }
        forward_remove(l, victim);
        l->evicted = lookup_add(l->evicted, 1);
    }
    l->entries[l->len++] = (fips_lookup_forward_entry){.from = *from, .target = *target, .at_ms = now};
    return true;
}
uint64_t fips_lookup_forward_take_evicted(fips_lookup_forward_rate_limiter *l) { uint64_t n = l->evicted; l->evicted = 0; return n; }
