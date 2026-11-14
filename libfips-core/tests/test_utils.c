#include <fips/utils/index.h>
#include <fips/utils/hash.h>
#include <fips/utils/onlink.h>
#include "fakes/host.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)
#define INIT_ALLOC(n) uint32_t storage[n]; fips_seeded_entropy seed = {42}; \
    fips_entropy entropy = fips_seeded_entropy_adapter(&seed); \
    fips_index_allocator a; \
    fips_index_allocator_init(&a, storage, n, FIPS_INDEX_DEFAULT_MAX_ATTEMPTS, entropy)

static int test_session_index_roundtrip(void)
{
    fips_session_index idx = fips_session_index_new(0x12345678);
    CHECK(fips_session_index_as_u32(idx) == 0x12345678);
    uint8_t bytes[4], expected[] = {0x78, 0x56, 0x34, 0x12};
    fips_session_index_to_le_bytes(idx, bytes);
    CHECK(memcmp(bytes, expected, 4) == 0);
    fips_session_index restored = fips_session_index_from_le_bytes(bytes);
    CHECK(restored.value == idx.value);
    return 0;
}
static int test_session_index_display(void)
{
    char out[9];
    CHECK(fips_session_index_format(fips_session_index_new(255), out, sizeof(out)) == FIPS_OK);
    CHECK(strcmp(out, "000000ff") == 0);
    CHECK(fips_session_index_format(fips_session_index_new(0xdeadbeef), out, sizeof(out)) == FIPS_OK);
    CHECK(strcmp(out, "deadbeef") == 0);
    CHECK(fips_session_index_format(fips_session_index_new(0), out, 8) == FIPS_ERR_RANGE);
    CHECK(strcmp(out, "deadbeef") == 0);
    return 0;
}
static int test_allocator_basic(void)
{
    INIT_ALLOC(4);
    CHECK(fips_index_allocator_is_empty(&a) && fips_index_allocator_count(&a) == 0);
    fips_session_index one, two;
    CHECK(fips_index_allocator_allocate(&a, &one) == FIPS_INDEX_OK);
    CHECK(!fips_index_allocator_is_empty(&a) && fips_index_allocator_count(&a) == 1);
    CHECK(fips_index_allocator_is_allocated(&a, one));
    CHECK(fips_index_allocator_allocate(&a, &two) == FIPS_INDEX_OK);
    CHECK(fips_index_allocator_count(&a) == 2 && fips_index_allocator_is_allocated(&a, two));
    CHECK(one.value != two.value);
    CHECK(fips_index_allocator_free(&a, one) == FIPS_INDEX_OK);
    CHECK(fips_index_allocator_count(&a) == 1 && !fips_index_allocator_is_allocated(&a, one));
    CHECK(fips_index_allocator_is_allocated(&a, two));
    return 0;
}
static int test_allocator_free_not_found(void)
{
    INIT_ALLOC(1);
    CHECK(fips_index_allocator_free(&a, fips_session_index_new(12345)) == FIPS_INDEX_NOT_FOUND);
    return 0;
}
static int test_allocator_reserve(void)
{
    INIT_ALLOC(2);
    fips_session_index idx = fips_session_index_new(0xdeadbeef);
    CHECK(fips_index_allocator_reserve(&a, idx) == FIPS_INDEX_OK);
    CHECK(fips_index_allocator_is_allocated(&a, idx));
    CHECK(fips_index_allocator_reserve(&a, idx) == FIPS_INDEX_ALREADY_IN_USE);
    return 0;
}
static int test_allocator_uniqueness(void)
{
    INIT_ALLOC(1000);
    fips_session_index indices[1000];
    for (size_t i = 0; i < 1000; ++i) {
        CHECK(fips_index_allocator_allocate(&a, &indices[i]) == FIPS_INDEX_OK);
        for (size_t j = 0; j < i; ++j) CHECK(indices[j].value != indices[i].value);
    }
    CHECK(fips_index_allocator_count(&a) == 1000);
    return 0;
}
static int test_allocator_clear(void)
{
    INIT_ALLOC(10);
    fips_session_index idx;
    for (size_t i = 0; i < 10; ++i) CHECK(fips_index_allocator_allocate(&a, &idx) == FIPS_INDEX_OK);
    CHECK(fips_index_allocator_count(&a) == 10);
    fips_index_allocator_clear(&a);
    CHECK(fips_index_allocator_is_empty(&a) && fips_index_allocator_count(&a) == 0);
    return 0;
}
static int test_allocator_reuse_after_free(void)
{
    INIT_ALLOC(2);
    fips_session_index one, two;
    CHECK(fips_index_allocator_allocate(&a, &one) == FIPS_INDEX_OK);
    CHECK(fips_index_allocator_free(&a, one) == FIPS_INDEX_OK);
    CHECK(fips_index_allocator_allocate(&a, &two) == FIPS_INDEX_OK);
    CHECK(fips_index_allocator_count(&a) == 1);
    if (one.value != two.value) CHECK(fips_index_allocator_reserve(&a, one) == FIPS_INDEX_OK);
    return 0;
}
static int allocator_draws_indices_from_its_entropy_source(void)
{
    uint32_t storage[2];
    fips_fixed_entropy fixed = {1};
    fips_entropy entropy = fips_fixed_entropy_adapter(&fixed);
    fips_index_allocator a;
    fips_index_allocator_init(&a, storage, 2, 100, entropy);
    fips_session_index idx;
    CHECK(fips_index_allocator_allocate(&a, &idx) == FIPS_INDEX_OK && idx.value == 0x01010101);
    CHECK(fips_index_allocator_allocate(&a, &idx) == FIPS_INDEX_EXHAUSTED && idx.value == 0x01010101);
    return 0;
}
static int keyed_hasher_takes_its_key_from_the_entropy_source(void)
{
    fips_fixed_entropy one = {1}, two = {2};
    fips_entropy e1 = fips_fixed_entropy_adapter(&one), e2 = fips_fixed_entropy_adapter(&two);
    fips_keyed_hasher a, b, c;
    fips_keyed_hasher_init(&a, e1); fips_keyed_hasher_init(&b, e1); fips_keyed_hasher_init(&c, e2);
    uint8_t key[16]; memset(key, 0x5a, sizeof(key));
    CHECK(fips_siphash13(&a, key, 16) == fips_siphash13(&b, key, 16));
    CHECK(fips_siphash13(&a, key, 16) != fips_siphash13(&c, key, 16));
    fips_keyed_hasher_clear(&a); fips_keyed_hasher_clear(&b); fips_keyed_hasher_clear(&c);
    return 0;
}
static int keyed_hasher_draws_both_key_halves_from_the_entropy_source(void)
{
    fips_fixed_entropy fixed = {1};
    fips_entropy entropy = fips_fixed_entropy_adapter(&fixed);
    fips_keyed_hasher h; fips_keyed_hasher_init(&h, entropy);
    CHECK(h.k0 == UINT64_C(0x0101010101010101) && h.k1 == h.k0);
    fips_keyed_hasher_clear(&h);
    CHECK(h.k0 == 0 && h.k1 == 0);
    return 0;
}
/* Numeric fixture octets generated with Python ipaddress, not OS parsing. */
static fips_ip_addr ip(const char *text)
{
    static const struct { const char *text; fips_ip_addr addr; } fixtures[] = {
        {"10.0.0.1", {{10,0,0,1}, 4}},
        {"10.0.0.2", {{10,0,0,2}, 4}},
        {"10.20.1.1", {{10,20,1,1}, 4}},
        {"10.20.7.8", {{10,20,7,8}, 4}},
        {"10.21.0.1", {{10,21,0,1}, 4}},
        {"10.8.0.2", {{10,8,0,2}, 4}},
        {"10.8.0.5", {{10,8,0,5}, 4}},
        {"10.9.8.7", {{10,9,8,7}, 4}},
        {"172.17.0.1", {{172,17,0,1}, 4}},
        {"172.17.0.9", {{172,17,0,9}, 4}},
        {"192.168.1.1", {{192,168,1,1}, 4}},
        {"192.168.1.10", {{192,168,1,10}, 4}},
        {"192.168.1.127", {{192,168,1,127}, 4}},
        {"192.168.1.128", {{192,168,1,128}, 4}},
        {"192.168.1.2", {{192,168,1,2}, 4}},
        {"192.168.1.200", {{192,168,1,200}, 4}},
        {"192.168.1.5", {{192,168,1,5}, 4}},
        {"192.168.1.50", {{192,168,1,50}, 4}},
        {"192.168.1.7", {{192,168,1,7}, 4}},
        {"192.168.2.1", {{192,168,2,1}, 4}},
        {"192.168.2.5", {{192,168,2,5}, 4}},
        {"2001:db8:7fff::1", {{32,1,13,184,127,255,0,0,0,0,0,0,0,0,0,1}, 16}},
        {"2001:db8:8000::", {{32,1,13,184,128,0,0,0,0,0,0,0,0,0,0,0}, 16}},
        {"2001:db8::1", {{32,1,13,184,0,0,0,0,0,0,0,0,0,0,0,1}, 16}},
        {"2001:db8::2", {{32,1,13,184,0,0,0,0,0,0,0,0,0,0,0,2}, 16}},
        {"2001:db8:ffff::1", {{32,1,13,184,255,255,0,0,0,0,0,0,0,0,0,1}, 16}},
        {"203.0.113.5", {{203,0,113,5}, 4}},
        {"203.0.113.6", {{203,0,113,6}, 4}},
        {"8.8.8.8", {{8,8,8,8}, 4}},
        {"::ffff:192.168.1.1", {{0,0,0,0,0,0,0,0,0,0,255,255,192,168,1,1}, 16}},
        {"::ffff:192.168.1.10", {{0,0,0,0,0,0,0,0,0,0,255,255,192,168,1,10}, 16}},
        {"::ffff:192.168.1.7", {{0,0,0,0,0,0,0,0,0,0,255,255,192,168,1,7}, 16}},
        {"::ffff:192.168.2.7", {{0,0,0,0,0,0,0,0,0,0,255,255,192,168,2,7}, 16}},
        {"fd00::1", {{253,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}, 16}},
        {"fd00::2", {{253,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2}, 16}},
        {"fd01::1", {{253,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1}, 16}},
    };
    for (size_t i = 0; i < sizeof(fixtures)/sizeof(fixtures[0]); ++i)
        if (strcmp(text, fixtures[i].text) == 0) return fixtures[i].addr;
    return (fips_ip_addr){{0}, 0};
}
static fips_onlink_prefix prefix(const char *text, uint8_t len)
{
    return (fips_onlink_prefix){ip(text), len, false, 0};
}
static int ipv4_prefixes_hold_exactly_their_range(void)
{
    const struct { const char *net, *yes, *no; uint8_t len; } cases[] = {
        {"192.168.1.10", "192.168.1.200", "192.168.2.1", 24},
        {"192.168.1.10", "192.168.1.127", "192.168.1.128", 25},
        {"10.20.1.1", "10.20.7.8", "10.21.0.1", 16},
        {"203.0.113.5", "203.0.113.5", "203.0.113.6", 32}};
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        fips_onlink_prefix in = prefix(cases[i].net, cases[i].len), storage[1];
        fips_onlink_prefixes s;
        CHECK(fips_onlink_from_pairs(&s, storage, 1, &in, 1) == FIPS_OK);
        CHECK(fips_onlink_contains(&s, ip(cases[i].yes)));
        CHECK(!fips_onlink_contains(&s, ip(cases[i].no)));
    }
    return 0;
}
static int ipv6_prefixes_hold_exactly_their_range(void)
{
    fips_onlink_prefix in = prefix("fd00::2", 64), storage[1];
    fips_onlink_prefixes s;
    CHECK(fips_onlink_from_pairs(&s, storage, 1, &in, 1) == FIPS_OK);
    CHECK(fips_onlink_contains(&s, ip("fd00::1")));
    CHECK(!fips_onlink_contains(&s, ip("fd01::1")));
    in = prefix("2001:db8::1", 128);
    CHECK(fips_onlink_from_pairs(&s, storage, 1, &in, 1) == FIPS_OK);
    CHECK(fips_onlink_contains(&s, ip("2001:db8::1")));
    CHECK(!fips_onlink_contains(&s, ip("2001:db8::2")));
    return 0;
}
static int ipv4_mapped_addresses_are_judged_as_ipv4(void)
{
    fips_onlink_prefix in = prefix("::ffff:192.168.1.10", 24), storage[1];
    fips_onlink_prefixes s;
    CHECK(fips_onlink_from_pairs(&s, storage, 1, &in, 1) == FIPS_OK);
    CHECK(fips_onlink_contains(&s, ip("192.168.1.7")));
    CHECK(fips_onlink_contains(&s, ip("::ffff:192.168.1.7")));
    CHECK(!fips_onlink_contains(&s, ip("::ffff:192.168.2.7")));
    return 0;
}
static int a_zero_length_prefix_is_discarded_and_holds_nothing(void)
{
    fips_onlink_prefix in = prefix("192.168.1.1", 0);
    fips_onlink_prefixes s;
    CHECK(fips_onlink_from_pairs(&s, NULL, 0, &in, 1) == FIPS_OK);
    CHECK(fips_onlink_is_empty(&s) && !fips_onlink_contains(&s, ip("8.8.8.8")));
    return 0;
}
static int families_never_match_each_other(void)
{
    fips_onlink_prefix in = prefix("192.168.1.1", 24), storage[1];
    fips_onlink_prefixes s;
    CHECK(fips_onlink_from_pairs(&s, storage, 1, &in, 1) == FIPS_OK);
    CHECK(!fips_onlink_contains(&s, ip("fd00::1")));
    return 0;
}
static int share_link_needs_one_prefix_holding_both_addresses(void)
{
    fips_onlink_prefix in[] = {prefix("192.168.1.1", 24), prefix("10.0.0.1", 8)}, storage[2];
    fips_onlink_prefixes s, empty = {0};
    CHECK(fips_onlink_from_pairs(&s, storage, 2, in, 2) == FIPS_OK);
    CHECK(fips_onlink_share_link(&s, ip("192.168.1.5"), ip("192.168.1.50")));
    CHECK(!fips_onlink_share_link(&s, ip("192.168.1.5"), ip("10.9.8.7")));
    CHECK(!fips_onlink_share_link(&s, ip("192.168.1.5"), ip("192.168.2.5")));
    CHECK(fips_onlink_is_empty(&empty));
    CHECK(!fips_onlink_share_link(&empty, ip("10.0.0.1"), ip("10.0.0.2")));
    return 0;
}
static int contains_on_holds_only_the_named_interfaces_prefixes(void)
{
    fips_onlink_prefix in[] = {prefix("192.168.1.10", 24), prefix("10.8.0.2", 24),
        prefix("172.17.0.1", 16)}, storage[3];
    in[0].has_index = in[1].has_index = true; in[0].index = 2; in[1].index = 3;
    uint32_t interfaces[] = {2,3};
    fips_onlink_prefixes s;
    CHECK(fips_onlink_from_indexed(&s, storage, 3, in, 3) == FIPS_OK);
    CHECK(fips_onlink_contains_on(&s, ip("192.168.1.7"), interfaces, 1));
    CHECK(!fips_onlink_contains_on(&s, ip("10.8.0.5"), interfaces, 1));
    CHECK(fips_onlink_contains_on(&s, ip("10.8.0.5"), interfaces, 2));
    CHECK(!fips_onlink_contains_on(&s, ip("172.17.0.9"), interfaces, 2));
    CHECK(!fips_onlink_contains_on(&s, ip("192.168.1.7"), NULL, 0));
    CHECK(fips_onlink_contains(&s, ip("172.17.0.9")));
    CHECK(fips_onlink_from_pairs(&s, storage, 3, in, 1) == FIPS_OK);
    CHECK(!fips_onlink_contains_on(&s, ip("192.168.1.7"), interfaces, 1));
    return 0;
}
static int a_discarded_zero_length_prefix_or_an_empty_read_is_reported(void)
{
    fips_onlink_prefix in[] = {prefix("192.168.1.1", 0), prefix("10.0.0.1", 8)}, storage[2];
    fips_onlink_prefixes partly, empty = {0}, healthy;
    CHECK(fips_onlink_from_pairs(&partly, storage, 2, in, 2) == FIPS_OK);
    CHECK(fips_onlink_discarded(&partly) == 1 && fips_onlink_read_suspect(2, &partly));
    CHECK(fips_onlink_read_suspect(2, &empty));
    CHECK(!fips_onlink_read_suspect(0, &empty));
    CHECK(fips_onlink_from_pairs(&healthy, storage, 2, in + 1, 1) == FIPS_OK);
    CHECK(!fips_onlink_read_suspect(1, &healthy));
    return 0;
}

/* Raw-byte SipHash-1-3 reference vectors, lengths 0..63, key 00..0f.
 * https://raw.githubusercontent.com/jedisct1/rust-siphash/master/src/tests.rs (test_siphash_1_3). */
static const uint64_t sip_vectors[64] = {
    UINT64_C(0xabac0158050fc4dc), UINT64_C(0xc9f49bf37d57ca93), UINT64_C(0x82cb9b024dc7d44d), UINT64_C(0x8bf80ab8e7ddf7fb),
    UINT64_C(0xcf75576088d38328), UINT64_C(0xdef9d52f49533b67), UINT64_C(0xc50d2b50c59f22a7), UINT64_C(0xd3927d989bb11140),
    UINT64_C(0x369095118d299a8e), UINT64_C(0x25a48eb36c063de4), UINT64_C(0x79de85ee92ff097f), UINT64_C(0x70c118c1f94dc352),
    UINT64_C(0x78a384b157b4d9a2), UINT64_C(0x306f760c1229ffa7), UINT64_C(0x605aa111c0f95d34), UINT64_C(0xd320d86d2a519956),
    UINT64_C(0xcc4fdd1a7d908b66), UINT64_C(0x9cf2689063dbd80c), UINT64_C(0x8ffc389cb473e63e), UINT64_C(0xf21f9de58d297d1c),
    UINT64_C(0xc0dc2f46a6cce040), UINT64_C(0xb992abfe2b45f844), UINT64_C(0x7ffe7b9ba320872e), UINT64_C(0x525a0e7fdae6c123),
    UINT64_C(0xf464aeb267349c8c), UINT64_C(0x45cd5928705b0979), UINT64_C(0x3a3e35e3ca9913a5), UINT64_C(0xa91dc74e4ade3b35),
    UINT64_C(0xfb0bed02ef6cd00d), UINT64_C(0x88d93cb44ab1e1f4), UINT64_C(0x540f11d643c5e663), UINT64_C(0x2370dd1f8c21d1bc),
    UINT64_C(0x81157b6c16a7b60d), UINT64_C(0x4d54b9e57a8ff9bf), UINT64_C(0x759f12781f2a753e), UINT64_C(0xcea1a3bebf186b91),
    UINT64_C(0x2cf508d3ada26206), UINT64_C(0xb6101c2da3c33057), UINT64_C(0xb3f47496ae3a36a1), UINT64_C(0x626b57547b108392),
    UINT64_C(0xc1d2363299e41531), UINT64_C(0x667cc1923f1ad944), UINT64_C(0x65704ffec8138825), UINT64_C(0x24f280d1c28949a6),
    UINT64_C(0xc2ca1cedfaf8876b), UINT64_C(0xc2164bfc9f042196), UINT64_C(0xa16e9c9368b1d623), UINT64_C(0x49fb169c8b5114fd),
    UINT64_C(0x9f3143f8df074c46), UINT64_C(0xc6fdaf2412cc86b3), UINT64_C(0x7eaf49d10a52098f), UINT64_C(0x1cf313559d292f9a),
    UINT64_C(0xc44a30dda2f41f12), UINT64_C(0x36fae98943a71ed0), UINT64_C(0x318fb34c73f0bce6), UINT64_C(0xa27abf3670a7e980),
    UINT64_C(0xb4bcc0db243c6d75), UINT64_C(0x23f8d852fdb71513), UINT64_C(0x8f035f4da67d8a08), UINT64_C(0xd89cd0e5b7e8f148),
    UINT64_C(0xf6f4e6bcf7a644ee), UINT64_C(0xaec59ad80f1837f2), UINT64_C(0xc3b2f6154b6694e0), UINT64_C(0x9d199062b7bbb3a8),
};
static int siphash_reference_vectors(void)
{
    fips_keyed_hasher h = {UINT64_C(0x0706050403020100), UINT64_C(0x0f0e0d0c0b0a0908)};
    uint8_t bytes[63];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
    for (size_t len = 0; len < 64; ++len) CHECK(fips_siphash13(&h, bytes, len) == sip_vectors[len]);
    CHECK(fips_siphash13(&h, NULL, 0) == sip_vectors[0]);
    return 0;
}
/* Track fill call boundaries as well as deterministic bytes: a 4-byte draw
 * discards the upper half-word, whereas a 16-byte key uses two whole words. */
typedef struct trace_entropy { fips_seeded_entropy seed; size_t calls, last; } trace_entropy;
static void trace_fill(void *ctx, uint8_t *bytes, size_t n)
{
    trace_entropy *t = ctx;
    fips_entropy seeded = fips_seeded_entropy_adapter(&t->seed);
    ++t->calls; t->last = n;
    seeded.fill(seeded.ctx, bytes, n);
}
static int entropy_replay_and_allocator_edges(void)
{
    trace_entropy t = {{42}, 0, 0};
    fips_entropy entropy = {trace_fill, &t};
    fips_seeded_entropy expected = {42};
    uint64_t k0 = fips_splitmix64_next(&expected), k1 = fips_splitmix64_next(&expected);
    fips_keyed_hasher h;
    fips_keyed_hasher_init(&h, entropy);
    CHECK(t.calls == 1 && t.last == 16 && h.k0 == k0 && h.k1 == k1);
    uint32_t storage[2]; fips_index_allocator a;
    fips_index_allocator_init(&a, storage, 2, 3, entropy);
    fips_session_index idx = {99};
    uint32_t collision = (uint32_t)fips_splitmix64_next(&expected);
    CHECK(fips_index_allocator_reserve(&a, fips_session_index_new(collision)) == FIPS_INDEX_OK);
    uint32_t next = (uint32_t)fips_splitmix64_next(&expected);
    CHECK(fips_index_allocator_allocate(&a, &idx) == FIPS_INDEX_OK && idx.value == next);
    CHECK(t.calls == 3 && t.last == 4 && t.seed.state == expected.state);
    CHECK(fips_index_allocator_allocate(&a, &idx) == FIPS_INDEX_CAPACITY && idx.value == next);
    CHECK(t.calls == 3);
    CHECK(fips_index_allocator_reserve(&a, idx) == FIPS_INDEX_ALREADY_IN_USE);
    CHECK(fips_index_allocator_reserve(&a, fips_session_index_new(7)) == FIPS_INDEX_CAPACITY);
    CHECK(fips_index_allocator_free(&a, idx) == FIPS_INDEX_OK);
    a.max_attempts = 0;
    CHECK(fips_index_allocator_allocate(&a, &idx) == FIPS_INDEX_EXHAUSTED && t.calls == 3);
    fips_index_allocator_init(&a, NULL, 0, 100, entropy);
    CHECK(fips_index_allocator_allocate(&a, &idx) == FIPS_INDEX_CAPACITY && t.calls == 3);
    fips_fixed_entropy fixed = {1};
    fips_entropy fixed_source = fips_fixed_entropy_adapter(&fixed);
    fips_index_allocator_init(&a, storage, 2, 3, fixed_source);
    CHECK(fips_index_allocator_allocate(&a, &idx) == FIPS_INDEX_OK);
    CHECK(fips_index_allocator_free(&a, idx) == FIPS_INDEX_OK);
    CHECK(fips_index_allocator_allocate(&a, &idx) == FIPS_INDEX_OK && idx.value == 0x01010101);
    return 0;
}
static int allocator_exhaustion_draw_count(void)
{
    trace_entropy t = {{0}, 0, 0};
    fips_seeded_entropy copy = {0};
    fips_entropy entropy = {trace_fill, &t};
    uint32_t storage[4]; fips_index_allocator a;
    fips_index_allocator_init(&a, storage, 4, 3, entropy);
    for (size_t i = 0; i < 3; ++i) {
        fips_session_index index = {(uint32_t)fips_splitmix64_next(&copy)};
        CHECK(fips_index_allocator_reserve(&a, index) == FIPS_INDEX_OK);
    }
    fips_session_index out = {77};
    CHECK(fips_index_allocator_allocate(&a, &out) == FIPS_INDEX_EXHAUSTED && out.value == 77);
    CHECK(t.calls == 3 && t.last == 4 && t.seed.state == copy.state);
    CHECK(a.count == 3);
    return 0;
}
static int hash_table_adversarial_collisions(void)
{
    trace_entropy t = {{11}, 0, 0};
    fips_entropy entropy = {trace_fill, &t};
    fips_hash_slot slots[16], grown[32], small[4]; fips_hash_map m;
    CHECK(fips_hash_map_init(&m, slots, 3, entropy) == FIPS_ERR_RANGE && t.calls == 0);
    CHECK(fips_hash_map_init(&m, slots, 16, entropy) == FIPS_OK && t.calls == 1 && t.last == 16);
    CHECK(fips_hash_map_limit(&m) == 14);
    /* Deliberately grant the attacker knowledge of this TEST key and grind 15
     * distinct keys into bucket zero. A production attacker never has the key.
     * Exercise long chains, equality, deletion holes and bounded refusal. */
    uint8_t keys[15][4]; uint32_t candidate = 0; int values[15];
    for (size_t i = 0; i < 15; ++i) {
        do {
            fips_session_index index = {candidate++};
            fips_session_index_to_le_bytes(index, keys[i]);
        } while ((fips_siphash13(&m.hasher, keys[i], 4) & 15) != 0);
        values[i] = (int)i;
        if (i < 14) CHECK(fips_hash_map_insert(&m, keys[i], 4, &values[i]) == FIPS_OK);
    }
    CHECK(fips_hash_map_count(&m) == 14);
    CHECK(fips_hash_map_insert(&m, keys[14], 4, &values[14]) == FIPS_ERR_RANGE);
    CHECK(fips_hash_map_count(&m) == 14);
    void *value = &values[14];
    CHECK(!fips_hash_map_get(&m, keys[14], 4, &value) && value == &values[14]);
    CHECK(!fips_hash_map_remove(&m, keys[14], 4, &value) && value == &values[14]);
    CHECK(fips_hash_map_insert(&m, keys[13], 4, NULL) == FIPS_OK);
    CHECK(fips_hash_map_get(&m, keys[13], 4, &value) && value == NULL);
    CHECK(fips_hash_map_remove(&m, keys[0], 4, &value) && value == &values[0]);
    for (size_t i = 1; i < 13; ++i) {
        CHECK(fips_hash_map_get(&m, keys[i], 4, &value) && value == &values[i]);
    }
    CHECK(fips_hash_map_insert(&m, keys[14], 4, &values[14]) == FIPS_OK);
    memset(small, 0xa5, sizeof(small));
    unsigned char snapshot[sizeof(small)]; memcpy(snapshot, small, sizeof(small));
    CHECK(fips_hash_map_rehash(&m, small, 4) == FIPS_ERR_RANGE);
    CHECK(memcmp(small, snapshot, sizeof(small)) == 0 && m.slots == slots && m.count == 14);
    uint64_t k0 = m.hasher.k0, k1 = m.hasher.k1;
    CHECK(fips_hash_map_rehash(&m, grown, 32) == FIPS_OK);
    CHECK(t.calls == 1 && m.hasher.k0 == k0 && m.hasher.k1 == k1);
    for (size_t i = 0; i < 16; ++i) CHECK(slots[i].state == 0 && slots[i].key == NULL);
    for (size_t i = 1; i < 15; ++i) {
        CHECK(fips_hash_map_get(&m, keys[i], 4, &value));
        CHECK(value == (i == 13 ? NULL : &values[i]));
    }
    CHECK(fips_hash_map_insert(&m, NULL, 0, NULL) == FIPS_OK);
    CHECK(fips_hash_map_get(&m, NULL, 0, &value) && value == NULL);
    fips_hash_map_clear(&m);
    CHECK(m.count == 0 && m.hasher.k0 == k0);
    /* Churn leaves tombstones in every bucket; no empty-slot assumption. */
    uint8_t churn[4];
    for (uint32_t i = 0; i < 1024; ++i) {
        fips_session_index_to_le_bytes(fips_session_index_new(i), churn);
        CHECK(fips_hash_map_insert(&m, churn, 4, NULL) == FIPS_OK);
        CHECK(fips_hash_map_remove(&m, churn, 4, &value));
    }
    CHECK(!fips_hash_map_get(&m, churn, 4, &value));
    CHECK(fips_hash_map_insert(&m, NULL, 0, NULL) == FIPS_OK);
    fips_hash_map_deinit(&m);
    CHECK(m.hasher.k0 == 0 && m.hasher.k1 == 0 && m.slots == NULL && m.count == 0);
    return 0;
}
static int onlink_c_edges(void)
{
    fips_onlink_prefix in[] = {prefix("192.168.1.1", 255), prefix("2001:db8::1", 255)}, storage[2];
    fips_onlink_prefixes s = fips_onlink_failed();
    CHECK(fips_onlink_read_failed(&s) && fips_onlink_is_empty(&s));
    CHECK(!fips_onlink_contains(&s, ip("192.168.1.1")));
    CHECK(!fips_onlink_read_suspect(0, &s) && fips_onlink_read_suspect(1, &s));
    CHECK(fips_onlink_from_indexed(&s, storage, 2, in, 2) == FIPS_OK);
    CHECK(!fips_onlink_read_failed(&s) && storage[0].len == 32 && storage[1].len == 128);
    CHECK(fips_onlink_contains(&s, ip("192.168.1.1")));
    CHECK(!fips_onlink_contains(&s, ip("192.168.1.2")));
    CHECK(fips_onlink_contains(&s, ip("2001:db8::1")));
    CHECK(!fips_onlink_contains(&s, ip("2001:db8::2")));
    fips_onlink_prefixes saved = s;
    fips_onlink_prefix snapshot[2]; memcpy(snapshot, storage, sizeof(snapshot));
    CHECK(fips_onlink_from_pairs(&s, storage, 1, in, 2) == FIPS_ERR_RANGE);
    CHECK(s.entries == saved.entries && s.count == saved.count && s.failed == saved.failed);
    CHECK(memcmp(snapshot, storage, sizeof(storage)) == 0);
    in[1].net.width = 5;
    CHECK(fips_onlink_from_indexed(&s, storage, 2, in, 2) == FIPS_ERR_RANGE);
    CHECK(memcmp(snapshot, storage, sizeof(storage)) == 0);
    in[0] = prefix("2001:db8:8000::", 33);
    CHECK(fips_onlink_from_pairs(&s, storage, 2, in, 1) == FIPS_OK);
    CHECK(fips_onlink_contains(&s, ip("2001:db8:ffff::1")));
    CHECK(!fips_onlink_contains(&s, ip("2001:db8:7fff::1")));
    in[0] = prefix("::ffff:192.168.1.1", 120);
    CHECK(fips_onlink_from_pairs(&s, storage, 2, in, 1) == FIPS_OK);
    CHECK(storage[0].len == 32 && fips_onlink_contains(&s, ip("192.168.1.1")));
    CHECK(!fips_onlink_contains(&s, ip("192.168.1.2")));
    CHECK(fips_onlink_from_pairs(&s, NULL, 0, NULL, 0) == FIPS_OK);
    CHECK(fips_onlink_is_empty(&s) && !fips_onlink_read_failed(&s));
    return 0;
}
#define RUN(test) do { CHECK(test() == 0); ++tests; } while (0)
int main(void)
{
    unsigned tests = 0;
    RUN(test_session_index_roundtrip);
    RUN(test_session_index_display);
    RUN(test_allocator_basic);
    RUN(test_allocator_free_not_found);
    RUN(test_allocator_reserve);
    RUN(test_allocator_uniqueness);
    RUN(test_allocator_clear);
    RUN(test_allocator_reuse_after_free);
    RUN(allocator_draws_indices_from_its_entropy_source);
    RUN(keyed_hasher_takes_its_key_from_the_entropy_source);
    RUN(keyed_hasher_draws_both_key_halves_from_the_entropy_source);
    RUN(ipv4_prefixes_hold_exactly_their_range);
    RUN(ipv6_prefixes_hold_exactly_their_range);
    RUN(ipv4_mapped_addresses_are_judged_as_ipv4);
    RUN(a_zero_length_prefix_is_discarded_and_holds_nothing);
    RUN(families_never_match_each_other);
    RUN(share_link_needs_one_prefix_holding_both_addresses);
    RUN(contains_on_holds_only_the_named_interfaces_prefixes);
    RUN(a_discarded_zero_length_prefix_or_an_empty_read_is_reported);
    RUN(siphash_reference_vectors);
    RUN(entropy_replay_and_allocator_edges);
    RUN(allocator_exhaustion_draw_count);
    RUN(hash_table_adversarial_collisions);
    RUN(onlink_c_edges);
    printf("%u utils tests passed (19 upstream ports; 64 SipHash-1-3 vectors)\n", tests);
    return 0;
}
