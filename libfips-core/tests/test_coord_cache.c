#include "fakes/host.h"
#include <fips/cache/coord_cache.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); return 1; } } while (0)
#define CHECKS(e, step) do { if (!(e)) { fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #e, step); return 1; } } while (0)
#define INSERTED FIPS_HINT_INSERTED
#define CHANGED FIPS_HINT_CHANGED
#define UNCHANGED FIPS_HINT_UNCHANGED
#define REJECTED FIPS_HINT_REJECTED
#define MAIN FIPS_SEGMENT_MAIN
#define PROBATION FIPS_SEGMENT_PROBATION
#define UNCHARGED FIPS_FUNDING_UNCHARGED
#define UPTO(n) FIPS_FUNDING_UPTO(n)
#define APE FIPS_ADDRS_PER_ENTRY
#define DEFAULT_SIZE FIPS_DEFAULT_COORD_CACHE_SIZE
#define DEFAULT_TTL FIPS_DEFAULT_COORD_CACHE_TTL_MS
#define VERIFIED_TTL FIPS_VERIFIED_TTL_MS
/* Headroom of 4 slots over the default geometry for verified-excess tests. */
#define SLOTS (FIPS_COORD_CACHE_SLOTS(DEFAULT_SIZE) + 4)
static fips_coord_slot slots[SLOTS];
static uint32_t index_words[FIPS_COORD_CACHE_INDEX_WORDS(SLOTS)];
static fips_coord_cache cache;
static int unexpected; /* FIPS_ERR_RANGE where upstream cannot fail */
typedef struct naddr { uint8_t b[16]; } naddr;
static fips_instant t(uint64_t ms) { return fips_test_instant(ms); }
static fips_status new_cache_with(size_t cap, size_t max, uint64_t ttl, uint8_t entropy_byte)
{
    fips_fixed_entropy entropy = {entropy_byte};
    return fips_coord_cache_init(&cache, slots, cap, index_words,
                                 FIPS_COORD_CACHE_INDEX_WORDS(SLOTS), max, ttl, t(0),
                                 fips_fixed_entropy_adapter(&entropy));
}
#define NEW(max, ttl) CHECK(new_cache_with(SLOTS, max, ttl, 7) == FIPS_OK)
#define NEW_DEFAULT() NEW(DEFAULT_SIZE, DEFAULT_TTL)
static naddr make_node_addr(uint8_t val)
{
    naddr a = {{0}};
    a.b[0] = val;
    return a;
}
static naddr minted_addr(uint8_t tag, uint32_t val)
{
    naddr a = {{0}};
    for (int i = 0; i < 4; ++i) a.b[i] = (uint8_t)(val >> (8 * i));
    a.b[15] = tag;
    return a;
}
static fips_cached_coord co(const uint8_t *ids, size_t n)
{
    uint8_t addrs[FIPS_CACHED_COORD_MAX_ENTRIES][16];
    fips_cached_coord c;
    for (size_t i = 0; i < n; ++i) memcpy(addrs[i], make_node_addr(ids[i]).b, 16);
    if (fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])addrs, n) != FIPS_OK) ++unexpected;
    return c;
}
#define C(...) co((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
/* addr, len - 2 minted ancestors, root. */
static fips_cached_coord coords_of_len(naddr addr, naddr root, size_t len)
{
    uint8_t addrs[FIPS_CACHED_COORD_MAX_ENTRIES][16];
    fips_cached_coord c;
    memcpy(addrs[0], addr.b, 16);
    for (uint32_t i = 0; i + 2 < len; ++i) memcpy(addrs[i + 1], minted_addr(0xd0, i).b, 16);
    memcpy(addrs[len - 1], root.b, 16);
    if (fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])addrs, len) != FIPS_OK) ++unexpected;
    return c;
}
static int ins(naddr a, fips_cached_coord c, uint64_t ms)
{
    fips_hint_outcome out;
    if (fips_coord_cache_insert(&cache, a.b, &c, t(ms), &out) != FIPS_OK) { ++unexpected; return -1; }
    return (int)out;
}
static int inst(naddr a, fips_cached_coord c, uint64_t ms, uint64_t ttl)
{
    fips_hint_outcome out;
    if (fips_coord_cache_insert_with_ttl(&cache, a.b, &c, t(ms), ttl, &out) != FIPS_OK) {
        ++unexpected;
        return -1;
    }
    return (int)out;
}
static fips_placed insf(naddr a, fips_cached_coord c, uint64_t ms, fips_funding f)
{
    fips_placed out = {REJECTED, 0, MAIN, false};
    if (fips_coord_cache_insert_funded(&cache, a.b, &c, t(ms), f, &out) != FIPS_OK) ++unexpected;
    return out;
}
static fips_placed inss(naddr a, fips_cached_coord c, uint64_t ms, fips_funding f)
{
    fips_placed out = {REJECTED, 0, MAIN, false};
    if (fips_coord_cache_insert_stated(&cache, a.b, &c, t(ms), f, &out) != FIPS_OK) ++unexpected;
    return out;
}
static void insv(naddr a, fips_cached_coord c, uint64_t ms)
{
    if (fips_coord_cache_insert_verified(&cache, a.b, &c, t(ms)) != FIPS_OK) ++unexpected;
}
static void insvm(naddr a, fips_cached_coord c, uint64_t ms, uint16_t mtu)
{
    if (fips_coord_cache_insert_verified_with_path_mtu(&cache, a.b, &c, t(ms), mtu) != FIPS_OK)
        ++unexpected;
}
static const fips_cached_coord *get(naddr a, uint64_t ms)
{
    return fips_coord_cache_get(&cache, a.b, t(ms));
}
static const fips_cached_coord *gat(naddr a, uint64_t ms)
{
    return fips_coord_cache_get_and_touch(&cache, a.b, t(ms));
}
static const fips_cache_entry *ent(naddr a) { return fips_coord_cache_get_entry(&cache, a.b); }
static bool has(naddr a, uint64_t ms) { return fips_coord_cache_contains(&cache, a.b, t(ms)); }
static size_t len(void) { return fips_coord_cache_len(&cache); }
static bool eq(const fips_cached_coord *p, fips_cached_coord c)
{
    return p != NULL && fips_cached_coord_equal(p, &c);
}
static bool in_segment(naddr a, fips_segment want)
{
    fips_segment seg;
    return fips_coord_cache_segment_of(&cache, a.b, &seg) && seg == want;
}
static bool verified_at(naddr a, uint64_t ms)
{
    const fips_cache_entry *e = ent(a);
    return e != NULL && fips_cache_entry_is_verified(e, ms);
}
static uint64_t weight_of(const fips_cached_coord *c) { return c->len > APE ? c->len : APE; }
/* Port of assert_index_matches_map: every slot filed under its own key in the
 * heap its place names, heaps hold nothing else, and the weights add up. */
static int index_matches(const char *step)
{
    const fips_coord_cache *c = &cache;
    CHECKS(c->by_use_len + c->parked_len + c->probation_len == c->len, step);
    bool all_main_verified = true;
    uint64_t main_weight = 0;
    size_t used = 0;
    for (size_t i = 0; i < c->slot_capacity; ++i) {
        const fips_coord_slot *s = &c->slots[i];
        if (!s->used) continue;
        ++used;
        const uint32_t *heap = s->place == FIPS_PLACE_BY_USE ? c->by_use
                             : s->place == FIPS_PLACE_PARKED ? c->parked : c->probation;
        size_t heap_len = s->place == FIPS_PLACE_BY_USE ? c->by_use_len
                        : s->place == FIPS_PLACE_PARKED ? c->parked_len : c->probation_len;
        CHECKS(s->heap_pos < heap_len && heap[s->heap_pos] == i, step);
        CHECKS(s->weight >= weight_of(&s->entry.coords), step);
        CHECKS(fips_coord_cache_get_entry(c, s->addr) == &s->entry, step);
        if (s->place != FIPS_PLACE_PROBATION) {
            main_weight += s->weight;
            all_main_verified &= s->entry.source == FIPS_COORD_VERIFIED;
        }
    }
    CHECKS(used == c->len, step);
    CHECKS(fips_coord_cache_main_len(c) <= c->max_entries || all_main_verified, step);
    CHECKS(c->probation_len <= fips_coord_cache_probation_bound(c), step);
    CHECKS(c->stored_weight == main_weight, step);
    const uint32_t *heaps[] = {c->by_use, c->parked, c->probation};
    size_t lens[] = {c->by_use_len, c->parked_len, c->probation_len};
    fips_cache_place places[] = {FIPS_PLACE_BY_USE, FIPS_PLACE_PARKED, FIPS_PLACE_PROBATION};
    for (int h = 0; h < 3; ++h)
        for (size_t pos = 0; pos < lens[h]; ++pos) {
            const fips_coord_slot *s = &c->slots[heaps[h][pos]];
            CHECKS(s->used && s->place == places[h] && s->heap_pos == pos, step);
            if (pos == 0) continue;
            const fips_coord_slot *parent = &c->slots[heaps[h][(pos - 1) / 2]];
            CHECKS(parent->key_stamp < s->key_stamp ||
                   (parent->key_stamp == s->key_stamp && parent->key_seq < s->key_seq), step);
        }
    return 0;
}
#define INDEX_MATCHES(step) CHECK(index_matches(step) == 0)

/* cache/tests/entry.rs: 6 upstream cases. */
static int test_cache_entry_expiry(void)
{
    fips_cache_entry e;
    fips_cached_coord c = C(1, 0);
    fips_cache_entry_init(&e, &c, 1000, 500);
    CHECK(!fips_cache_entry_is_expired(&e, 1000));
    CHECK(!fips_cache_entry_is_expired(&e, 1500));
    CHECK(fips_cache_entry_is_expired(&e, 1501));
    CHECK(fips_cache_entry_is_expired(&e, 2000));
    return 0;
}
static int test_cache_entry_refresh(void)
{
    fips_cache_entry e;
    fips_cached_coord c = C(1, 0);
    fips_cache_entry_init(&e, &c, 1000, 500);
    CHECK(fips_cache_entry_is_expired(&e, 1501));
    fips_cache_entry_refresh(&e, 1400, 500);
    CHECK(!fips_cache_entry_is_expired(&e, 1600));
    CHECK(!fips_cache_entry_is_expired(&e, 1900));
    CHECK(fips_cache_entry_is_expired(&e, 1901));
    return 0;
}
static int test_cache_entry_times(void)
{
    fips_cache_entry e;
    fips_cached_coord c = C(1, 0);
    fips_cache_entry_init(&e, &c, 1000, 500);
    CHECK(e.created_at == 1000 && e.last_used == 1000 && e.expires_at == 1500);
    CHECK(fips_cache_entry_age(&e, 1200) == 200);
    CHECK(fips_cache_entry_idle_time(&e, 1200) == 200);
    CHECK(fips_cache_entry_time_to_expiry(&e, 1200) == 300);
    CHECK(fips_cache_entry_time_to_expiry(&e, 1600) == 0);
    return 0;
}
static int test_cache_entry_touch(void)
{
    fips_cache_entry e;
    fips_cached_coord c = C(1, 0);
    fips_cache_entry_init(&e, &c, 1000, 500);
    CHECK(e.last_used == 1000);
    fips_cache_entry_touch(&e, 1300);
    CHECK(e.last_used == 1300 && e.expires_at == 1500);
    return 0;
}
static int test_cache_entry_update(void)
{
    fips_cache_entry e;
    fips_cached_coord c = C(1, 0), moved = C(1, 2, 0);
    fips_cache_entry_init(&e, &c, 1000, 500);
    fips_cache_entry_update(&e, &moved, 2000, 600);
    CHECK(fips_cached_coord_equal(&e.coords, &moved));
    CHECK(e.last_used == 2000 && e.expires_at == 2600 && e.created_at == 1000);
    return 0;
}
static int test_cache_entry_saturating_add(void)
{
    fips_cache_entry e;
    fips_cached_coord c = C(1, 0);
    fips_cache_entry_init(&e, &c, UINT64_MAX - 10, 100);
    CHECK(e.expires_at == UINT64_MAX);
    return 0;
}
/* cache/tests/mod.rs: 4 upstream cases. */
static int test_cache_stats_fill_ratio(void)
{
    fips_cache_stats s = {50, 100, 0, 0};
    CHECK(fabs(fips_cache_stats_fill_ratio(&s) - 0.5) < 1e-15);
    return 0;
}
static int test_cache_stats_fill_ratio_zero_max(void)
{
    fips_cache_stats s = {0, 0, 0, 0};
    CHECK(fabs(fips_cache_stats_fill_ratio(&s)) < 1e-15);
    return 0;
}
static int test_cache_stats_fill_ratio_full(void)
{
    fips_cache_stats s = {100, 100, 10, 500};
    CHECK(fabs(fips_cache_stats_fill_ratio(&s) - 1.0) < 1e-15);
    return 0;
}
static int test_cache_error_display(void)
{
    CHECK(strcmp(fips_cache_error_name(FIPS_CACHE_FULL), "cache full") == 0);
    CHECK(strcmp(fips_cache_error_name(FIPS_CACHE_NOT_FOUND), "entry not found") == 0);
    CHECK(strcmp(fips_cache_error_name(FIPS_CACHE_EXPIRED), "entry expired") == 0);
    return 0;
}

/* cache/tests/coord_cache.rs: 66 upstream cases, same names. */
static int test_coord_cache_basic(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    fips_cached_coord coords = C(1, 0);
    (void)ins(addr, coords, 0);
    CHECK(has(addr, 0));
    CHECK(eq(get(addr, 0), coords));
    CHECK(len() == 1);
    return 0;
}
static int test_coord_cache_expiry(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    (void)ins(addr, C(1, 0), 0);
    CHECK(has(addr, 500));
    CHECK(!has(addr, 1500));
    return 0;
}
static int test_coord_cache_update(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    (void)ins(addr, C(1, 0), 0);
    (void)ins(addr, C(1, 2, 0), 500);
    CHECK(len() == 1);
    const fips_cached_coord *coords = get(addr, 500);
    CHECK(coords && coords->len - 1 == 2);
    return 0;
}
static int test_coord_cache_eviction(void)
{
    NEW(2, 10000);
    naddr a1 = make_node_addr(1), a2 = make_node_addr(2), a3 = make_node_addr(3);
    (void)ins(a1, C(1, 0), 0);
    (void)ins(a2, C(2, 0), 100);
    (void)gat(a2, 200);
    (void)ins(a3, C(3, 0), 300);
    CHECK(!has(a1, 300));
    CHECK(has(a2, 300));
    CHECK(has(a3, 300));
    return 0;
}
static int test_coord_cache_evict_expired_first(void)
{
    NEW(2, 100);
    (void)ins(make_node_addr(1), C(1, 0), 0);
    (void)ins(make_node_addr(2), C(2, 0), 50);
    (void)ins(make_node_addr(3), C(3, 0), 150);
    CHECK(!has(make_node_addr(1), 150));
    CHECK(has(make_node_addr(2), 150));
    CHECK(has(make_node_addr(3), 150));
    return 0;
}
static int test_coord_cache_purge_expired(void)
{
    NEW(100, 100);
    (void)ins(make_node_addr(1), C(1, 0), 0);
    (void)ins(make_node_addr(2), C(2, 0), 50);
    (void)ins(make_node_addr(3), C(3, 0), 200);
    CHECK(len() == 3);
    CHECK(fips_coord_cache_purge_expired(&cache, t(151)) == 2);
    CHECK(len() == 1);
    CHECK(has(make_node_addr(3), 151));
    return 0;
}
static int test_coord_cache_stats(void)
{
    NEW(100, 100);
    (void)ins(make_node_addr(1), C(1, 0), 0);
    (void)ins(make_node_addr(2), C(2, 0), 50);
    fips_cache_stats s = fips_coord_cache_stats(&cache, t(150));
    CHECK(s.entries == 2 && s.max_entries == 100 && s.expired == 1 && s.avg_age_ms > 0);
    return 0;
}
static int test_coord_cache_insert_with_ttl(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    (void)inst(addr, C(1, 0), 0, 200);
    CHECK(has(addr, 100));
    CHECK(!has(addr, 201));
    return 0;
}
static int test_coord_cache_insert_with_ttl_update(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    (void)inst(addr, C(1, 0), 0, 200);
    (void)inst(addr, C(1, 2, 0), 100, 300);
    CHECK(len() == 1);
    const fips_cached_coord *coords = get(addr, 100);
    CHECK(coords && coords->len - 1 == 2);
    CHECK(has(addr, 399));
    CHECK(!has(addr, 401));
    return 0;
}
static int test_coord_cache_get_and_touch_removes_expired(void)
{
    NEW(100, 100);
    naddr addr = make_node_addr(1);
    (void)ins(addr, C(1, 0), 0);
    CHECK(len() == 1);
    CHECK(gat(addr, 200) == NULL);
    CHECK(len() == 0);
    return 0;
}
static int test_coord_cache_get_entry(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    (void)ins(addr, C(1, 0), 500);
    const fips_cache_entry *e = ent(addr);
    CHECK(e && e->created_at == 500 && e->expires_at == 1500);
    CHECK(ent(make_node_addr(99)) == NULL);
    return 0;
}
static int test_coord_cache_remove(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    (void)ins(addr, C(1, 0), 0);
    CHECK(len() == 1);
    CHECK(fips_coord_cache_remove(&cache, addr.b, NULL));
    CHECK(len() == 0);
    CHECK(!fips_coord_cache_remove(&cache, addr.b, NULL));
    return 0;
}
static int test_coord_cache_clear_and_is_empty(void)
{
    NEW(100, 1000);
    CHECK(fips_coord_cache_is_empty(&cache));
    (void)ins(make_node_addr(1), C(1, 0), 0);
    (void)ins(make_node_addr(2), C(2, 0), 0);
    CHECK(!fips_coord_cache_is_empty(&cache));
    fips_coord_cache_clear(&cache);
    CHECK(fips_coord_cache_is_empty(&cache) && len() == 0);
    return 0;
}
static int test_coord_cache_default(void)
{
    NEW_DEFAULT();
    CHECK(fips_coord_cache_max_entries(&cache) == DEFAULT_SIZE);
    CHECK(fips_coord_cache_default_ttl_ms(&cache) == DEFAULT_TTL);
    CHECK(fips_coord_cache_is_empty(&cache));
    return 0;
}
static int test_coord_cache_set_default_ttl(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    fips_coord_cache_set_default_ttl_ms(&cache, 200);
    CHECK(fips_coord_cache_default_ttl_ms(&cache) == 200);
    (void)ins(addr, C(1, 0), 0);
    CHECK(has(addr, 100));
    CHECK(!has(addr, 201));
    return 0;
}
static int test_coord_cache_stats_empty(void)
{
    NEW(100, 1000);
    fips_cache_stats s = fips_coord_cache_stats(&cache, t(0));
    CHECK(s.entries == 0 && s.max_entries == 100 && s.expired == 0 && s.avg_age_ms == 0);
    return 0;
}
static int test_invalidate_via_node_at_self_depth(void)
{
    NEW(100, 1000);
    naddr target = make_node_addr(1);
    (void)ins(target, C(1, 0), 0);
    CHECK(len() == 1);
    CHECK(fips_coord_cache_invalidate_via_node(&cache, target.b) == 1);
    CHECK(len() == 0);
    return 0;
}
static int test_invalidate_via_node_interior(void)
{
    NEW(100, 1000);
    (void)ins(make_node_addr(5), C(5, 3, 1, 0), 0);
    CHECK(fips_coord_cache_invalidate_via_node(&cache, make_node_addr(3).b) == 1);
    CHECK(len() == 0);
    return 0;
}
static int test_invalidate_via_node_absent(void)
{
    NEW(100, 1000);
    naddr dest = make_node_addr(5);
    (void)ins(dest, C(5, 3, 1, 0), 0);
    CHECK(fips_coord_cache_invalidate_via_node(&cache, make_node_addr(99).b) == 0);
    CHECK(len() == 1 && has(dest, 0));
    return 0;
}
static int test_invalidate_via_node_empty_cache(void)
{
    NEW(100, 1000);
    CHECK(fips_coord_cache_invalidate_via_node(&cache, make_node_addr(1).b) == 0);
    CHECK(len() == 0);
    return 0;
}
static int test_invalidate_other_roots_current_root_kept(void)
{
    NEW(100, 1000);
    (void)ins(make_node_addr(1), C(1, 0), 0);
    (void)ins(make_node_addr(2), C(2, 0), 0);
    CHECK(fips_coord_cache_invalidate_other_roots(&cache, make_node_addr(0).b) == 0);
    CHECK(len() == 2);
    return 0;
}
static int test_invalidate_other_roots_different_root_dropped(void)
{
    NEW(100, 1000);
    (void)ins(make_node_addr(1), C(1, 0), 0);
    (void)ins(make_node_addr(2), C(2, 0), 0);
    (void)ins(make_node_addr(3), C(3, 0), 0);
    (void)ins(make_node_addr(4), C(4, 9), 0);
    CHECK(fips_coord_cache_invalidate_other_roots(&cache, make_node_addr(0).b) == 1);
    CHECK(len() == 3);
    CHECK(!has(make_node_addr(4), 0));
    CHECK(has(make_node_addr(1), 0));
    return 0;
}
static int test_invalidate_other_roots_all_match(void)
{
    NEW(100, 1000);
    (void)ins(make_node_addr(1), C(1, 0), 0);
    (void)ins(make_node_addr(2), C(2, 0), 0);
    CHECK(fips_coord_cache_invalidate_other_roots(&cache, make_node_addr(0).b) == 0);
    CHECK(len() == 2);
    return 0;
}
static int test_invalidate_other_roots_empty_cache(void)
{
    NEW(100, 1000);
    CHECK(fips_coord_cache_invalidate_other_roots(&cache, make_node_addr(0).b) == 0);
    CHECK(len() == 0);
    return 0;
}
static int a_hint_does_not_displace_a_live_verified_entry(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    fips_cached_coord good = C(1, 0);
    insv(addr, good, 0);
    CHECK(ins(addr, C(1, 2, 0), 10) == REJECTED);
    CHECK(eq(get(addr, 10), good));
    return 0;
}
static int a_verified_write_displaces_a_hint(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    fips_cached_coord good = C(1, 0);
    CHECK(ins(addr, C(1, 2, 0), 0) == INSERTED);
    insv(addr, good, 10);
    CHECK(eq(get(addr, 10), good));
    return 0;
}
static int verification_ages_out_so_a_stale_verified_entry_stops_refusing_hints(void)
{
    NEW(100, UINT64_MAX / 4);
    naddr addr = make_node_addr(1);
    insv(addr, C(1, 0), 0);
    CHECK(ins(addr, C(1, 2, 0), VERIFIED_TTL) == REJECTED);
    fips_cached_coord moved = C(1, 3, 0);
    CHECK(ins(addr, moved, VERIFIED_TTL + 1) == CHANGED);
    CHECK(eq(get(addr, VERIFIED_TTL + 1), moved));
    return 0;
}
static int ordinary_traffic_does_not_extend_the_verification_window(void)
{
    NEW(100, VERIFIED_TTL);
    naddr addr = make_node_addr(1);
    insv(addr, C(1, 0), 0);
    uint64_t touches[] = {100, 1000, 100000, VERIFIED_TTL};
    for (size_t i = 0; i < 4; ++i) (void)gat(addr, touches[i]);
    CHECK(ins(addr, C(1, 2, 0), VERIFIED_TTL + 1) == CHANGED);
    return 0;
}
static int eviction_prefers_an_unverified_victim_over_a_verified_one(void)
{
    NEW(2, 1000000);
    naddr verified = make_node_addr(1), hint = make_node_addr(2), newcomer = make_node_addr(3);
    insv(verified, C(1, 0), 0);
    CHECK(ins(hint, C(2, 0), 100) == INSERTED);
    CHECK(ins(newcomer, C(3, 0), 200) == INSERTED);
    CHECK(has(verified, 200));
    CHECK(!has(hint, 200));
    return 0;
}
static int a_cache_full_of_verified_entries_refuses_a_hint_rather_than_evicting_one(void)
{
    NEW(2, 1000000);
    insv(make_node_addr(1), C(1, 0), 0);
    insv(make_node_addr(2), C(2, 0), 0);
    CHECK(ins(make_node_addr(3), C(3, 0), 10) == REJECTED);
    CHECK(has(make_node_addr(1), 10) && has(make_node_addr(2), 10));
    return 0;
}
static int a_stated_write_replaces_a_live_verified_entry_whose_coordinates_differ(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    fips_cached_coord moved = C(1, 2, 0);
    insv(addr, C(1, 0), 0);
    fips_placed p = inss(addr, moved, 10, UNCHARGED);
    CHECK(p.outcome == CHANGED);
    const fips_cache_entry *e = ent(addr);
    CHECK(e && fips_cached_coord_equal(&e->coords, &moved));
    CHECK(!fips_cache_entry_is_verified(e, 10));
    return 0;
}
static int a_stated_write_that_does_not_name_its_key_yields_to_a_live_verified_entry(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    fips_cached_coord good = C(1, 0);
    insv(addr, good, 0);
    fips_placed p = inss(addr, C(2, 0), 10, UNCHARGED);
    CHECK(p.outcome == REJECTED);
    const fips_cache_entry *e = ent(addr);
    CHECK(e && fips_cached_coord_equal(&e->coords, &good));
    CHECK(fips_cache_entry_is_verified(e, 10));
    return 0;
}
static int a_stated_write_of_the_verified_coordinates_leaves_the_entry_verified(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    fips_cached_coord good = C(1, 0);
    insv(addr, good, 0);
    fips_placed p = inss(addr, good, 10, UNCHARGED);
    CHECK(p.outcome == REJECTED);
    CHECK(verified_at(addr, 10));
    return 0;
}
static int a_stated_write_after_the_verification_lapses_is_an_ordinary_hint_write(void)
{
    NEW(100, UINT64_MAX / 4);
    naddr addr = make_node_addr(1);
    insv(addr, C(1, 0), 0);
    fips_cached_coord moved = C(1, 3, 0);
    fips_placed p = inss(addr, moved, VERIFIED_TTL + 1, UNCHARGED);
    CHECK(p.outcome == CHANGED);
    CHECK(eq(get(addr, VERIFIED_TTL + 1), moved));
    return 0;
}
static int a_stated_write_deeper_than_the_entry_charge_replaces_a_verified_entry_through_eviction(void)
{
    naddr root = make_node_addr(0), addr = make_node_addr(1);
    NEW(100, 1000);
    insv(addr, C(1, 0), 0);
    fips_cached_coord deep = coords_of_len(addr, root, APE + 4);
    fips_placed p = inss(addr, deep, 10, UNCHARGED);
    CHECK(p.outcome == CHANGED && p.added_weight == 4);
    const fips_cache_entry *e = ent(addr);
    CHECK(e && fips_cached_coord_equal(&e->coords, &deep));
    CHECK(!fips_cache_entry_is_verified(e, 10));
    INDEX_MATCHES("stated growing update with room");
    return 0;
}
static int a_stated_write_that_finds_no_room_is_stored_and_leaves_the_other_verified_entry(void)
{
    naddr root = make_node_addr(0), addr = make_node_addr(1), other = make_node_addr(2);
    fips_cached_coord deep = coords_of_len(addr, root, APE + 4);
    NEW(2, 1000000);
    CHECK(ins(addr, C(1, 0), 0) == INSERTED);
    insv(other, C(2, 0), 0);
    CHECK(ins(addr, deep, 10) == REJECTED);
    fips_placed p = inss(addr, deep, 10, UNCHARGED);
    CHECK(p.outcome == CHANGED);
    CHECK(eq(get(addr, 10), deep));
    INDEX_MATCHES("stated growing update over a hint, no room");

    NEW(2, 1000000);
    insv(addr, C(1, 0), 0);
    insv(other, C(2, 0), 0);
    p = inss(addr, deep, 10, UNCHARGED);
    CHECK(p.outcome == CHANGED);
    const fips_cache_entry *e = ent(addr);
    CHECK(e && fips_cached_coord_equal(&e->coords, &deep));
    CHECK(!fips_cache_entry_is_verified(e, 10));
    CHECK(verified_at(other, 10));
    CHECK(cache.stored_weight == 2 * APE + 4);
    INDEX_MATCHES("stated growing update over a verified entry, no room");
    return 0;
}
static int demote_keeps_the_value_and_lets_a_hint_replace_it(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    fips_cached_coord real = C(1, 0);
    insvm(addr, real, 10, 1400);
    CHECK(ins(addr, C(1, 2, 0), 10) == REJECTED);
    CHECK(fips_coord_cache_demote(&cache, addr.b));
    const fips_cache_entry *e = ent(addr);
    CHECK(e && fips_cached_coord_equal(&e->coords, &real));
    CHECK(e->source == FIPS_COORD_HINT && !fips_cache_entry_is_verified(e, 10));
    CHECK(e->has_path_mtu && e->path_mtu == 1400);
    CHECK(ins(addr, C(1, 2, 0), 11) == CHANGED);
    return 0;
}
static int demote_of_an_absent_entry_reports_none(void)
{
    NEW(100, 1000);
    CHECK(!fips_coord_cache_demote(&cache, make_node_addr(1).b));
    CHECK(fips_coord_cache_is_empty(&cache));
    return 0;
}
static int clear_path_mtu_keeps_the_entry(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    insvm(addr, C(1, 0), 10, 1400);
    fips_coord_cache_clear_path_mtu(&cache, addr.b);
    const fips_cache_entry *e = ent(addr);
    CHECK(e && !e->has_path_mtu && fips_cache_entry_is_verified(e, 10));
    return 0;
}
#define MAX_MEAN_EXAMINED_PER_INSERT 8
static int eviction_at_capacity_examines_a_bounded_number_of_entries(void)
{
    NEW_DEFAULT();
    naddr root = make_node_addr(0);
    uint32_t capacity = DEFAULT_SIZE;
    for (uint32_t i = 0; i < capacity; ++i) {
        naddr addr = minted_addr(0xfe, i);
        (void)ins(addr, coords_of_len(addr, root, 2), 1000 + i);
    }
    CHECK(len() == DEFAULT_SIZE);
    CHECK(fips_coord_cache_eviction_examined(&cache) == 0);
    uint32_t fresh = 1000;
    uint64_t now = 1000 + capacity;
    for (uint32_t i = 0; i < fresh; ++i) {
        naddr addr = minted_addr(0xfd, i);
        CHECK(ins(addr, coords_of_len(addr, root, 2), now + i) == INSERTED);
    }
    CHECK(len() == DEFAULT_SIZE);
    uint64_t mean = fips_coord_cache_eviction_examined(&cache) / fresh;
    CHECK(mean <= MAX_MEAN_EXAMINED_PER_INSERT);
    return 0;
}
static int a_cache_filled_with_depth_seventy_hints_stays_within_its_address_budget(void)
{
    NEW_DEFAULT();
    naddr root = make_node_addr(0), newest = root;
    uint32_t capacity = DEFAULT_SIZE;
    for (uint32_t i = 0; i < capacity; ++i) {
        naddr addr = minted_addr(0xfe, i);
        (void)ins(addr, coords_of_len(addr, root, 71), 1000 + i);
        newest = addr;
    }
    CHECK(has(newest, 1000 + capacity));
    CHECK(fips_coord_cache_stored_addrs(&cache) <= (size_t)DEFAULT_SIZE * APE);
    return 0;
}
static int a_depth_five_fill_reaches_max_entries_with_nothing_evicted_early(void)
{
    NEW_DEFAULT();
    naddr root = make_node_addr(0);
    uint32_t capacity = DEFAULT_SIZE;
    for (uint32_t i = 0; i < capacity; ++i) {
        naddr addr = minted_addr(0xfe, i);
        (void)ins(addr, coords_of_len(addr, root, 6), 1000 + i);
    }
    uint64_t now = 1000 + capacity;
    CHECK(len() == DEFAULT_SIZE);
    CHECK(has(minted_addr(0xfe, 0), now));
    CHECK(fips_coord_cache_eviction_examined(&cache) == 0);
    CHECK(fips_coord_cache_stored_addrs(&cache) == (size_t)DEFAULT_SIZE * 6);
    return 0;
}
static int a_touched_entry_survives_a_flood_of_new_keys(void)
{
    NEW(1000, DEFAULT_TTL);
    naddr root = make_node_addr(0);
    for (uint32_t i = 0; i < 1000; ++i) {
        naddr addr = minted_addr(0xfe, i);
        (void)ins(addr, coords_of_len(addr, root, 2), 1000 + i);
    }
    naddr kept = minted_addr(0xfe, 0);
    uint64_t now = 3000;
    for (uint32_t i = 0; i < 5000; ++i) {
        CHECK(gat(kept, now) != NULL);
        naddr addr = minted_addr(0xfd, i);
        (void)ins(addr, coords_of_len(addr, root, 2), now + 1);
        now += 2;
    }
    CHECK(has(kept, now));
    CHECK(!has(minted_addr(0xfe, 1), now));
    return 0;
}
static int the_eviction_index_matches_the_map_after_every_mutation(void)
{
    naddr root = make_node_addr(0), other_root = make_node_addr(9), via = minted_addr(0xd0, 0);
    NEW(8, 1000);
    uint64_t now = 10000;
    bool parked_seen = false, expiry_seen = false, probation_seen = false, promoted_seen = false;
    insv(minted_addr(1, 0), coords_of_len(minted_addr(1, 0), root, 2), now);
    INDEX_MATCHES("insert_verified");
    insvm(minted_addr(1, 1), coords_of_len(minted_addr(1, 1), root, 2), now, 1400);
    INDEX_MATCHES("insert_verified_with_path_mtu");
    uint32_t state = 0x2545f491u;
    char step_name[64];
    for (uint32_t step = 0; step < 4000; ++step) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        naddr addr = minted_addr(2, state % 24);
        now += state % 7;
        const char *name;
        switch ((state >> 8) % 12) {
        case 0: case 1: {
            size_t l = 2 + (state >> 4) % 30;
            (void)ins(addr, coords_of_len(addr, root, l), now);
            name = "insert";
            break;
        }
        case 2: case 3: {
            size_t l = 2 + (state >> 4) % 30;
            fips_placed p = insf(addr, coords_of_len(addr, root, l), now, UPTO((state >> 20) % 48));
            probation_seen |= p.segment == PROBATION;
            promoted_seen |= p.segment == MAIN && p.added_weight >= APE && p.outcome != INSERTED;
            name = "insert_funded";
            break;
        }
        case 4: {
            const fips_cache_entry *e = ent(addr);
            bool had = e != NULL && fips_cache_entry_is_expired(e, now);
            (void)gat(addr, now);
            expiry_seen |= had;
            name = "get_and_touch";
            break;
        }
        case 5:
            insv(addr, coords_of_len(addr, root, 2), now);
            name = "insert_verified";
            break;
        case 6:
            (void)fips_coord_cache_remove(&cache, addr.b, NULL);
            name = "remove";
            break;
        case 7:
            (void)fips_coord_cache_demote(&cache, addr.b);
            name = "demote";
            break;
        case 8:
            (void)fips_coord_cache_purge_expired(&cache, t(now));
            name = "purge_expired";
            break;
        case 9:
            (void)ins(addr, coords_of_len(addr, other_root, 2), now);
            (void)fips_coord_cache_invalidate_other_roots(&cache, root.b);
            name = "invalidate_other_roots";
            break;
        case 10:
            (void)fips_coord_cache_invalidate_via_node(&cache, via.b);
            name = "invalidate_via_node";
            break;
        default:
            if (step % 500 == 499) {
                fips_coord_cache_clear(&cache);
                name = "clear";
            } else {
                now += 2000;
                name = "time passes";
            }
        }
        parked_seen |= cache.parked_len != 0;
        snprintf(step_name, sizeof(step_name), "step %u (%s)", step, name);
        INDEX_MATCHES(step_name);
    }
    CHECK(parked_seen && expiry_seen && probation_seen && promoted_seen);

    fips_coord_cache_clear(&cache);
    insv(minted_addr(3, 0), coords_of_len(minted_addr(3, 0), root, 2), now);
    insv(minted_addr(3, 1), coords_of_len(minted_addr(3, 1), root, 2), now);
    for (uint32_t i = 0; i < 8; ++i) {
        naddr addr = minted_addr(4, i);
        (void)ins(addr, coords_of_len(addr, root, 2), now + 1);
    }
    CHECK(cache.parked_len == 2);
    CHECK(fips_coord_cache_demote(&cache, minted_addr(3, 0).b));
    INDEX_MATCHES("demote of a parked entry");
    naddr stated = minted_addr(3, 1);
    fips_placed p = inss(stated, coords_of_len(stated, root, APE + 4), now + 1, UNCHARGED);
    CHECK(p.outcome == CHANGED);
    INDEX_MATCHES("stated write over a parked entry");
    uint64_t lapsed = now + VERIFIED_TTL + 1;
    naddr addr = minted_addr(5, 0);
    (void)ins(addr, coords_of_len(addr, root, 2), lapsed);
    INDEX_MATCHES("eviction after a verification lapsed");
    return 0;
}
static int an_entry_whose_verification_lapsed_becomes_evictable(void)
{
    NEW(2, UINT64_MAX / 4);
    naddr root = make_node_addr(0), verified = make_node_addr(1);
    insv(verified, C(1, 0), 0);
    (void)ins(make_node_addr(2), C(2, 0), 10);
    (void)ins(make_node_addr(3), C(3, 0), 20);
    CHECK(has(verified, 20));
    CHECK(!has(make_node_addr(2), 20));
    uint64_t after = VERIFIED_TTL + 1;
    naddr newcomer = minted_addr(0xfe, 0);
    CHECK(ins(newcomer, coords_of_len(newcomer, root, 2), after) == INSERTED);
    CHECK(!has(verified, after));
    CHECK(has(make_node_addr(3), after));
    return 0;
}
static int entries_untouched_since_insertion_are_evicted_in_insertion_order(void)
{
    NEW(64, DEFAULT_TTL);
    naddr root = make_node_addr(0);
    uint64_t now = 1000;
    naddr addrs[64];
    for (uint32_t i = 0; i < 64; ++i) {
        addrs[i] = minted_addr(0xfe, 1000 - i);
        (void)ins(addrs[i], coords_of_len(addrs[i], root, 2), now);
    }
    for (uint32_t i = 0; i < 32; ++i) {
        naddr addr = minted_addr(0xfd, i);
        (void)ins(addr, coords_of_len(addr, root, 2), now);
    }
    for (size_t i = 0; i < 64; ++i) CHECK(has(addrs[i], now) == (i >= 32));
    return 0;
}
static int a_growing_update_evicts_others_to_stay_within_the_address_budget(void)
{
    NEW(4, DEFAULT_TTL);
    naddr root = make_node_addr(0), addrs[4];
    for (uint32_t i = 0; i < 4; ++i) {
        addrs[i] = minted_addr(0xfe, i);
        (void)ins(addrs[i], coords_of_len(addrs[i], root, 2), 1000 + i);
    }
    CHECK(cache.stored_weight == 64);
    CHECK(ins(addrs[0], coords_of_len(addrs[0], root, 40), 2000) == CHANGED);
    const fips_cached_coord *c = get(addrs[0], 2000);
    CHECK(c && c->len == 40);
    CHECK(!has(addrs[1], 2000) && !has(addrs[2], 2000) && has(addrs[3], 2000));
    CHECK(cache.stored_weight <= 64);
    INDEX_MATCHES("growing update");
    return 0;
}
static void fill_depth_five(naddr root, uint8_t tag, uint64_t start)
{
    uint32_t i = 0;
    while (fips_coord_cache_main_len(&cache) < cache.max_entries) {
        naddr addr = minted_addr(tag, i);
        (void)ins(addr, coords_of_len(addr, root, 6), start + i);
        ++i;
    }
}
#define SENDER_WEIGHT_PER_SEC (64 * (uint64_t)APE)
static void refill_main(naddr root, uint32_t *refill, uint64_t now)
{
    while (fips_coord_cache_main_len(&cache) < cache.max_entries &&
           cache.stored_weight + APE <= fips_coord_cache_weight_bound(&cache)) {
        naddr addr = minted_addr(0xfc, *refill);
        (void)ins(addr, coords_of_len(addr, root, 6), now);
        ++*refill;
    }
}
static int alternating_deep_and_shallow_updates_of_one_key_evict_no_more_than_the_senders_budget_allows(void)
{
    NEW_DEFAULT();
    naddr root = make_node_addr(0);
    fill_depth_five(root, 0xfe, 1000);
    uint64_t now = 1000 + DEFAULT_SIZE;
    uint32_t refill = 0;
    naddr key = minted_addr(0xaa, 0);
    uint64_t milli = 0, evicted = 0, paid_writes = 0, growths_applied = 0, steps = 1000;
    for (uint64_t i = 0; i < steps; ++i) {
        now += 1;
        milli += SENDER_WEIGHT_PER_SEC;
        if (milli > 256 * (uint64_t)APE * 1000) milli = 256 * (uint64_t)APE * 1000;
        uint64_t available = milli / 1000;
        refill_main(root, &refill, now);
        size_t l = i % 2 == 0 ? 3 : 71;
        bool in_main = in_segment(key, MAIN);
        uint64_t main_before = fips_coord_cache_main_len(&cache);
        fips_placed p = insf(key, coords_of_len(key, root, l), now, UPTO(available));
        uint64_t joined = !in_main && p.segment == MAIN;
        evicted += main_before + joined - fips_coord_cache_main_len(&cache);
        CHECK(p.added_weight <= available);
        milli -= p.added_weight * 1000;
        paid_writes += p.added_weight > 0;
        growths_applied += l == 71 && p.outcome == CHANGED;
    }
    uint64_t earned = steps * SENDER_WEIGHT_PER_SEC / 1000;
    uint64_t allowed = (earned + APE - 1) / APE + paid_writes;
    CHECK(growths_applied > 0);
    CHECK(evicted <= allowed);
    return 0;
}
static int growing_updates_of_many_keys_evict_no_more_than_the_senders_budget_allows(void)
{
    NEW_DEFAULT();
    naddr root = make_node_addr(0);
    fill_depth_five(root, 0xfe, 1000);
    uint64_t now = 1000 + DEFAULT_SIZE;
    uint32_t refill = 0, steps = 1000;
    uint64_t milli = 0, evicted = 0, paid_writes = 0;
    for (uint32_t i = 0; i < steps; ++i) {
        now += 1;
        milli += SENDER_WEIGHT_PER_SEC;
        if (milli > 256 * (uint64_t)APE * 1000) milli = 256 * (uint64_t)APE * 1000;
        uint64_t available = milli / 1000;
        refill_main(root, &refill, now);
        naddr key = minted_addr(0xfe, DEFAULT_SIZE - 1 - i);
        if (!in_segment(key, MAIN)) continue;
        uint64_t main_before = fips_coord_cache_main_len(&cache);
        fips_placed p = insf(key, coords_of_len(key, root, 71), now, UPTO(available));
        evicted += main_before - fips_coord_cache_main_len(&cache);
        CHECK(p.added_weight <= available);
        milli -= p.added_weight * 1000;
        paid_writes += p.added_weight > 0;
    }
    uint64_t earned = (uint64_t)steps * SENDER_WEIGHT_PER_SEC / 1000;
    uint64_t allowed = (earned + APE - 1) / APE + paid_writes;
    CHECK(evicted <= allowed);
    CHECK(paid_writes > 0);
    return 0;
}
static int a_growing_update_is_charged_its_added_weight(void)
{
    NEW(100, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    fips_placed p = insf(key, coords_of_len(key, root, 2), 10, UPTO(16));
    CHECK(p.outcome == INSERTED && p.added_weight == 16 && p.segment == MAIN);
    p = insf(key, coords_of_len(key, root, 40), 20, UPTO(24));
    CHECK(p.outcome == CHANGED && p.added_weight == 24 && !p.unfunded);
    CHECK(cache.stored_weight == 40);
    return 0;
}
static int an_unpaid_growing_update_keeps_the_old_coordinates(void)
{
    NEW(64, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    fips_cached_coord old = coords_of_len(key, root, 2);
    (void)ins(key, old, 10);
    fill_depth_five(root, 0xfe, 20);
    size_t before = len();
    fips_placed p = insf(key, coords_of_len(key, root, 40), 30, UPTO(23));
    CHECK(p.outcome == REJECTED && p.added_weight == 0 && p.unfunded);
    CHECK(eq(get(key, 30), old));
    CHECK(len() == before);
    return 0;
}
static int a_same_weight_rewrite_of_an_existing_key_is_never_charged(void)
{
    NEW(100, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    (void)ins(key, coords_of_len(key, root, 20), 10);
    uint8_t addrs[20][16];
    memcpy(addrs[0], key.b, 16);
    for (uint32_t i = 0; i < 18; ++i) memcpy(addrs[i + 1], minted_addr(0xc0, i).b, 16);
    memcpy(addrs[19], root.b, 16);
    fips_cached_coord moved;
    CHECK(fips_cached_coord_from_addrs(&moved, (const uint8_t (*)[16])addrs, 20) == FIPS_OK);
    fips_placed p = insf(key, moved, 20, UPTO(0));
    CHECK(p.outcome == CHANGED && p.added_weight == 0 && !p.unfunded);
    CHECK(eq(get(key, 20), moved));
    fips_cached_coord shallow = coords_of_len(key, root, 3);
    p = insf(key, shallow, 30, UPTO(0));
    CHECK(p.added_weight == 0 && !p.unfunded);
    CHECK(eq(get(key, 30), shallow));
    CHECK(cache.stored_weight == 20);
    return 0;
}
static int an_unpaid_rewrite_back_to_an_entrys_charged_depth_corrects_a_shallow_forgery(void)
{
    NEW(100, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    fips_cached_coord real = coords_of_len(key, root, 20);
    fips_placed p = insf(key, real, 10, UPTO(20));
    CHECK(p.segment == MAIN && p.added_weight == 20);
    fips_cached_coord forged = coords_of_len(key, root, 3);
    (void)insf(key, forged, 20, UPTO(0));
    CHECK(eq(get(key, 20), forged));
    p = insf(key, real, 30, UPTO(0));
    CHECK(p.outcome == CHANGED && p.added_weight == 0 && !p.unfunded);
    CHECK(eq(get(key, 30), real));
    p = insf(key, coords_of_len(key, root, 21), 40, UPTO(0));
    CHECK(p.outcome == REJECTED && p.unfunded);
    CHECK(eq(get(key, 40), real));
    CHECK(cache.stored_weight == 20);
    return 0;
}
static int a_paid_new_key_main_has_no_room_for_goes_to_probation_and_evicts_nothing(void)
{
    NEW(8, DEFAULT_TTL);
    naddr root = make_node_addr(0), hint = minted_addr(0xaa, 0);
    (void)ins(hint, coords_of_len(hint, root, 2), 10);
    for (uint32_t i = 0; i < 3; ++i) {
        naddr verified = minted_addr(0xbb, i);
        insv(verified, coords_of_len(verified, root, 37), 20);
    }
    CHECK(cache.stored_weight == 127);
    naddr key = minted_addr(0xcc, 0);
    fips_placed p = insf(key, coords_of_len(key, root, 30), 30, UPTO(30));
    CHECK(p.outcome == INSERTED && p.added_weight == 0 && p.segment == PROBATION);
    CHECK(has(key, 30));
    CHECK(has(hint, 30));
    CHECK(cache.stored_weight == 127);
    INDEX_MATCHES("placed in probation for want of room");
    naddr small = minted_addr(0xcc, 1);
    p = insf(small, coords_of_len(small, root, 2), 40, UPTO(16));
    CHECK(p.outcome == INSERTED);
    CHECK(!has(hint, 40));
    INDEX_MATCHES("room made");
    return 0;
}
static int an_unpaid_write_evicts_only_probation_entries(void)
{
    NEW(64, DEFAULT_TTL);
    naddr root = make_node_addr(0);
    fill_depth_five(root, 0xfe, 10);
    CHECK(fips_coord_cache_probation_bound(&cache) == 2);
    for (uint32_t i = 0; i < 10; ++i) {
        naddr addr = minted_addr(0xaa, i);
        fips_placed p = insf(addr, coords_of_len(addr, root, 2), 100, UPTO(0));
        CHECK(p.outcome == INSERTED && p.segment == PROBATION && p.unfunded);
    }
    for (uint32_t i = 0; i < 64; ++i) CHECK(has(minted_addr(0xfe, i), 100));
    CHECK(fips_coord_cache_probation_len(&cache) == 2);
    CHECK(has(minted_addr(0xaa, 9), 100) && has(minted_addr(0xaa, 8), 100));
    CHECK(!has(minted_addr(0xaa, 7), 100));
    return 0;
}
static int a_probation_entry_is_not_promoted_by_get_and_touch(void)
{
    NEW(64, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    (void)insf(key, coords_of_len(key, root, 2), 10, UPTO(0));
    for (uint64_t tm = 11; tm < 100; ++tm) CHECK(gat(key, tm) != NULL);
    CHECK(in_segment(key, PROBATION));
    return 0;
}
static int a_paid_write_of_a_probation_key_promotes_it(void)
{
    NEW(64, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    (void)insf(key, coords_of_len(key, root, 2), 10, UPTO(0));
    fips_placed p = insf(key, coords_of_len(key, root, 2), 20, UPTO(15));
    CHECK(p.segment == PROBATION);
    p = insf(key, coords_of_len(key, root, 2), 30, UPTO(16));
    CHECK(p.segment == MAIN && p.added_weight == 16);
    CHECK(fips_coord_cache_probation_len(&cache) == 0);
    naddr other = minted_addr(0xaa, 1);
    (void)insf(other, coords_of_len(other, root, 2), 40, UPTO(0));
    (void)ins(other, coords_of_len(other, root, 2), 50);
    CHECK(in_segment(other, MAIN));
    return 0;
}
static int probation_holds_its_entry_count_whatever_the_depth(void)
{
    NEW_DEFAULT();
    naddr root = make_node_addr(0);
    size_t bound = DEFAULT_SIZE / FIPS_PROBATION_SHARE_DIVISOR;
    for (uint32_t i = 0; i < 5000; ++i) {
        naddr addr = minted_addr(0xaa, i);
        (void)insf(addr, coords_of_len(addr, root, 71), 1000 + i, UPTO(0));
    }
    CHECK(fips_coord_cache_probation_len(&cache) == bound);
    CHECK(len() == bound);
    CHECK(cache.stored_weight == 0);
    return 0;
}
static int a_probation_entry_a_frame_used_survives_a_flood_of_unpaid_keys(void)
{
    NEW_DEFAULT();
    naddr root = make_node_addr(0), used = minted_addr(0xab, 0), idle = minted_addr(0xab, 1);
    (void)insf(used, coords_of_len(used, root, 2), 1000, UPTO(0));
    (void)insf(idle, coords_of_len(idle, root, 2), 1000, UPTO(0));
    uint64_t now = 1001;
    for (uint32_t i = 0; i < 8000; ++i) {
        if (i % 100 == 0) CHECK(gat(used, now) != NULL);
        naddr addr = minted_addr(0xaa, i);
        (void)insf(addr, coords_of_len(addr, root, 71), now, UPTO(0));
        now += 1;
    }
    CHECK(has(used, now));
    CHECK(!has(idle, now));
    return 0;
}
static int a_promotion_is_charged_at_least_the_weight_probation_held(void)
{
    NEW(100, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    fips_cached_coord real = coords_of_len(key, root, 20);
    (void)insf(key, real, 10, UPTO(0));
    CHECK(in_segment(key, PROBATION));
    fips_cached_coord forged = coords_of_len(key, root, 3);
    fips_placed p = insf(key, forged, 20, UPTO(16));
    CHECK(p.segment == PROBATION && p.added_weight == 0);
    CHECK(eq(get(key, 20), forged));
    (void)insf(key, real, 30, UPTO(0));
    CHECK(eq(get(key, 30), real));
    p = insf(key, forged, 40, UPTO(20));
    CHECK(p.segment == MAIN && p.added_weight == 20);
    CHECK(cache.stored_weight == 20);
    p = insf(key, real, 50, UPTO(0));
    CHECK(p.outcome == CHANGED && p.added_weight == 0 && !p.unfunded);
    CHECK(eq(get(key, 50), real));
    INDEX_MATCHES("promoted at the held weight");
    return 0;
}
static int an_unpaid_deepening_of_an_expired_entry_still_leaves_a_route(void)
{
    NEW(64, 1000);
    naddr root = make_node_addr(0), d = minted_addr(0xdd, 0);
    fips_placed p = insf(d, coords_of_len(d, root, 17), 0, UPTO(17));
    CHECK(p.segment == MAIN);
    uint64_t later = 5000;
    naddr control = minted_addr(0xdd, 1);
    (void)insf(control, coords_of_len(control, root, 18), later, UPTO(0));
    CHECK(gat(control, later) != NULL);
    p = insf(d, coords_of_len(d, root, 18), later, UPTO(0));
    CHECK(p.outcome == INSERTED && p.segment == PROBATION && p.unfunded);
    CHECK(gat(d, later) != NULL);
    CHECK(cache.stored_weight == 0);
    INDEX_MATCHES("expired entry replaced");
    return 0;
}
static int a_hint_deeper_than_the_whole_address_bound_is_stored_alone(void)
{
    NEW(1, DEFAULT_TTL);
    naddr root = make_node_addr(0), d = minted_addr(0xde, 0);
    CHECK(ins(d, coords_of_len(d, root, 20), 10) == INSERTED);
    CHECK(has(d, 10));
    naddr e = minted_addr(0xde, 1);
    fips_placed p = insf(e, coords_of_len(e, root, 30), 20, UPTO(30));
    CHECK(p.outcome == INSERTED && p.added_weight == 30 && p.segment == MAIN);
    CHECK(!has(d, 20));
    CHECK(cache.stored_weight == 30);
    INDEX_MATCHES("one deep entry");
    return 0;
}
static void fill_verified(naddr root, uint32_t count, uint64_t now)
{
    for (uint32_t i = 0; i < count; ++i) {
        naddr addr = minted_addr(0xbb, i);
        insv(addr, coords_of_len(addr, root, 2), now);
    }
}
static int a_growing_update_refused_for_want_of_room_keeps_the_entry(void)
{
    NEW(4, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    fips_cached_coord old = coords_of_len(key, root, 2);
    (void)ins(key, old, 10);
    fill_verified(root, 3, 20);
    CHECK(cache.stored_weight == 64);
    fips_placed p = insf(key, coords_of_len(key, root, 20), 30, UPTO(100));
    CHECK(p.outcome == REJECTED && p.added_weight == 0 && !p.unfunded);
    CHECK(eq(get(key, 30), old));
    CHECK(in_segment(key, MAIN));
    CHECK(cache.stored_weight == 64);
    INDEX_MATCHES("growing update refused");
    return 0;
}
static int a_promotion_refused_for_want_of_room_keeps_the_key_in_probation(void)
{
    NEW(4, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    (void)insf(key, coords_of_len(key, root, 2), 10, UPTO(0));
    fill_verified(root, 4, 20);
    fips_cached_coord new_coords = coords_of_len(key, root, 3);
    fips_placed p = insf(key, new_coords, 30, UPTO(100));
    CHECK(p.outcome == CHANGED && p.added_weight == 0 && p.segment == PROBATION);
    CHECK(eq(get(key, 30), new_coords));
    CHECK(in_segment(key, PROBATION));
    INDEX_MATCHES("promotion refused");
    return 0;
}
static int a_shallow_rewrite_in_probation_does_not_lower_the_weight_a_promotion_pays(void)
{
    NEW(100, DEFAULT_TTL);
    naddr root = make_node_addr(0), key = minted_addr(0xaa, 0);
    fips_cached_coord real = coords_of_len(key, root, 20);
    (void)insf(key, real, 10, UPTO(0));
    fips_cached_coord forged = coords_of_len(key, root, 3);
    (void)insf(key, forged, 20, UPTO(0));
    fips_placed p = insf(key, forged, 30, UPTO(16));
    CHECK(p.segment == PROBATION);
    (void)insf(key, real, 40, UPTO(0));
    CHECK(eq(get(key, 40), real));
    INDEX_MATCHES("probation charge kept");
    return 0;
}
static int coord_cache_keys_its_map_from_its_entropy_source(void)
{
    naddr key = make_node_addr(7);
    uint8_t first[16], second[16];
    CHECK(new_cache_with(SLOTS, 16, 1000, 1) == FIPS_OK);
    memcpy(first, cache.hash_key, 16);
    CHECK(ins(key, C(7, 0), 0) == INSERTED && has(key, 0));
    CHECK(new_cache_with(SLOTS, 16, 1000, 1) == FIPS_OK);
    CHECK(memcmp(first, cache.hash_key, 16) == 0);
    CHECK(new_cache_with(SLOTS, 16, 1000, 2) == FIPS_OK);
    memcpy(second, cache.hash_key, 16);
    CHECK(memcmp(first, second, 16) != 0);
    CHECK(ins(key, C(7, 0), 0) == INSERTED && has(key, 0));
    return 0;
}

/* C storage/geometry edges beyond upstream. */
static int init_rejects_insufficient_storage_and_runs_a_zero_entry_geometry(void)
{
    fips_fixed_entropy entropy = {7};
    fips_entropy e = fips_fixed_entropy_adapter(&entropy);
    CHECK(fips_coord_cache_init(&cache, slots, 2, index_words, 11, 2, 1000, t(0), e) == FIPS_ERR_RANGE);
    CHECK(fips_coord_cache_init(&cache, slots, 3, index_words, 15, 2, 1000, t(0), e) == FIPS_ERR_RANGE);
    CHECK(fips_coord_cache_init(&cache, slots, 3, index_words, 16, 2, 1000, t(0), e) == FIPS_OK);
    CHECK(fips_coord_cache_init(&cache, slots, 3, index_words, 16, 4, 1000, t(0), e) == FIPS_ERR_RANGE);
    /* Probation always holds one entry: zero slots never fit, one slot does. */
    CHECK(fips_coord_cache_init(&cache, NULL, 0, NULL, 0, 0, 1000, t(0), e) == FIPS_ERR_RANGE);
    CHECK(fips_coord_cache_init(&cache, slots, 1, index_words, 6, 0, 1000, t(0), e) == FIPS_OK);
    naddr addr = make_node_addr(1), other = make_node_addr(2);
    fips_cached_coord c10 = C(1, 0), c20 = C(2, 0);
    fips_hint_outcome outcome;
    fips_placed p = {REJECTED, 99, MAIN, true};
    CHECK(get(addr, 0) == NULL && gat(addr, 0) == NULL && ent(addr) == NULL);
    CHECK(!fips_coord_cache_remove(&cache, addr.b, NULL) && !fips_coord_cache_demote(&cache, addr.b));
    CHECK(fips_coord_cache_purge_expired(&cache, t(0)) == 0 && len() == 0);
    fips_cache_stats s = fips_coord_cache_stats(&cache, t(0));
    CHECK(s.entries == 0 && fabs(fips_cache_stats_fill_ratio(&s)) < 1e-15);
    /* max_entries 0: an uncharged hint is refused (nothing to evict), a
     * verified write is stored past the bound, then storage is exhausted. */
    CHECK(fips_coord_cache_insert(&cache, addr.b, &c10, t(0), &outcome) == FIPS_OK);
    CHECK(outcome == REJECTED && len() == 0);
    CHECK(fips_coord_cache_insert_verified(&cache, addr.b, &c10, t(0)) == FIPS_OK);
    CHECK(len() == 1 && has(addr, 0) && fips_coord_cache_main_len(&cache) == 1);
    CHECK(fips_coord_cache_insert_funded(&cache, other.b, &c20, t(0), UPTO(16), &p) == FIPS_ERR_RANGE);
    CHECK(p.added_weight == 99 && len() == 1);
    CHECK(fips_coord_cache_insert_verified(&cache, other.b, &c20, t(0)) == FIPS_ERR_RANGE);
    CHECK(len() == 1 && has(addr, 0) && !has(other, 0));
    INDEX_MATCHES("zero-entry geometry");
    return 0;
}
static int verified_excess_exhausts_storage_transactionally(void)
{
    /* max 2 with exactly FIPS_COORD_CACHE_SLOTS(2) == 3 slots. */
    CHECK(new_cache_with(3, 2, 1000000, 7) == FIPS_OK);
    naddr root = make_node_addr(0), a[4];
    fips_cached_coord c[4];
    for (uint32_t i = 0; i < 4; ++i) {
        a[i] = minted_addr(0xbb, i);
        c[i] = coords_of_len(a[i], root, 2);
    }
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(fips_coord_cache_insert_verified(&cache, a[i].b, &c[i], t(10)) == FIPS_OK);
    CHECK(len() == 3 && fips_coord_cache_main_len(&cache) == 3);
    INDEX_MATCHES("verified write past max_entries");
    CHECK(fips_coord_cache_insert_verified(&cache, a[3].b, &c[3], t(20)) == FIPS_ERR_RANGE);
    CHECK(len() == 3 && !has(a[3], 20) && cache.stored_weight == 48);
    for (uint32_t i = 0; i < 3; ++i) CHECK(verified_at(a[i], 20));
    INDEX_MATCHES("refused verified write");
    fips_placed p = {REJECTED, 99, MAIN, true};
    CHECK(fips_coord_cache_insert_funded(&cache, a[3].b, &c[3], t(20), UPTO(16), &p) == FIPS_ERR_RANGE);
    CHECK(p.added_weight == 99 && len() == 3);
    fips_hint_outcome outcome;
    CHECK(fips_coord_cache_insert(&cache, a[3].b, &c[3], t(20), &outcome) == FIPS_OK);
    CHECK(outcome == REJECTED && len() == 3);
    /* A rewrite of a resident key needs no slot. */
    fips_cached_coord deeper = coords_of_len(a[0], root, 3);
    CHECK(fips_coord_cache_insert_verified(&cache, a[0].b, &deeper, t(30)) == FIPS_OK);
    const fips_cached_coord *g = get(a[0], 30);
    CHECK(len() == 3 && g && g->len == 3);
    INDEX_MATCHES("verified rewrite at full storage");
    CHECK(fips_coord_cache_remove(&cache, a[1].b, NULL));
    CHECK(fips_coord_cache_insert_verified(&cache, a[3].b, &c[3], t(40)) == FIPS_OK);
    CHECK(len() == 3 && has(a[3], 40));
    INDEX_MATCHES("slot reclaimed");
    return 0;
}
static int ttl_boundaries_and_refresh(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    CHECK(ins(addr, C(1, 0), 0) == INSERTED);
    CHECK(has(addr, 1000) && !has(addr, 1001) && len() == 1);
    CHECK(gat(addr, 1000) != NULL);
    const fips_cache_entry *e = ent(addr);
    CHECK(e && e->expires_at == 2000 && e->last_used == 1000 && e->created_at == 0);
    CHECK(has(addr, 2000) && !has(addr, 2001));
    fips_coord_cache_set_default_ttl_ms(&cache, 10);
    CHECK(gat(addr, 1500) != NULL);
    e = ent(addr);
    CHECK(e && e->expires_at == 1510);
    CHECK(inst(make_node_addr(2), C(2, 0), 5, 0) == INSERTED);
    CHECK(has(make_node_addr(2), 5) && !has(make_node_addr(2), 6));
    CHECK(inst(make_node_addr(3), C(3, 0), 5, UINT64_MAX) == INSERTED);
    CHECK(has(make_node_addr(3), UINT64_MAX));
    /* Verified writes take the default TTL, not the verification window. */
    insv(make_node_addr(4), C(4, 0), 100);
    CHECK(has(make_node_addr(4), 110) && !has(make_node_addr(4), 111));
    e = ent(make_node_addr(4));
    CHECK(e && e->verified_until == 100 + VERIFIED_TTL);
    return 0;
}
static int instants_before_the_epoch_read_as_millisecond_zero(void)
{
    fips_fixed_entropy entropy = {7};
    CHECK(fips_coord_cache_init(&cache, slots, SLOTS, index_words,
                                FIPS_COORD_CACHE_INDEX_WORDS(SLOTS), 100, 1000, t(500),
                                fips_fixed_entropy_adapter(&entropy)) == FIPS_OK);
    CHECK(fips_coord_cache_millis(&cache, t(0)) == 0);
    CHECK(fips_coord_cache_millis(&cache, t(500)) == 0);
    CHECK(fips_coord_cache_millis(&cache, t(1700)) == 1200);
    CHECK(fips_coord_cache_millis(&cache, t(UINT64_MAX)) == UINT64_MAX - 500);
    naddr addr = make_node_addr(1);
    CHECK(ins(addr, C(1, 0), 700) == INSERTED);
    const fips_cache_entry *e = ent(addr);
    CHECK(e && e->created_at == 200 && e->expires_at == 1200);
    CHECK(has(addr, 1700) && !has(addr, 1701));
    return 0;
}
static bool count_visit(void *ctx, const uint8_t addr[16], const fips_cache_entry *entry)
{
    (void)addr;
    (void)entry;
    return ++*(size_t *)ctx < 2;
}
static int iter_visits_live_entries_and_stops_on_request(void)
{
    NEW(100, 100);
    (void)ins(make_node_addr(1), C(1, 0), 0);
    (void)ins(make_node_addr(2), C(2, 0), 50);
    (void)ins(make_node_addr(3), C(3, 0), 200);
    size_t seen = 0;
    fips_coord_cache_iter(&cache, t(151), count_visit, &seen);
    CHECK(seen == 1);
    seen = 0;
    fips_coord_cache_iter(&cache, t(60), count_visit, &seen);
    CHECK(seen == 2);
    return 0;
}
typedef struct seen_list { const uint8_t *addr[64]; const fips_cache_entry *entry[64]; size_t n, stop; } seen_list;
static bool record_visit(void *ctx, const uint8_t addr[16], const fips_cache_entry *entry)
{
    seen_list *s = ctx;
    if (s->n < 64) { s->addr[s->n] = addr; s->entry[s->n] = entry; }
    return ++s->n < s->stop;
}
/* fips_coord_cache_scan is the O(len) fusion of iter + stats: same visits in
 * the same order, same stats, even after a visitor stop and on the fallback. */
static int scan_matches_iter_and_stats_under_churn(void)
{
    static uint64_t marks[FIPS_COORD_CACHE_MARK_WORDS(SLOTS)];
    NEW(48, 100);
    unexpected = 0;
    uint32_t rng = 12345;
    for (uint64_t step = 0, now = 0; step < 3000; ++step) {
        rng = rng * 1103515245u + 12345u;
        uint32_t r = rng >> 8;
        naddr a = minted_addr(1, r % 80), root = make_node_addr(9);
        now += r % 7;
        switch (r % 9) {
        case 0: case 1: case 2: (void)ins(a, coords_of_len(a, root, 2 + r % 4), now); break;
        case 3: insv(a, coords_of_len(a, root, 3), now); break;
        case 4: (void)fips_coord_cache_get_and_touch(&cache, a.b, t(now)); break;
        case 5: (void)fips_coord_cache_remove(&cache, a.b, NULL); break;
        case 6: (void)fips_coord_cache_demote(&cache, a.b); break;
        case 7: if (r % 5 == 0) (void)fips_coord_cache_purge_expired(&cache, t(now)); break;
        default: break;
        }
        uint64_t at = now + r % 150;
        size_t stop = 1 + r % 70;
        seen_list want = {.stop = stop}, got = {.stop = stop}, fallback = {.stop = stop};
        fips_coord_cache_iter(&cache, t(at), record_visit, &want);
        fips_cache_stats ws = fips_coord_cache_stats(&cache, t(at));
        fips_cache_stats gs = fips_coord_cache_scan(&cache, t(at), marks, FIPS_COORD_CACHE_MARK_WORDS(SLOTS), record_visit, &got);
        fips_cache_stats fs = fips_coord_cache_scan(&cache, t(at), marks, 1, record_visit, &fallback);
        fips_cache_stats ns = fips_coord_cache_scan(&cache, t(at), marks, FIPS_COORD_CACHE_MARK_WORDS(SLOTS), NULL, NULL);
        CHECK(want.n == got.n && want.n == fallback.n);
        for (size_t i = 0; i < want.n && i < 64; ++i)
            CHECK(want.addr[i] == got.addr[i] && want.entry[i] == got.entry[i] && want.entry[i] == fallback.entry[i]);
        const fips_cache_stats *all[3] = {&gs, &fs, &ns};
        for (size_t i = 0; i < 3; ++i)
            CHECK(all[i]->entries == ws.entries && all[i]->max_entries == ws.max_entries &&
                  all[i]->expired == ws.expired && all[i]->avg_age_ms == ws.avg_age_ms);
    }
    CHECK(unexpected == 0);
    return 0;
}
static int remove_copies_the_entry_out(void)
{
    NEW(100, 1000);
    naddr addr = make_node_addr(1);
    insvm(addr, C(1, 0), 10, 1300);
    fips_cache_entry e = {0};
    CHECK(fips_coord_cache_remove(&cache, addr.b, &e));
    CHECK(e.created_at == 10 && e.has_path_mtu && e.path_mtu == 1300 && e.source == FIPS_COORD_VERIFIED);
    CHECK(len() == 0 && !fips_coord_cache_remove(&cache, addr.b, &e));
    return 0;
}
static int cached_coord_bounds_and_accessors(void)
{
    uint8_t addrs[FIPS_CACHED_COORD_MAX_ENTRIES + 1][16];
    for (uint32_t i = 0; i <= FIPS_CACHED_COORD_MAX_ENTRIES; ++i) memcpy(addrs[i], minted_addr(1, i).b, 16);
    fips_cached_coord c;
    c.len = 42;
    CHECK(fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])addrs, 0) == FIPS_ERR_RANGE && c.len == 42);
    CHECK(fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])addrs, FIPS_CACHED_COORD_MAX_ENTRIES + 1) == FIPS_ERR_RANGE);
    CHECK(c.len == 42);
    CHECK(fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])addrs, FIPS_CACHED_COORD_MAX_ENTRIES) == FIPS_OK);
    CHECK(c.len == FIPS_CACHED_COORD_MAX_ENTRIES);
    CHECK(memcmp(fips_cached_coord_node_addr(&c), addrs[0], 16) == 0);
    CHECK(memcmp(fips_cached_coord_root_id(&c), addrs[FIPS_CACHED_COORD_MAX_ENTRIES - 1], 16) == 0);
    CHECK(fips_cached_coord_contains(&c, addrs[17]) && !fips_cached_coord_contains(&c, addrs[FIPS_CACHED_COORD_MAX_ENTRIES]));
    fips_cached_coord d = c;
    CHECK(fips_cached_coord_equal(&c, &d));
    d.entries[3].sequence = 1;
    CHECK(!fips_cached_coord_equal(&c, &d));
    d = c;
    d.len -= 1;
    CHECK(!fips_cached_coord_equal(&c, &d));
    return 0;
}
static int lookups_survive_interleaved_deletions(void)
{
    NEW(512, DEFAULT_TTL);
    naddr root = make_node_addr(0);
    for (uint32_t i = 0; i < 400; ++i) {
        naddr addr = minted_addr(0x11, i);
        CHECK(ins(addr, coords_of_len(addr, root, 2), 10) == INSERTED);
    }
    for (uint32_t i = 0; i < 400; i += 2) CHECK(fips_coord_cache_remove(&cache, minted_addr(0x11, i).b, NULL));
    for (uint32_t i = 0; i < 400; ++i) CHECK(has(minted_addr(0x11, i), 10) == (i % 2 == 1));
    for (uint32_t i = 0; i < 400; i += 2) {
        naddr addr = minted_addr(0x11, i);
        CHECK(ins(addr, coords_of_len(addr, root, 3), 20) == INSERTED);
    }
    for (uint32_t i = 0; i < 400; ++i) {
        const fips_cached_coord *g = get(minted_addr(0x11, i), 20);
        CHECK(g && g->len == (i % 2 == 1 ? 2 : 3));
    }
    CHECK(len() == 400);
    INDEX_MATCHES("interleaved deletions");
    fips_coord_cache_clear(&cache);
    for (uint32_t i = 0; i < 400; ++i) CHECK(!has(minted_addr(0x11, i), 20));
    CHECK(ins(minted_addr(0x11, 5), coords_of_len(minted_addr(0x11, 5), root, 2), 30) == INSERTED);
    CHECK(len() == 1);
    return 0;
}

int main(void)
{
    int (*tests[])(void) = {
        test_cache_entry_expiry, test_cache_entry_refresh, test_cache_entry_times,
        test_cache_entry_touch, test_cache_entry_update, test_cache_entry_saturating_add,
        test_cache_stats_fill_ratio, test_cache_stats_fill_ratio_zero_max,
        test_cache_stats_fill_ratio_full, test_cache_error_display,
        test_coord_cache_basic, test_coord_cache_expiry, test_coord_cache_update,
        test_coord_cache_eviction, test_coord_cache_evict_expired_first,
        test_coord_cache_purge_expired, test_coord_cache_stats, test_coord_cache_insert_with_ttl,
        test_coord_cache_insert_with_ttl_update, test_coord_cache_get_and_touch_removes_expired,
        test_coord_cache_get_entry, test_coord_cache_remove, test_coord_cache_clear_and_is_empty,
        test_coord_cache_default, test_coord_cache_set_default_ttl, test_coord_cache_stats_empty,
        test_invalidate_via_node_at_self_depth, test_invalidate_via_node_interior,
        test_invalidate_via_node_absent, test_invalidate_via_node_empty_cache,
        test_invalidate_other_roots_current_root_kept,
        test_invalidate_other_roots_different_root_dropped, test_invalidate_other_roots_all_match,
        test_invalidate_other_roots_empty_cache, a_hint_does_not_displace_a_live_verified_entry,
        a_verified_write_displaces_a_hint,
        verification_ages_out_so_a_stale_verified_entry_stops_refusing_hints,
        ordinary_traffic_does_not_extend_the_verification_window,
        eviction_prefers_an_unverified_victim_over_a_verified_one,
        a_cache_full_of_verified_entries_refuses_a_hint_rather_than_evicting_one,
        a_stated_write_replaces_a_live_verified_entry_whose_coordinates_differ,
        a_stated_write_that_does_not_name_its_key_yields_to_a_live_verified_entry,
        a_stated_write_of_the_verified_coordinates_leaves_the_entry_verified,
        a_stated_write_after_the_verification_lapses_is_an_ordinary_hint_write,
        a_stated_write_deeper_than_the_entry_charge_replaces_a_verified_entry_through_eviction,
        a_stated_write_that_finds_no_room_is_stored_and_leaves_the_other_verified_entry,
        demote_keeps_the_value_and_lets_a_hint_replace_it, demote_of_an_absent_entry_reports_none,
        clear_path_mtu_keeps_the_entry, eviction_at_capacity_examines_a_bounded_number_of_entries,
        a_cache_filled_with_depth_seventy_hints_stays_within_its_address_budget,
        a_depth_five_fill_reaches_max_entries_with_nothing_evicted_early,
        a_touched_entry_survives_a_flood_of_new_keys,
        the_eviction_index_matches_the_map_after_every_mutation,
        an_entry_whose_verification_lapsed_becomes_evictable,
        entries_untouched_since_insertion_are_evicted_in_insertion_order,
        a_growing_update_evicts_others_to_stay_within_the_address_budget,
        alternating_deep_and_shallow_updates_of_one_key_evict_no_more_than_the_senders_budget_allows,
        growing_updates_of_many_keys_evict_no_more_than_the_senders_budget_allows,
        a_growing_update_is_charged_its_added_weight, an_unpaid_growing_update_keeps_the_old_coordinates,
        a_same_weight_rewrite_of_an_existing_key_is_never_charged,
        an_unpaid_rewrite_back_to_an_entrys_charged_depth_corrects_a_shallow_forgery,
        a_paid_new_key_main_has_no_room_for_goes_to_probation_and_evicts_nothing,
        an_unpaid_write_evicts_only_probation_entries, a_probation_entry_is_not_promoted_by_get_and_touch,
        a_paid_write_of_a_probation_key_promotes_it, probation_holds_its_entry_count_whatever_the_depth,
        a_probation_entry_a_frame_used_survives_a_flood_of_unpaid_keys,
        a_promotion_is_charged_at_least_the_weight_probation_held,
        an_unpaid_deepening_of_an_expired_entry_still_leaves_a_route,
        a_hint_deeper_than_the_whole_address_bound_is_stored_alone,
        a_growing_update_refused_for_want_of_room_keeps_the_entry,
        a_promotion_refused_for_want_of_room_keeps_the_key_in_probation,
        a_shallow_rewrite_in_probation_does_not_lower_the_weight_a_promotion_pays,
        coord_cache_keys_its_map_from_its_entropy_source,
        init_rejects_insufficient_storage_and_runs_a_zero_entry_geometry,
        verified_excess_exhausts_storage_transactionally, ttl_boundaries_and_refresh,
        instants_before_the_epoch_read_as_millisecond_zero,
        iter_visits_live_entries_and_stops_on_request, scan_matches_iter_and_stats_under_churn,
        remove_copies_the_entry_out,
        cached_coord_bounds_and_accessors, lookups_survive_interleaved_deletions
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i]()) return 1;
        if (unexpected) { fprintf(stderr, "test %zu: unexpected FIPS_ERR_RANGE\n", i); return 1; }
    }
    puts("coord cache: 66 upstream cache + 6 entry + 4 stats/error tests + 9 C edge tests passed");
    return 0;
}
