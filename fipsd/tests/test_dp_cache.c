#ifdef NDEBUG
#undef NDEBUG /* Correctness assertions run in optimized builds too. */
#endif
/* The dataplane's published coordinate cache (nostrc-x345n). Over random
 * cache operation sequences (hint, funded, stated and verified inserts,
 * touches, demotes, removes, expiry and purges, eviction under pressure,
 * invalidations, clears, re-inserts of evicted keys), a view updated
 * incrementally after one or several operations must equal one built afresh
 * from the cache: the same rows, and every lookup answering as a linear scan
 * of the rows and as the core's own get, for present, absent and expired
 * addresses, at and just past each expiry. A single touch rebuilds one part. */
#include "node/handlers/internal.h"
#include "fakes/host.h"
#include <assert.h>
#include <stdio.h>

enum { MAX = 1100, SLOTS = FIPS_COORD_CACHE_SLOTS(MAX) + 8, WORDS = FIPS_COORD_CACHE_INDEX_WORDS(SLOTS), POOL = 2400 };
static fips_coord_cache cache;
static uint32_t rng = 20261119u;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static fips_node_addr pool_addr(uint32_t i)
{
    fips_node_addr a = {{0}};
    memcpy(a.bytes, &i, sizeof i); a.bytes[15] = 0x7c;
    return a;
}
static fips_cached_coord coords_for(const fips_node_addr *a, uint8_t root, size_t len)
{
    uint8_t path[8][16];
    memcpy(path[0], a->bytes, 16);
    for (size_t i = 1; i + 1 < len; ++i) { memset(path[i], 0, 16); path[i][0] = (uint8_t)(0xd0 + rnd() % 4); path[i][1] = (uint8_t)i; }
    memset(path[len - 1], 0, 16); path[len - 1][0] = root;
    fips_cached_coord c;
    assert(fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])path, len) == FIPS_OK);
    for (size_t i = 0; i < len; ++i) { c.entries[i].sequence = rnd() % 3; c.entries[i].timestamp = rnd() % 5; }
    return c;
}
static void step(uint64_t now)
{
    uint32_t r = rnd();
    fips_node_addr a = pool_addr(r % POOL);
    fips_cached_coord c = coords_for(&a, r % 29 == 0 ? 8 : 9, 2 + r % 6);
    fips_instant at = fips_test_instant(now);
    fips_hint_outcome o; fips_placed placed;
    switch (r % 16) {
    case 0: case 1: case 2: (void)fips_coord_cache_insert_with_ttl(&cache, a.bytes, &c, at, 50 + r % 30000, &o); break;
    case 3: (void)fips_coord_cache_insert(&cache, a.bytes, &c, at, &o); break;
    case 4: (void)fips_coord_cache_insert_funded(&cache, a.bytes, &c, at, r % 3 ? FIPS_FUNDING_UPTO(r % 40) : FIPS_FUNDING_UNCHARGED, &placed); break;
    case 5: (void)fips_coord_cache_insert_stated(&cache, a.bytes, &c, at, FIPS_FUNDING_UPTO(r % 60), &placed); break;
    case 6: (void)fips_coord_cache_insert_verified(&cache, a.bytes, &c, at); break;
    case 7: (void)fips_coord_cache_insert_verified_with_path_mtu(&cache, a.bytes, &c, at, (uint16_t)(1280 + r % 9)); break;
    case 8: case 9: case 10: (void)fips_coord_cache_get_and_touch(&cache, a.bytes, at); break;
    case 11: (void)fips_coord_cache_demote(&cache, a.bytes); break;
    case 12: if (r % 2) { (void)fips_coord_cache_remove(&cache, a.bytes, NULL); } break;
    case 13: if (r % 5 == 0) { (void)fips_coord_cache_purge_expired(&cache, at); } break;
    case 14: fips_coord_cache_clear_path_mtu(&cache, a.bytes); break;
    default:
        if (r % 97 == 0) { uint8_t via[16] = {(uint8_t)(0xd0 + r % 4), 1}; (void)fips_coord_cache_invalidate_via_node(&cache, via); }
        else if (r % 89 == 0) { uint8_t root[16] = {9}; (void)fips_coord_cache_invalidate_other_roots(&cache, root); }
        else if (r % 1999 == 0) fips_coord_cache_clear(&cache);
        break;
    }
}
static bool same_row(const fipsd_dp_cached *x, const fipsd_dp_cached *y)
{
    return x && y && !memcmp(x->addr.bytes, y->addr.bytes, 16) && x->expires == y->expires && x->len == y->len &&
        x->slot == y->slot && !memcmp(x->coords, y->coords, x->len * sizeof *x->coords);
}
typedef struct { const fipsd_dp_cache *other; size_t rows; } cross;
static bool found_in_other(void *ctx, const fipsd_dp_cached *row)
{
    cross *c = ctx; ++c->rows;
    assert(same_row(row, fipsd_dp_cache_find(c->other, &row->addr, 0))); /* at 0 nothing has expired */
    return true;
}
typedef struct { const fips_node_addr *addr; const fipsd_dp_cached *row; size_t matches; } scan;
static bool scan_row(void *ctx, const fipsd_dp_cached *row)
{
    scan *s = ctx;
    if (!memcmp(row->addr.bytes, s->addr->bytes, 16)) { s->row = row; ++s->matches; }
    return true;
}
/* What forwarding did before the index, with upstream's expiry. */
static const fipsd_dp_cached *linear(const fipsd_dp_cache *s, const fips_node_addr *a, uint64_t now)
{
    scan sc = {a, NULL, 0};
    fipsd_dp_cache_walk(s, scan_row, &sc);
    assert(sc.matches <= 1);
    return sc.row && now <= sc.row->expires ? sc.row : NULL;
}
static size_t present, absent, expired;
static void lookups_agree(const fipsd_dp_cache *s, const fips_node_addr *a, uint64_t now, bool scan_too)
{
    const fipsd_dp_cached *got = fipsd_dp_cache_find(s, a, now);
    if (scan_too) assert(got == linear(s, a, now));
    const fips_cached_coord *core = fips_coord_cache_get(&cache, a->bytes, fips_test_instant(now));
    assert(!got == !core);
    if (got) {
        assert(got->len == core->len && !memcmp(got->coords, core->entries, got->len * sizeof *got->coords));
        ++present;
    } else if (fips_coord_cache_get_entry(&cache, a->bytes)) ++expired;
    else ++absent;
}
static void views_agree(const fipsd_dp_cache_view *inc, uint64_t now)
{
    fipsd_dp_cache_view fresh; fips_fixed_entropy e = {0x42};
    fipsd_dp_cache_view_init(&fresh, fips_fixed_entropy_adapter(&e));
    assert(fipsd_dp_cache_view_update(&fresh, &cache));
    assert(inc->generation == fips_coord_cache_generation(&cache));
    size_t inc_rows = inc->section ? inc->section->count : 0, fresh_rows = fresh.section ? fresh.section->count : 0;
    assert(inc_rows == cache.len && fresh_rows == cache.len);
    cross a = {fresh.section, 0}, b = {inc->section, 0};
    fipsd_dp_cache_walk(inc->section, found_in_other, &a);
    fipsd_dp_cache_walk(fresh.section, found_in_other, &b);
    assert(a.rows == inc_rows && b.rows == fresh_rows);
    for (size_t i = 0; i < cache.slot_capacity; ++i) {
        const fips_coord_slot *slot = &cache.slots[i];
        if (!slot->used) continue;
        fips_node_addr addr; memcpy(addr.bytes, slot->addr, 16);
        uint64_t exp = slot->entry.expires_at;
        bool scan_too = i % 16 == 0; /* the O(rows) oracle on a sample, the core on all */
        lookups_agree(inc->section, &addr, now, scan_too);
        lookups_agree(inc->section, &addr, exp, scan_too); /* the last live instant */
        if (exp < UINT64_MAX) lookups_agree(inc->section, &addr, exp + 1, scan_too);
    }
    for (int k = 0; k < 64; ++k) {
        fips_node_addr miss = pool_addr(POOL + rnd()); /* never inserted */
        lookups_agree(inc->section, &miss, now, k % 4 == 0);
        fips_node_addr maybe = pool_addr(rnd() % POOL);
        lookups_agree(inc->section, &maybe, now, k % 4 == 0);
    }
    fipsd_dp_cache_view_free(&fresh);
}
static size_t parts_differing(const fipsd_dp_cache *a, const fipsd_dp_cache *b)
{
    size_t n = 0;
    for (size_t g = 0; g <= a->group_mask; ++g)
        for (size_t j = 0; j < FIPSD_DP_CACHE_FANOUT; ++j)
            n += (a->groups[g] ? a->groups[g]->parts[j] : NULL) != (b->groups[g] ? b->groups[g]->parts[j] : NULL);
    return n;
}
int main(void)
{
    fips_coord_slot *slots = calloc(SLOTS, sizeof *slots); uint32_t *index = calloc(WORDS, sizeof *index);
    assert(slots && index);
    fips_fixed_entropy entropy = {0x5a};
    assert(fips_coord_cache_init(&cache, slots, SLOTS, index, WORDS, MAX, 20000, fips_test_instant(0),
        fips_fixed_entropy_adapter(&entropy)) == FIPS_OK);
    fipsd_dp_cache_view inc; fips_fixed_entropy key = {0x17};
    fipsd_dp_cache_view_init(&inc, fips_fixed_entropy_adapter(&key));
    assert(fipsd_dp_cache_view_update(&inc, &cache) && !inc.section); /* empty: nothing to publish */
    size_t updates = 0, checks = 0, touches = 0, max_len = 0;
    uint64_t now = 0;
    for (uint32_t i = 0; i < MAX + MAX / 4; ++i) { /* fill past capacity: hint writes evict */
        fips_node_addr a = pool_addr(i); fips_cached_coord c = coords_for(&a, 9, 2 + i % 6); fips_hint_outcome o;
        assert(fips_coord_cache_insert(&cache, a.bytes, &c, fips_test_instant(now), &o) == FIPS_OK);
        if (i % 97 == 0) { assert(fipsd_dp_cache_view_update(&inc, &cache)); ++updates; }
    }
    for (int round = 0; round < 4000; ++round) {
        uint32_t ops = 1 + rnd() % 8, r = rnd();
        now += r % 50 == 0 ? r % 500 : r % 3;
        if (r % 997 == 0) now += 25000; /* most entries expire */
        for (uint32_t i = 0; i < ops; ++i) step(now);
        assert(fipsd_dp_cache_view_update(&inc, &cache));
        ++updates;
        if (cache.len > max_len) max_len = cache.len;
        if (round % 25 == 0 || round < 40) { views_agree(&inc, now); ++checks; }
        /* A single touch of a live entry rebuilds one part and shares the rest. */
        fips_node_addr a = pool_addr(rnd() % POOL);
        if (inc.section && fips_coord_cache_get(&cache, a.bytes, fips_test_instant(now))) {
            fipsd_dp_cache *before = fipsd_dp_cache_retain(inc.section);
            uint64_t version = inc.version, was = fipsd_dp_cache_find(before, &a, 0)->expires;
            ++now;
            assert(fips_coord_cache_get_and_touch(&cache, a.bytes, fips_test_instant(now)));
            assert(fipsd_dp_cache_view_update(&inc, &cache));
            if (was == fips_coord_cache_get_entry(&cache, a.bytes)->expires_at) /* the row did not move */
                assert(inc.version == version && inc.section == before);
            else
                assert(inc.version == version + 1 && parts_differing(before, inc.section) == 1);
            assert(fipsd_dp_cache_find(inc.section, &a, now)->expires == fips_coord_cache_get_entry(&cache, a.bytes)->expires_at);
            fipsd_dp_cache_release(before);
            ++touches;
        }
        /* A demote changes no published row: the section stands. */
        if (inc.section && r % 3 == 0 && fips_coord_cache_get_entry(&cache, a.bytes)) {
            const fipsd_dp_cache *before = inc.section;
            assert(fips_coord_cache_demote(&cache, a.bytes));
            assert(fipsd_dp_cache_view_update(&inc, &cache) && inc.section == before);
        }
    }
    views_agree(&inc, now);
    uint64_t evictions = fips_coord_cache_eviction_examined(&cache);
    printf("dataplane cache: %zu updates, %zu full comparisons, %zu single-touch updates, peak %zu entries "
           "(%zu groups), %llu eviction candidates; lookups %zu present, %zu expired, %zu absent: ok\n", updates, checks,
           touches, max_len, inc.groups, (unsigned long long)evictions, present, expired, absent);
    assert(checks > 100 && touches > 500 && max_len >= MAX && inc.groups > 1 && evictions > 300);
    assert(present > 1000 && expired > 1000 && absent > 1000);
    fipsd_dp_cache_view_free(&inc);
    free(index); free(slots);
    return 0;
}
