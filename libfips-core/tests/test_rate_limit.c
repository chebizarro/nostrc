#include "fakes/host.h"
#include <fips/node/rate_limit.h>
#include <fips/proto/rate_limit.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); return 1; } } while (0)
#define STRANGER FIPS_MSG1_STRANGER
#define ESTABLISHED FIPS_MSG1_ESTABLISHED
#define ADMIT FIPS_MSG1_ADMIT
#define RATE FIPS_MSG1_RATE_LIMIT
#define PENDING FIPS_MSG1_PENDING_LIMIT
static fips_instant t(uint64_t ms)
{
    fips_test_clock state = {0};
    fips_clock clock = fips_test_clock_adapter(&state);
    (void)fips_test_clock_advance(&state, ms);
    return clock.now(clock.ctx);
}
static fips_token_bucket bucket(uint32_t cap, double rate)
{
    fips_token_bucket b;
    fips_token_bucket_init(&b, (fips_bucket_params){cap, rate}, t(0));
    return b;
}
static fips_handshake_rate_limiter limiter(fips_token_bucket b, size_t max)
{
    fips_handshake_rate_limiter l;
    fips_handshake_init(&l, b, bucket(1000, 100), max);
    return l;
}
static void addr(uint8_t a[16], unsigned int n)
{
    memset(a, 0, 16);
    a[0] = (uint8_t)(n >> 8);
    a[1] = (uint8_t)n;
}
/* Names correspond one-for-one to node/tests/rate_limit.rs (18 tests). */
static int test_token_bucket_basic(void)
{
    fips_token_bucket b = bucket(10, 1);
    CHECK(b.capacity == 10 && fips_token_bucket_tokens(&b, t(0)) >= 9.9);
    for (int i = 0; i < 10; ++i) CHECK(fips_token_bucket_acquire(&b, 1, t(0)));
    CHECK(!fips_token_bucket_acquire(&b, 1, t(0)));
    CHECK(fips_token_bucket_tokens(&b, t(0)) < 1);
    return 0;
}
static int test_token_bucket_refill(void)
{
    fips_token_bucket b = bucket(10, 100);
    for (int i = 0; i < 10; ++i) CHECK(fips_token_bucket_acquire(&b, 1, t(0)));
    CHECK(fips_token_bucket_tokens(&b, t(0)) < 1);
    CHECK(fips_token_bucket_tokens(&b, t(50)) == 5); /* deterministic clock */
    return 0;
}
static int test_token_bucket_try_acquire_n(void)
{
    fips_token_bucket b = bucket(10, 1);
    CHECK(fips_token_bucket_acquire(&b, 5, t(0)));
    CHECK(fips_token_bucket_tokens(&b, t(0)) == 5);
    CHECK(fips_token_bucket_acquire(&b, 5, t(0)));
    CHECK(!fips_token_bucket_acquire(&b, 1, t(0)));
    return 0;
}
static int test_token_bucket_reset(void)
{
    fips_token_bucket b = bucket(10, 1);
    for (int i = 0; i < 10; ++i) CHECK(fips_token_bucket_acquire(&b, 1, t(0)));
    fips_token_bucket_reset(&b, t(0));
    CHECK(fips_token_bucket_tokens(&b, t(0)) == 10);
    return 0;
}
static int test_token_bucket_time_until_available(void)
{
    fips_token_bucket b = bucket(10, 10);
    fips_duration wait;
    CHECK(fips_token_bucket_wait(&b, t(0), &wait) == FIPS_OK);
    CHECK(wait.secs == 0 && wait.nanos == 0);
    CHECK(fips_token_bucket_acquire(&b, 10, t(0)));
    CHECK(fips_token_bucket_wait(&b, t(0), &wait) == FIPS_OK);
    CHECK(wait.secs == 0 && wait.nanos == 100000000);
    return 0;
}
static int test_handshake_rate_limiter_basic(void)
{
    fips_handshake_rate_limiter l = limiter(bucket(100, 10), 100);
    CHECK(fips_handshake_can_start(&l, STRANGER, t(0)) && l.pending == 0);
    fips_pending_handshake slot = {0};
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &slot) == ADMIT && l.pending == 1);
    fips_pending_handshake_release(&slot);
    CHECK(l.pending == 0);
    return 0;
}
static int test_handshake_rate_limiter_max_pending(void)
{
    fips_handshake_rate_limiter l = limiter(bucket(1000, 100), 3);
    fips_pending_handshake slots[4] = {{0}};
    for (int i = 0; i < 3; ++i) CHECK(fips_handshake_start(&l, STRANGER, t(0), &slots[i]) == ADMIT);
    CHECK(!fips_handshake_can_start(&l, STRANGER, t(0)));
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &slots[3]) == PENDING);
    CHECK(slots[3].owner == NULL && l.stranger.tokens == 997);
    fips_pending_handshake_release(&slots[0]);
    CHECK(fips_handshake_can_start(&l, STRANGER, t(0)));
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &slots[3]) == ADMIT);
    for (int i = 0; i < 4; ++i) fips_pending_handshake_release(&slots[i]);
    CHECK(l.pending == 0);
    return 0;
}
static int test_handshake_rate_limiter_token_exhaustion(void)
{
    fips_handshake_rate_limiter l = limiter(bucket(5, 0), 100);
    fips_pending_handshake slot = {0};
    for (int i = 0; i < 5; ++i) {
        CHECK(fips_handshake_start(&l, STRANGER, t(0), &slot) == ADMIT);
        fips_pending_handshake_release(&slot);
    }
    CHECK(l.pending == 0 && !fips_handshake_can_start(&l, STRANGER, t(0)));
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &slot) == RATE);
    return 0;
}
static int test_handshake_rate_limiter_reset(void)
{
    fips_handshake_rate_limiter l = limiter(bucket(100, 10), 100);
    fips_pending_handshake a = {0}, b = {0};
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &a) == ADMIT);
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &b) == ADMIT && l.pending == 2);
    fips_handshake_reset(&l, t(0));
    CHECK(l.pending == 0 && l.stranger.tokens == FIPS_DEFAULT_BURST_CAPACITY);
    fips_pending_handshake_release(&a); fips_pending_handshake_release(&b);
    return 0;
}
static int pending_slot_releases_on_drop_and_saturates(void)
{
    fips_handshake_rate_limiter l = limiter(bucket(100, 100), 100);
    fips_pending_handshake outer = {0}, inner = {0};
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &outer) == ADMIT);
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &inner) == ADMIT && l.pending == 2);
    fips_pending_handshake_release(&inner);
    CHECK(l.pending == 1);
    fips_pending_handshake_release(&inner); /* C's explicit release is idempotent. */
    CHECK(l.pending == 1);
    fips_handshake_reset(&l, t(0));
    CHECK(l.pending == 0);
    fips_pending_handshake_release(&outer);
    CHECK(l.pending == 0);
    return 0;
}
static int refusal_reason_names_the_limb(void)
{
    fips_handshake_rate_limiter p = limiter(bucket(1000, 100), 1);
    fips_handshake_rate_limiter r = limiter(bucket(0, 0), 1000);
    fips_pending_handshake held = {0}, slot = {0};
    CHECK(fips_handshake_start(&p, STRANGER, t(0), &held) == ADMIT);
    CHECK(fips_handshake_start(&p, STRANGER, t(0), &slot) == PENDING);
    CHECK(fips_handshake_start(&r, STRANGER, t(0), &slot) == RATE);
    CHECK(strcmp(fips_msg1_result_name(PENDING), "pending_limit") == 0);
    CHECK(strcmp(fips_msg1_result_name(RATE), "rate_limit") == 0);
    fips_pending_handshake_release(&held);
    return 0;
}
static int msg1_classes_draw_on_separate_buckets(void)
{
    fips_handshake_rate_limiter l;
    fips_handshake_init(&l, bucket(1, 0), bucket(3, 0), 1000);
    fips_pending_handshake slot = {0};
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &slot) == ADMIT);
    fips_pending_handshake_release(&slot);
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &slot) == RATE);
    for (int i = 0; i < 3; ++i) {
        CHECK(fips_handshake_start(&l, ESTABLISHED, t(0), &slot) == ADMIT);
        fips_pending_handshake_release(&slot);
    }
    CHECK(fips_handshake_start(&l, ESTABLISHED, t(0), &slot) == RATE);
    return 0;
}
static int derive_established_bucket_from_shipped_defaults(void)
{
    fips_bucket_params p = fips_derive_established_bucket(128, 120, 5, (fips_bucket_params){100, 10});
    CHECK(p.burst == 128 && fabs(p.rate - 6.4) < 1e-9);
    p = fips_derive_established_bucket(512, 120, 5, (fips_bucket_params){100, 10});
    CHECK(p.burst == 512 && fabs(p.rate - 25.6) < 1e-9);
    return 0;
}
static int derive_established_bucket_degenerate_rekey_periods(void)
{
    fips_bucket_params p = fips_derive_established_bucket(128, 0, 5, (fips_bucket_params){100, 10});
    CHECK(p.burst == 128 && isfinite(p.rate) && p.rate > FIPS_ESTABLISHED_RATE_FLOOR);
    CHECK(p.rate == fips_derive_established_bucket(128, 1, 5, (fips_bucket_params){100, 10}).rate);
    CHECK(fips_derive_established_bucket(1, 100000, 5, (fips_bucket_params){100, 10}).rate == 1);
    return 0;
}
static int derive_established_bucket_unlimited_peers_reuses_stranger_params(void)
{
    fips_bucket_params p = fips_derive_established_bucket(0, 120, 5, (fips_bucket_params){100, 10});
    CHECK(p.burst == 100 && p.rate == 10);
    return 0;
}
#define SETUP(c, ms, a, expected) do { bool result = !(expected); \
    CHECK(fips_session_setup_admit(&l, a, c, t(ms), &result) == FIPS_OK); CHECK(result == (expected)); } while (0)
static int setup_limiter_draining_one_link_leaves_another_links_budget_untouched(void)
{
    fips_setup_entry storage[2]; fips_session_setup_limiter l;
    uint8_t noisy[16], quiet[16]; addr(noisy, 1); addr(quiet, 2);
    fips_session_setup_init(&l, storage, 2, (fips_bucket_params){2, .001}, (fips_bucket_params){2, .001});
    SETUP(STRANGER, 0, noisy, true); SETUP(STRANGER, 0, noisy, true);
    SETUP(STRANGER, 0, noisy, false); SETUP(STRANGER, 0, quiet, true);
    CHECK(l.len == 2);
    return 0;
}
static int setup_limiter_draining_the_stranger_bucket_still_admits_established_peer_setups(void)
{
    fips_setup_entry storage[1]; fips_session_setup_limiter l;
    uint8_t link[16]; addr(link, 1);
    fips_session_setup_init(&l, storage, 1, (fips_bucket_params){1, .001}, (fips_bucket_params){1, .001});
    SETUP(STRANGER, 0, link, true); SETUP(STRANGER, 0, link, false);
    SETUP(ESTABLISHED, 0, link, true);
    return 0;
}
static int setup_buckets_are_pruned_after_the_idle_period(void)
{
    fips_setup_entry storage[2]; fips_session_setup_limiter l;
    uint8_t idle[16], busy[16]; addr(idle, 1); addr(busy, 2);
    fips_session_setup_init(&l, storage, 2, (fips_bucket_params){10, 1}, (fips_bucket_params){10, 1});
    SETUP(STRANGER, 0, idle, true); SETUP(STRANGER, 299999, busy, true);
    CHECK(l.len == 2);
    SETUP(STRANGER, 300000, busy, true);
    CHECK(l.len == 1);
    return 0;
}
/* node/tests/rate_limit_sign.rs (2 tests). */
static int test_sign_budget_is_spent_per_peer_and_does_not_touch_another_peer(void)
{
    fips_sign_entry storage[2]; fips_lookup_sign_limiter l;
    uint8_t a[16], b[16]; addr(a, 1); addr(b, 2);
    bool result;
    fips_lookup_sign_init(&l, storage, 2, 4, 0);
    for (int i = 0; i < 4; ++i) {
        CHECK(fips_lookup_should_sign(&l, a, t(0), &result) == FIPS_OK && result);
    }
    CHECK(fips_lookup_should_sign(&l, a, t(0), &result) == FIPS_OK && !result);
    CHECK(fips_lookup_should_sign(&l, b, t(0), &result) == FIPS_OK && result);
    CHECK(l.len == 2);
    return 0;
}
static int test_sign_budget_of_zero_burst_is_read_as_unlimited(void)
{
    fips_lookup_sign_limiter l; uint8_t a[16]; addr(a, 1); bool result;
    fips_lookup_sign_init(&l, NULL, 0, 0, 0);
    for (int i = 0; i < 1000; ++i)
        CHECK(fips_lookup_should_sign(&l, a, t(0), &result) == FIPS_OK && result);
    CHECK(l.len == 0);
    return 0;
}
/* Additional capacity, cleanup, vtable and numerical edges. */
static int token_bucket_edges(void)
{
    fips_token_bucket b = bucket(1, 2);
    CHECK(fips_token_bucket_acquire(&b, 0, t(0)));
    CHECK(!fips_token_bucket_acquire(&b, 2, t(0)) && b.tokens == 1);
    CHECK(fips_token_bucket_acquire(&b, 1, t(1000)));
    CHECK(fips_token_bucket_tokens(&b, t(500)) == 0); /* backwards saturates */
    CHECK(fips_token_bucket_tokens(&b, t(750)) == .5); /* baseline still updated */
    CHECK(fips_token_bucket_tokens(&b, t(UINT64_MAX)) == 1);
    fips_duration wait = {123, 456};
    b = bucket(0, 0);
    CHECK(fips_token_bucket_wait(&b, t(0), &wait) == FIPS_ERR_RANGE);
    CHECK(wait.secs == 123 && wait.nanos == 456);
    b = bucket(0, 1e-30);
    CHECK(fips_token_bucket_wait(&b, t(0), &wait) == FIPS_ERR_RANGE);
    CHECK(wait.secs == 123 && wait.nanos == 456);
    fips_bucket_params p = fips_derive_established_bucket(SIZE_MAX, 1, UINT32_MAX, (fips_bucket_params){1, 1});
    CHECK(p.burst == (SIZE_MAX > UINT32_MAX ? UINT32_MAX : (uint32_t)SIZE_MAX));
    return 0;
}
static int shared_pending_and_reset_slots(void)
{
    fips_handshake_rate_limiter l;
    fips_handshake_init(&l, bucket(10, 0), bucket(10, 0), 1);
    fips_pending_handshake a = {0}, b = {0};
    CHECK(fips_handshake_start(&l, STRANGER, t(0), &a) == ADMIT);
    CHECK(fips_handshake_start(&l, ESTABLISHED, t(0), &b) == PENDING);
    CHECK(l.established.tokens == 10);
    fips_handshake_reset(&l, t(0));
    CHECK(fips_handshake_start(&l, ESTABLISHED, t(0), &b) == ADMIT);
    fips_pending_handshake_release(&a); /* Upstream reset permits old guard to release newer count. */
    CHECK(l.pending == 0);
    fips_pending_handshake_release(&b);
    CHECK(l.pending == 0);
    return 0;
}
static int setup_capacity_and_denial_cleanup(void)
{
    fips_setup_entry storage[2]; fips_session_setup_limiter l;
    uint8_t a[16], b[16], c[16]; addr(a, 2); addr(b, 1); addr(c, 3);
    fips_session_setup_init(&l, storage, 2, (fips_bucket_params){1, 0}, (fips_bucket_params){1, 0});
    SETUP(STRANGER, 0, a, true); SETUP(STRANGER, 0, b, true);
    CHECK(memcmp(storage[0].addr, b, 16) == 0); /* sorted insertion */
    bool out = true;
    CHECK(fips_session_setup_admit(&l, c, STRANGER, t(300000), &out) == FIPS_ERR_RANGE && out);
    CHECK(l.len == 2);
    SETUP(STRANGER, 300000, b, false); /* denial refreshes b but must not sweep a */
    CHECK(l.len == 2);
    fips_session_setup_cleanup(&l, t(300000));
    CHECK(l.len == 1);
    SETUP(STRANGER, 300000, c, true); CHECK(l.len == 2);
    fips_session_setup_init(&l, NULL, 0, (fips_bucket_params){1, 1}, (fips_bucket_params){1, 1});
    CHECK(fips_session_setup_admit(&l, a, STRANGER, t(0), &out) == FIPS_ERR_RANGE && out);
    return 0;
}
static int sign_refill_capacity_and_cleanup(void)
{
    fips_sign_entry storage[2]; fips_lookup_sign_limiter l;
    uint8_t a[16], b[16], c[16]; addr(a, 2); addr(b, 1); addr(c, 3); bool out;
    fips_lookup_sign_init(&l, storage, 2, 1, 2);
    CHECK(fips_lookup_should_sign(&l, a, t(0), &out) == FIPS_OK && out);
    CHECK(fips_lookup_should_sign(&l, a, t(499), &out) == FIPS_OK && !out);
    CHECK(fips_lookup_should_sign(&l, a, t(500), &out) == FIPS_OK && out);
    CHECK(fips_lookup_should_sign(&l, b, t(500), &out) == FIPS_OK && out);
    CHECK(memcmp(storage[0].addr, b, 16) == 0);
    out = false;
    CHECK(fips_lookup_should_sign(&l, c, t(500), &out) == FIPS_ERR_RANGE && !out);
    fips_lookup_sign_cleanup(&l, t(300499)); CHECK(l.len == 2);
    fips_lookup_sign_cleanup(&l, t(300500)); CHECK(l.len == 0);
    CHECK(fips_lookup_should_sign(&l, c, t(300500), &out) == FIPS_OK && out);
    fips_lookup_sign_init(&l, storage, 2, 1, 0);
    CHECK(fips_lookup_should_sign(&l, a, t(0), &out) == FIPS_OK && out);
    CHECK(fips_lookup_should_sign(&l, b, t(1), &out) == FIPS_OK && out);
    CHECK(fips_lookup_should_sign(&l, b, t(300000), &out) == FIPS_OK && !out);
    CHECK(l.len == 2); /* refusal does not clean up */
    CHECK(fips_lookup_should_sign(&l, c, t(300000), &out) == FIPS_ERR_RANGE && !out);
    fips_lookup_sign_cleanup(&l, t(300000)); CHECK(l.len == 1);
    CHECK(fips_lookup_should_sign(&l, c, t(300000), &out) == FIPS_OK && out);
    fips_lookup_sign_init(&l, NULL, 0, -1, 0);
    CHECK(fips_lookup_should_sign(&l, a, t(0), &out) == FIPS_OK && out && l.len == 0);
    fips_lookup_sign_init(&l, NULL, 0, FIPS_DEFAULT_SIGN_BURST, FIPS_DEFAULT_SIGN_RATE);
    CHECK(fips_lookup_should_sign(&l, a, t(0), &out) == FIPS_ERR_RANGE && out);
    return 0;
}
static fips_per_addr_rate_limiter per_addr;
static int proto_interval_capacity_and_sweeps(void)
{
    uint8_t a[16]; addr(a, 1);
    fips_per_addr_init(&per_addr, 100, 8000, t(0));
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(0)) == FIPS_RECORD_ADMIT);
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(99)) == FIPS_RECORD_SUPPRESS);
    CHECK(per_addr.sweeps == 1);
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(100)) == FIPS_RECORD_ADMIT);
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(50)) == FIPS_RECORD_SUPPRESS);
    CHECK(per_addr.sweeps == 2); /* zero last-sweep sentinel mirrors Rust */
    fips_per_addr_init(&per_addr, 10, 8000, t(0));
    for (unsigned int i = 0; i < FIPS_PER_ADDR_MAX_ENTRIES; ++i) {
        addr(a, i);
        CHECK(fips_per_addr_check_and_record(&per_addr, a, t(1)) == FIPS_RECORD_ADMIT);
    }
    CHECK(per_addr.len == 4096 && per_addr.sweeps == 1);
    addr(a, 4096);
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(1)) == FIPS_RECORD_ADMIT_AT_CAPACITY);
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(2)) == FIPS_RECORD_ADMIT_AT_CAPACITY);
    addr(a, 0);
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(11)) == FIPS_RECORD_ADMIT);
    fips_per_addr_cleanup(&per_addr, t(8000)); CHECK(per_addr.len == 4096);
    fips_per_addr_cleanup(&per_addr, t(8001)); CHECK(per_addr.len == 1);
    fips_per_addr_cleanup(&per_addr, t(8011)); CHECK(per_addr.len == 0);
    /* Amortized sweeps, including a known address expiring in its own sweep. */
    fips_per_addr_init(&per_addr, 0, 800, t(0));
    for (uint64_t i = 1; i <= 1000; ++i)
        CHECK(fips_per_addr_check_and_record(&per_addr, a, t(i)) == FIPS_RECORD_ADMIT);
    CHECK(per_addr.sweeps == 10 && per_addr.len == 1);
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(1800)) == FIPS_RECORD_ADMIT && per_addr.len == 1);
    fips_per_addr_init(&per_addr, 0, 0, t(0));
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(0)) == FIPS_RECORD_ADMIT);
    CHECK(fips_per_addr_check_and_record(&per_addr, a, t(0)) == FIPS_RECORD_ADMIT && per_addr.len == 1);
    fips_per_addr_cleanup(&per_addr, t(0)); CHECK(per_addr.len == 0);
    return 0;
}
static int proto_backoff_boundaries(void)
{
    CHECK(fips_backoff_ms(0, 30, 600) == 30);
    CHECK(fips_backoff_ms(4, 30, 600) == 480);
    CHECK(fips_backoff_ms(5, 30, 600) == 600);
    CHECK(fips_backoff_ms(63, 1, UINT64_MAX) == (UINT64_C(1) << 63));
    CHECK(fips_backoff_ms(64, 1, UINT64_MAX) == UINT64_MAX);
    CHECK(fips_backoff_ms(UINT32_MAX, 0, UINT64_MAX) == 0);
    CHECK(fips_backoff_ms(1, UINT64_MAX, UINT64_MAX) == UINT64_MAX);
    CHECK(fips_backoff_ms(0, 10, 0) == 0);
    return 0;
}
/* Precision-only adapter: TestClock deliberately drops sub-millisecond time. */
static int ns_compare(fips_instant a, fips_instant b)
{
    return a.opaque[0] < b.opaque[0] ? -1 : a.opaque[0] > b.opaque[0];
}
static fips_duration ns_since(fips_instant a, fips_instant b)
{
    uint64_t n = a.opaque[0] < b.opaque[0] ? 0 : a.opaque[0] - b.opaque[0];
    return (fips_duration){n / 1000000000, (uint32_t)(n % 1000000000)};
}
static fips_status ns_offset(fips_instant a, fips_duration d, fips_instant *out, bool add)
{
    if (d.secs > UINT64_MAX / 1000000000) return FIPS_ERR_RANGE;
    uint64_t n = d.secs * 1000000000;
    if (d.nanos > UINT64_MAX - n) return FIPS_ERR_RANGE;
    n += d.nanos;
    if (add ? n > UINT64_MAX - a.opaque[0] : n > a.opaque[0]) return FIPS_ERR_RANGE;
    a.opaque[0] = add ? a.opaque[0] + n : a.opaque[0] - n;
    *out = a;
    return FIPS_OK;
}
static fips_status ns_add(fips_instant a, fips_duration d, fips_instant *out)
{ return ns_offset(a, d, out, true); }
static fips_status ns_sub(fips_instant a, fips_duration d, fips_instant *out)
{ return ns_offset(a, d, out, false); }
static fips_instant ns(uint64_t n)
{
    static const fips_instant_ops ops = {ns_compare, ns_since, ns_add, ns_sub};
    return (fips_instant){{n, 0}, &ops};
}
static int proto_quantizes_before_subtraction(void)
{
    uint8_t a[16], b[16]; addr(a, 1); addr(b, 2);
    fips_per_addr_init(&per_addr, 1, 8, ns(123));
    CHECK(fips_per_addr_check_and_record(&per_addr, a, ns(900123)) == FIPS_RECORD_ADMIT);
    CHECK(per_addr.last_sweep_ms == 0);
    CHECK(fips_per_addr_check_and_record(&per_addr, b, ns(950123)) == FIPS_RECORD_ADMIT);
    CHECK(per_addr.sweeps == 2); /* Both quantize to zero: repeat sentinel sweep. */
    CHECK(fips_per_addr_check_and_record(&per_addr, a, ns(1000123)) == FIPS_RECORD_ADMIT);
    CHECK(per_addr.entries[0].last_ms == 1 && per_addr.last_sweep_ms == 1);
    CHECK(fips_per_addr_check_and_record(&per_addr, a, ns(1999123)) == FIPS_RECORD_SUPPRESS);
    fips_per_addr_cleanup(&per_addr, ns(8000123));
    CHECK(per_addr.len == 1); /* b at 0 expires; a at 1 remains. */
    fips_per_addr_cleanup(&per_addr, ns(9000123)); CHECK(per_addr.len == 0);
    return 0;
}
static int token_wait_rounds_exact_binary_fraction_ties_even(void)
{
    fips_duration wait;
    fips_token_bucket b = bucket(0, 1024);
    CHECK(fips_token_bucket_wait(&b, t(0), &wait) == FIPS_OK);
    CHECK(wait.secs == 0 && wait.nanos == 976562); /* 976562.5 -> even */
    b = bucket(0, 1024.0 / 3.0);
    CHECK(fips_token_bucket_wait(&b, t(0), &wait) == FIPS_OK);
    CHECK(wait.secs == 0 && wait.nanos == 2929688); /* 2929687.5 -> even */
    for (unsigned int k = 1; k < 100; ++k) {
        b = bucket(0, ldexp(1.0, (int)k));
        CHECK(fips_token_bucket_wait(&b, t(0), &wait) == FIPS_OK);
        uint64_t expected = 0;
        if (k < 64) {
            uint64_t divisor = UINT64_C(1) << k;
            expected = UINT64_C(1000000000) / divisor;
            uint64_t remainder = UINT64_C(1000000000) % divisor;
            if (remainder > divisor / 2 || (remainder == divisor / 2 && (expected & 1))) ++expected;
        }
        CHECK(wait.secs == 0 && wait.nanos == expected);
    }
    return 0;
}
int main(void)
{
    int (*tests[])(void) = {
        test_token_bucket_basic, test_token_bucket_refill, test_token_bucket_try_acquire_n,
        test_token_bucket_reset, test_token_bucket_time_until_available,
        test_handshake_rate_limiter_basic, test_handshake_rate_limiter_max_pending,
        test_handshake_rate_limiter_token_exhaustion, test_handshake_rate_limiter_reset,
        pending_slot_releases_on_drop_and_saturates, refusal_reason_names_the_limb,
        msg1_classes_draw_on_separate_buckets, derive_established_bucket_from_shipped_defaults,
        derive_established_bucket_degenerate_rekey_periods,
        derive_established_bucket_unlimited_peers_reuses_stranger_params,
        setup_limiter_draining_one_link_leaves_another_links_budget_untouched,
        setup_limiter_draining_the_stranger_bucket_still_admits_established_peer_setups,
        setup_buckets_are_pruned_after_the_idle_period,
        test_sign_budget_is_spent_per_peer_and_does_not_touch_another_peer,
        test_sign_budget_of_zero_burst_is_read_as_unlimited,
        token_bucket_edges, shared_pending_and_reset_slots, setup_capacity_and_denial_cleanup,
        sign_refill_capacity_and_cleanup, proto_interval_capacity_and_sweeps, proto_backoff_boundaries,
        proto_quantizes_before_subtraction, token_wait_rounds_exact_binary_fraction_ties_even
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) if (tests[i]()) return 1;
    puts("rate limit: 20 upstream node tests + 8 edge/proto tests passed");
    return 0;
}
