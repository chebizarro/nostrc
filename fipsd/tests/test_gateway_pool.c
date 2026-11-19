/* Ported from upstream src/gateway/pool.rs tests (nostrc-iq1n9.3): the pool's
 * allocation, limits, restart holds, durable marks, conntrack evidence and
 * the mapping lifecycle, driven by a caller-supplied clock. */
#include "test_gateway.h"
#include "log.h"

/* Measure execution, not the synthetic instants that drive pool behaviour. */
#undef RUN
#define RUN(fn) do { \
    fipsd_gw_instant begin_ = fipsd_gw_now(); \
    fprintf(stderr, "  %s\n", #fn); \
    fn(); \
    fprintf(stderr, "  %s: %.3f s\n", #fn, \
            (double)fipsd_gw_since(fipsd_gw_now(), begin_) / (double)FIPSD_GW_SEC); \
} while (0)

static fipsd_gw_pool *pool_new(const char *cidr, uint64_t ttl, uint64_t grace)
{ fipsd_gw_pool_error e; fipsd_gw_pool *p = fipsd_gw_pool_new(cidr, ttl, grace, &e); CHECK(p); return p; }
static fipsd_gw_pool *limited(const char *cidr, uint64_t ttl, uint64_t grace, size_t ceiling, uint32_t burst, uint32_t rate)
{ fipsd_gw_pool_error e; fipsd_gw_pool *p = fipsd_gw_pool_with_limits(cidr, ttl, grace, ceiling, burst, rate, &e); CHECK(p); return p; }
/* A /120 pool with the given limits, TTL and grace of 60 s. */
static fipsd_gw_pool *limited_pool(size_t ceiling, uint32_t burst, uint32_t rate) { return limited("fd01::/120", 60, 60, ceiling, burst, rate); }
/* A /120 pool with TTL and grace ttl seconds and a rate never to refuse. */
static fipsd_gw_pool *fast_pool(size_t ceiling, uint64_t ttl) { return limited("fd01::/120", ttl, ttl, ceiling, 10000, 10000); }
static fipsd_gw_pool *started(const char *cidr, uint64_t ttl, uint64_t grace, const fipsd_gw_pool_begin *b, fipsd_gw_instant now)
{ fipsd_gw_pool_error e; fipsd_gw_pool *p = fipsd_gw_pool_start(cidr, ttl, grace, b, now, &e); CHECK(p); return p; }
static fipsd_gw_pool_begin fresh(uint32_t offset) { fipsd_gw_pool_begin b; memset(&b, 0, sizeof b); b.offset = offset; return b; }
static fipsd_gw_pool_begin restored(const fipsd_gw_pool_state *s) { fipsd_gw_pool_begin b; memset(&b, 0, sizeof b); b.restored = true; b.state = *s; return b; }

static fipsd_gw_pool_error alloc_at(fipsd_gw_pool *p, const fips_node_addr *node, const struct in6_addr *mesh, fipsd_gw_instant now,
                                    fipsd_gw_allocation *out)
{ return fipsd_gw_pool_allocate_at(p, node, mesh, "test.fips", now, out); }
/* Allocate node i at now. */
static fipsd_gw_pool_error alloc(fipsd_gw_pool *p, uint8_t i, fipsd_gw_instant now, struct in6_addr *vip, bool *is_new)
{
    fips_node_addr n = gw_node(i); struct in6_addr m = gw_mesh(i);
    fipsd_gw_allocation a;
    fipsd_gw_pool_error e = alloc_at(p, &n, &m, now, &a);
    if (!e) { if (vip) *vip = a.virtual_ip; if (is_new) *is_new = a.is_new; }
    return e;
}
static struct in6_addr alloc_ok(fipsd_gw_pool *p, uint8_t i, fipsd_gw_instant now)
{ struct in6_addr v; fipsd_gw_pool_error e = alloc(p, i, now, &v, NULL); CHECKM(e == FIPSD_GW_POOL_OK, "error %d", e); return v; }
static int tick(fipsd_gw_pool *p, fipsd_gw_instant now, const gw_sessions *s, fipsd_gw_events *events)
{
    fipsd_gw_ct_snapshot snap; gw_sessions_snapshot(s, &snap);
    fipsd_gw_events local = {0};
    int rc = fipsd_gw_pool_tick(p, now, &snap, events ? events : &local);
    fipsd_gw_ct_snapshot_clear(&snap); fipsd_gw_events_clear(&local);
    return rc;
}
static size_t tick_events(fipsd_gw_pool *p, fipsd_gw_instant now, const gw_sessions *s)
{ fipsd_gw_events ev = {0}; CHECK(tick(p, now, s, &ev) == 0); size_t n = ev.len; fipsd_gw_events_clear(&ev); return n; }
static void tick_unread(fipsd_gw_pool *p, fipsd_gw_instant now)
{ fipsd_gw_ct_snapshot snap; fipsd_gw_ct_snapshot_init(&snap); fipsd_gw_events ev = {0}; CHECK(fipsd_gw_pool_tick(p, now, &snap, &ev) == 0); fipsd_gw_events_clear(&ev); }
static const fipsd_gw_mapping *mapping(fipsd_gw_pool *p, uint8_t i) { fips_node_addr n = gw_node(i); return fipsd_gw_pool_lookup_node(p, &n); }
/* Mark the mapping for node i used, as a reply from its mesh address read on a tick at now would. */
static void mark_used(fipsd_gw_pool *p, uint8_t i, fipsd_gw_instant now)
{
    const fipsd_gw_mapping *m = mapping(p, i); CHECK(m);
    gw_sessions replies = {0}; gw_sessions_bind(&replies, m->virtual_ip, gw_mesh(i), true);
    CHECK(tick(p, now, &replies, NULL) == 0);
    CHECKM(mapping(p, i)->used, "control: marked used");
}
static uint32_t offset_of(const fipsd_gw_pool *p, const struct in6_addr *a) { uint32_t o = 0; CHECK(fipsd_gw_pool_test_addr_offset(p, a, &o)); return o; }
static bool state_mark(fipsd_gw_pool *p, fipsd_gw_pool_state *out) { memset(out, 0, sizeof *out); return fipsd_gw_pool_mark_request(p, out); }

/* ---- a pool driven the way the gateway drives it -------------------------- */
typedef struct sim {
    fipsd_gw_pool *pool;
    fipsd_gw_instant now, next_tick;
    gw_sessions pinned;
    fipsd_gw_pool_state written; /* the last state written, as a crash would leave it */
} sim;
static void sim_started(sim *s, const char *cidr, uint64_t ttl, uint64_t grace, const fipsd_gw_pool_begin *b, fipsd_gw_instant now)
{
    memset(s, 0, sizeof *s);
    s->pool = started(cidr, ttl, grace, b, now);
    CHECKM(state_mark(s->pool, &s->written), "a started pool asks for a write");
    fipsd_gw_pool_confirm_mark(s->pool, &s->written);
    s->now = now; s->next_tick = now + SECS(10);
}
static void sim_free(sim *s) { fipsd_gw_pool_free(s->pool); fipsd_gw_pool_state_clear(&s->written); }
static void sim_tick(sim *s)
{
    fipsd_gw_events ev = {0};
    CHECK(tick(s->pool, s->now, &s->pinned, &ev) == 0);
    for (size_t i = 0; i < ev.len; ++i)
        if (ev.items[i].kind == FIPSD_GW_EVENT_MAPPING_REMOVED) fipsd_gw_pool_nat_removed(s->pool, &ev.items[i].virtual_ip, &ev.items[i].mesh_addr);
    fipsd_gw_events_clear(&ev);
    fipsd_gw_pool_state st;
    if (state_mark(s->pool, &st)) { fipsd_gw_pool_confirm_mark(s->pool, &st); fipsd_gw_pool_state_clear(&s->written); s->written = st; }
}
/* Move time on, ticking at every 10 s boundary passed. */
static void sim_advance(sim *s, fipsd_gw_instant by)
{
    fipsd_gw_instant until = s->now + by;
    while (s->next_tick <= until) { s->now = s->next_tick; sim_tick(s); s->next_tick += SECS(10); }
    s->now = until;
}
static fipsd_gw_pool_error sim_alloc(sim *s, uint32_t i, fipsd_gw_allocation *out)
{
    fips_node_addr n = gw_node_n(i); struct in6_addr m = gw_mesh_n(i);
    fipsd_gw_allocation a;
    fipsd_gw_pool_error e = alloc_at(s->pool, &n, &m, s->now, &a);
    if (!e && a.evicted.present) fipsd_gw_pool_nat_removed(s->pool, &a.evicted.virtual_ip, &a.evicted.mesh_addr);
    if (out) *out = a;
    return e;
}
/* Allocate name i, waiting out the rate limit and the mark. */
static fipsd_gw_allocation sim_alloc_waiting(sim *s, uint32_t i)
{
    for (int k = 0; k < 1000; ++k) {
        fipsd_gw_allocation a;
        fipsd_gw_pool_error e = sim_alloc(s, i, &a);
        if (!e) return a;
        CHECKM(e == FIPSD_GW_POOL_RATE_LIMITED || e == FIPSD_GW_POOL_AWAITING_MARK, "name %u refused: %s", i, fipsd_gw_pool_error_reason(e));
        sim_advance(s, MILLIS(100));
    }
    CHECKM(false, "name %u still refused after 100 s", i);
    abort();
}
static uint32_t sim_offset(const sim *s, struct in6_addr a) { return offset_of(s->pool, &a); }

/* ---- tests --------------------------------------------------------------- */
static void pool_error_reasons_are_the_kebab_case_log_values(void)
{
    CHECK(!strcmp(fipsd_gw_pool_error_reason(FIPSD_GW_POOL_AT_CEILING), "ceiling"));
    CHECK(!strcmp(fipsd_gw_pool_error_reason(FIPSD_GW_POOL_RATE_LIMITED), "rate-limited"));
    CHECK(!strcmp(fipsd_gw_pool_error_reason(FIPSD_GW_POOL_EXHAUSTED), "exhausted"));
    CHECK(!strcmp(fipsd_gw_pool_error_reason(FIPSD_GW_POOL_AWAITING_MARK), "awaiting-mark"));
    CHECK(!strcmp(fipsd_gw_pool_error_reason(FIPSD_GW_POOL_INSIDE_POOL), "inside-pool"));
    CHECK(!strcmp(fipsd_gw_pool_error_reason(FIPSD_GW_POOL_INVALID_CIDR), "invalid"));
    CHECK(!strcmp(fipsd_gw_pool_error_reason(FIPSD_GW_POOL_INVALID_PREFIX), "invalid"));
    char buf[160]; struct in6_addr a = gw_v6("fd00::1");
    CHECK(!strcmp(fipsd_gw_pool_error_text(FIPSD_GW_POOL_EXHAUSTED, 3, NULL, NULL, buf, sizeof buf), "pool exhausted (3 addresses in use)"));
    CHECK(!strcmp(fipsd_gw_pool_error_text(FIPSD_GW_POOL_INSIDE_POOL, 0, &a, NULL, buf, sizeof buf), "mesh address fd00::1 lies inside the pool prefix"));
}
static void test_parse_cidr(void)
{
    struct in6_addr a; uint32_t p;
    CHECK(fipsd_gw_cidr_parse("fd01::/112", &a, &p));
    struct in6_addr want = gw_v6("fd01::"); CHECK(fipsd_gw_ip6_eq(&a, &want)); CHECK(p == 112);
}
static void test_parse_cidr_invalid(void)
{
    struct in6_addr a; uint32_t p;
    CHECK(!fipsd_gw_cidr_parse("not-a-cidr", &a, &p));
    CHECK(!fipsd_gw_cidr_parse("fd01::", &a, &p));
    CHECK(!fipsd_gw_cidr_parse("fd01::/abc", &a, &p));
}
static void a_pool_cidr_with_host_bits_set_issues_only_addresses_inside_its_prefix(void)
{
    fipsd_gw_pool *p = limited("fd01::1/112", 60, 60, 10, 10, 10);
    struct in6_addr net; uint8_t len; fipsd_gw_pool_network(p, &net, &len);
    struct in6_addr want = gw_v6("fd01::");
    CHECKM(fipsd_gw_ip6_eq(&net, &want) && len == 112, "the pool's network is the configured address's prefix");
    uint32_t offsets[] = {1, fipsd_gw_pool_test_total(p)};
    for (size_t i = 0; i < 2; ++i) {
        struct in6_addr a; fipsd_gw_pool_test_offset_addr(p, offsets[i], &a);
        CHECKM(fipsd_gw_ip6_in_prefix(&a, &want, 112), "offset %u is outside fd01::/112", offsets[i]);
    }
    fipsd_gw_pool_free(p);
}
static void a_pool_with_no_addresses_is_refused_at_start(void)
{
    uint32_t total;
    CHECKM(fipsd_gw_pool_total("fd01::1/128", &total) == FIPSD_GW_POOL_INVALID_PREFIX, "a /128 pool was accepted");
    fipsd_gw_pool_begin b = fresh(1); fipsd_gw_pool_error e;
    CHECK(!fipsd_gw_pool_start("fd01::1/128", 60, 60, &b, gw_t0(), &e));
    CHECK(fipsd_gw_pool_total("fd01::/127", &total) == FIPSD_GW_POOL_OK && total == 1);
}
static void test_pool_creation(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 60, 60);
    CHECK(fipsd_gw_pool_test_total(p) == 255); CHECK(fipsd_gw_pool_test_free_len(p) == 255);
    fipsd_gw_pool_free(p);
}
static void test_pool_allocation(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 60, 60);
    struct in6_addr v; bool is_new;
    CHECK(alloc(p, 1, gw_t0(), &v, &is_new) == FIPSD_GW_POOL_OK);
    struct in6_addr want = gw_v6("fd01::1");
    CHECK(is_new); CHECK(fipsd_gw_ip6_eq(&v, &want)); CHECK(fipsd_gw_pool_test_free_len(p) == 254);
    fipsd_gw_pool_free(p);
}
static void test_pool_idempotent(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 60, 60);
    struct in6_addr v1, v2; bool n1, n2; fipsd_gw_instant t = gw_t0();
    CHECK(alloc(p, 1, t, &v1, &n1) == 0); CHECK(alloc(p, 1, t, &v2, &n2) == 0);
    CHECK(n1); CHECK(!n2); CHECK(fipsd_gw_ip6_eq(&v1, &v2)); CHECK(fipsd_gw_pool_test_free_len(p) == 254);
    fipsd_gw_pool_free(p);
}
static void test_pool_exhaustion(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/126", 60, 60);
    CHECK(fipsd_gw_pool_test_total(p) == 3);
    fipsd_gw_instant t = gw_t0();
    for (uint8_t i = 1; i <= 3; ++i) alloc_ok(p, i, t);
    CHECK(alloc(p, 4, t, NULL, NULL) != FIPSD_GW_POOL_OK);
    fipsd_gw_pool_free(p);
}
static void ceiling_refuses_only_when_every_mapping_has_carried_traffic(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = limited_pool(4, 10, 1);
    struct in6_addr vips[4];
    for (uint8_t i = 1; i <= 4; ++i) vips[i - 1] = alloc_ok(p, i, t0);
    for (uint8_t i = 1; i <= 4; ++i) mark_used(p, i, t0);
    CHECK(fipsd_gw_pool_test_tokens(p) == 6);
    CHECKM(alloc(p, 5, t0, NULL, NULL) == FIPSD_GW_POOL_AT_CEILING, "a fifth new name must be refused when every mapping carries traffic");
    CHECKM(fipsd_gw_pool_test_tokens(p) == 6, "a ceiling refusal must not take a token");
    struct in6_addr v; bool is_new;
    CHECK(alloc(p, 2, t0, &v, &is_new) == 0);
    CHECKM(fipsd_gw_ip6_eq(&v, &vips[1]) && !is_new, "a name that already has a mapping must still resolve at the ceiling");
    fipsd_gw_pool_free(p);
}
static void ceiling_is_checked_before_the_rate_limit(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = limited_pool(3, 3, 1);
    for (uint8_t i = 1; i <= 3; ++i) { alloc_ok(p, i, t0); mark_used(p, i, t0); }
    CHECK(fipsd_gw_pool_test_tokens(p) == 0);
    CHECKM(alloc(p, 4, t0, NULL, NULL) == FIPSD_GW_POOL_AT_CEILING, "a name refused at the ceiling must report the ceiling, not the rate");
    fipsd_gw_pool_free(p);
}
static void rate_limit_refuses_a_burst_keeps_existing_names_and_refills(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = limited_pool(100, 2, 1);
    struct in6_addr vip1 = alloc_ok(p, 1, t0);
    alloc_ok(p, 2, t0);
    CHECKM(alloc(p, 3, t0, NULL, NULL) == FIPSD_GW_POOL_RATE_LIMITED, "a third new name at the same instant must be refused by a burst of 2");
    struct in6_addr v; bool is_new;
    CHECK(alloc(p, 1, t0, &v, &is_new) == 0);
    CHECKM(fipsd_gw_ip6_eq(&v, &vip1) && !is_new, "an existing name must resolve with the bucket empty");
    CHECK(fipsd_gw_pool_test_tokens(p) == 0);
    CHECKM(alloc(p, 3, t0, NULL, NULL) == FIPSD_GW_POOL_RATE_LIMITED, "resolving an existing name must not have freed a token");
    CHECK(alloc(p, 3, t0 + SECS(1), NULL, &is_new) == 0);
    CHECKM(is_new, "one refill interval later a new name must allocate");
    fipsd_gw_pool_free(p);
}
static void exhausted_pool_takes_no_token(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = limited("fd01::/126", 60, 60, 100, 10, 1);
    for (uint8_t i = 1; i <= 3; ++i) alloc_ok(p, i, t0);
    CHECK(alloc(p, 4, t0, NULL, NULL) == FIPSD_GW_POOL_EXHAUSTED);
    CHECKM(fipsd_gw_pool_test_tokens(p) == 7, "a refusal for an exhausted pool must not take a token");
    fipsd_gw_pool_free(p);
}
static void a_restored_pool_does_not_reissue_the_previous_runs_addresses(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool_begin b = fresh(1);
    fipsd_gw_pool *first = started("fd01::/112", 60, 60, &b, t0);
    fipsd_gw_pool_state state;
    CHECKM(state_mark(first, &state), "a fresh pool has no durable mark, so it asks for one");
    fipsd_gw_pool_confirm_mark(first, &state);
    struct in6_addr issued[3];
    for (uint8_t i = 1; i <= 3; ++i) issued[i - 1] = alloc_ok(first, i, t0);
    fipsd_gw_instant t1 = t0 + SECS(1);
    fipsd_gw_pool_begin r = restored(&state);
    fipsd_gw_pool *second = started("fd01::/112", 60, 60, &r, t1);
    fipsd_gw_pool_state mark;
    CHECKM(state_mark(second, &mark), "a restored pool has no durable mark yet");
    fipsd_gw_pool_confirm_mark(second, &mark);
    struct in6_addr next;
    CHECKM(alloc(second, 4, t1, &next, NULL) == 0, "the restored pool refused a new name");
    for (int i = 0; i < 3; ++i) CHECKM(!fipsd_gw_ip6_eq(&next, &issued[i]), "the restored pool reissued an address of the previous run");
    fipsd_gw_pool_state_clear(&state); fipsd_gw_pool_state_clear(&mark);
    fipsd_gw_pool_free(first); fipsd_gw_pool_free(second);
}
static void a_restored_pool_holds_a_live_mapping_from_before_the_last_lap(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool_begin b = fresh(1);
    sim a; sim_started(&a, "fd01::/120", 5, 5, &b, t0);
    struct in6_addr n1 = sim_alloc_waiting(&a, 1).virtual_ip;
    gw_sessions_set(&a.pinned, n1, 1);
    uint32_t n1_offset = sim_offset(&a, n1);
    uint32_t i = 2;
    for (;;) {
        fipsd_gw_allocation al = sim_alloc_waiting(&a, i);
        sim_advance(&a, SECS(1));
        i++;
        if (sim_offset(&a, al.virtual_ip) == fipsd_gw_pool_test_total(a.pool) &&
            fipsd_gw_pool_test_offset_at(a.pool, fipsd_gw_pool_test_cursor(a.pool)) == n1_offset) break;
        CHECKM(i < 2000, "control: the cursor did not lap");
    }
    CHECKM(i > 255, "control: the cursor lapped the pool");
    const fipsd_gw_mapping *m = fipsd_gw_pool_lookup_virtual_ip(a.pool, &n1);
    fips_node_addr n1_node = gw_node_n(1);
    CHECKM(m && !memcmp(&m->node_addr, &n1_node, sizeof n1_node), "control: n1 is still mapped");
    fipsd_gw_pool_state state; CHECK(fipsd_gw_pool_shutdown_state(a.pool, &state) == 0);
    fipsd_gw_pool_begin r = restored(&state);
    sim bs; sim_started(&bs, "fd01::/120", 5, 5, &r, a.now + SECS(1));
    for (uint32_t k = 0; k < 20; ++k) {
        fipsd_gw_allocation al;
        CHECKM(sim_alloc(&bs, 10000 + k, &al) == 0, "the restored pool refused");
        CHECKM(!fipsd_gw_ip6_eq(&al.virtual_ip, &n1), "a live address from before the last lap was reissued after a restart");
    }
    sim_advance(&bs, SECS(state.hold_secs + 10 * FIPSD_GW_MAX_ABSENT_READS));
    CHECKM(fipsd_gw_pool_test_free_contains(bs.pool, n1_offset), "n1's offset is released");
    fipsd_gw_pool_state_clear(&state); sim_free(&a); sim_free(&bs);
}
static void a_restored_slash_112_pool_holds_a_live_mapping_just_past_the_reserve(void)
{
    uint64_t ttl = 5, grace = 25;
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool_begin b = fresh(1);
    sim a; sim_started(&a, "fd01::/112", ttl, grace, &b, t0);
    struct in6_addr n1 = sim_alloc_waiting(&a, 1).virtual_ip;
    gw_sessions_set(&a.pinned, n1, 1);
    uint32_t n1_offset = sim_offset(&a, n1);
    uint64_t total = fipsd_gw_pool_test_total(a.pool);
    uint32_t i = 2;
    fipsd_gw_pool_state state; memset(&state, 0, sizeof state);
    for (;;) {
        (void)sim_alloc_waiting(&a, i);
        i++;
        CHECKM((uint64_t)i < 2 * total, "control: no state ended just short of n1");
        if (fipsd_gw_pool_test_cursor(a.pool) < total) continue;
        const fipsd_gw_pool_state *w = &a.written;
        uint64_t end = ((uint64_t)w->from - 1 + w->span) % total + 1;
        uint64_t gap = ((uint64_t)n1_offset + total - end) % total;
        if (w->span == FIPSD_GW_MARK_RESERVE && gap < 300 && end != (uint64_t)n1_offset + 1) { CHECK(fipsd_gw_pool_state_copy(&state, w) == 0); break; }
    }
    const fipsd_gw_mapping *m = fipsd_gw_pool_lookup_virtual_ip(a.pool, &n1);
    CHECKM(m, "control: n1 is still mapped");
    fipsd_gw_instant t1 = a.now + SECS(1);
    fipsd_gw_pool_begin r = restored(&state);
    sim bs; sim_started(&bs, "fd01::/112", ttl, grace, &r, t1);
    fipsd_gw_instant hold_ends = t1 + SECS(state.hold_secs);
    uint64_t n1_position = fipsd_gw_pool_test_ring_distance(bs.pool, fipsd_gw_pool_test_start(bs.pool), n1_offset);
    CHECKM(n1_position < 300, "control: n1 lies just past the stretch");
    uint32_t k = 0;
    while (fipsd_gw_pool_test_cursor(bs.pool) <= n1_position) {
        fipsd_gw_allocation al = sim_alloc_waiting(&bs, 100000 + k);
        k++;
        CHECKM(bs.now < hold_ends, "control: the restored pool passed n1 within the hold");
        CHECKM(!fipsd_gw_ip6_eq(&al.virtual_ip, &n1), "a live address just past the written stretch was reissued after a crash");
    }
    CHECKM(k > 0, "control: the restored pool issued names");
    fipsd_gw_pool_state_clear(&state); sim_free(&a); sim_free(&bs);
}
static void a_full_lap_stretch_survives_a_restart(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool_begin b = fresh(1);
    sim a; sim_started(&a, "fd01::/120", 600, 600, &b, t0);
    for (uint32_t i = 1; i <= 255; ++i) {
        struct in6_addr v = sim_alloc_waiting(&a, i).virtual_ip;
        if (i < 255) gw_sessions_set(&a.pinned, v, 1);
    }
    struct in6_addr last; fipsd_gw_pool_test_offset_addr(a.pool, 255, &last);
    sim_advance(&a, SECS(1300));
    CHECKM(!fipsd_gw_pool_lookup_virtual_ip(a.pool, &last), "control: the last name expired");
    CHECKM(fipsd_gw_pool_test_offset_at(a.pool, fipsd_gw_pool_test_cursor(a.pool)) == 1, "control: the cursor is just past it");
    fipsd_gw_pool_state state;
    CHECKM(state_mark(a.pool, &state), "no durable mark");
    fipsd_gw_pool_confirm_mark(a.pool, &state);
    CHECKM(state.span == 255, "control: a whole-lap stretch");
    struct in6_addr issued = sim_alloc_waiting(&a, 1000).virtual_ip;
    CHECKM(fipsd_gw_ip6_eq(&issued, &last), "control: the name after the write takes the free offset");
    fipsd_gw_pool_begin r = restored(&state);
    sim bs; sim_started(&bs, "fd01::/120", 600, 600, &r, a.now + SECS(1));
    fipsd_gw_allocation next;
    fipsd_gw_pool_error e = sim_alloc(&bs, 2000, &next);
    CHECKM(e != 0 || !fipsd_gw_ip6_eq(&next.virtual_ip, &issued), "an address issued in a whole-lap stretch was reissued after a restart");
    fipsd_gw_pool_state_clear(&state); sim_free(&a); sim_free(&bs);
}
static void the_mark_is_never_passed_until_the_next_one_is_confirmed(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool_begin b = fresh(1);
    fipsd_gw_pool *p = started("fd01::/112", 600, 600, &b, t0);
    fips_node_addr n = gw_node_n(1); struct in6_addr m = gw_mesh_n(1); fipsd_gw_allocation al;
    CHECKM(alloc_at(p, &n, &m, t0, &al) == FIPSD_GW_POOL_AWAITING_MARK, "a started pool waits for its first write");
    CHECK(fipsd_gw_pool_test_refused_total(p) == 1);
    CHECKM(fipsd_gw_pool_test_refusal_open(p, NULL), "the first refusal warns and latches");
    fipsd_gw_pool_state state; CHECK(state_mark(p, &state)); fipsd_gw_pool_confirm_mark(p, &state);
    fipsd_gw_instant now = t0;
    uint32_t issued = 0, i = 1;
    for (;;) {
        n = gw_node_n(i); m = gw_mesh_n(i);
        fipsd_gw_pool_error e = alloc_at(p, &n, &m, now, &al);
        if (!e) { issued++; i++; }
        else if (e == FIPSD_GW_POOL_RATE_LIMITED) now += MILLIS(100);
        else if (e == FIPSD_GW_POOL_AWAITING_MARK) break;
        else CHECKM(false, "unexpected refusal %s", fipsd_gw_pool_error_reason(e));
    }
    CHECKM(issued == FIPSD_GW_MARK_RESERVE, "the pool issues exactly up to the mark (%u)", issued);
    n = gw_node_n(i); m = gw_mesh_n(i);
    CHECK(alloc_at(p, &n, &m, now + SECS(1), &al) == FIPSD_GW_POOL_AWAITING_MARK);
    uint64_t count = 0;
    CHECK(fipsd_gw_pool_test_refusal_open(p, &count));
    CHECKM(count == 3, "later refusals are counted in the open episode, not warned again (%llu)", (unsigned long long)count);
    fipsd_gw_pool_state next; CHECKM(state_mark(p, &next), "the mark is reached");
    fipsd_gw_pool_confirm_mark(p, &next);
    CHECKM(alloc_at(p, &n, &m, now + SECS(2), &al) == 0, "a confirmed mark lets allocation continue");
    gw_sessions none = {0};
    CHECK(tick(p, now + SECS(30), &none, NULL) == 0);
    CHECKM(fipsd_gw_pool_test_refusal_open(p, NULL), "not yet");
    CHECK(tick(p, now + SECS(1) + SECS(FIPSD_GW_BOUND_LOG_HOLD_SECS), &none, NULL) == 0);
    CHECKM(!fipsd_gw_pool_test_refusal_open(p, NULL), "released after BOUND_LOG_HOLD");
    fipsd_gw_pool_state_clear(&state); fipsd_gw_pool_state_clear(&next); fipsd_gw_pool_free(p);
}
static void a_late_tick_at_full_rate_never_hits_the_mark(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool_begin b = fresh(1);
    fipsd_gw_pool *p = started("fd01::/112", 600, 600, &b, t0);
    fipsd_gw_pool_state state; CHECK(state_mark(p, &state)); fipsd_gw_pool_confirm_mark(p, &state);
    fipsd_gw_instant now = t0;
    uint32_t i = 1;
    fipsd_gw_allocation al;
    while (i <= FIPSD_GW_MARK_RESERVE - FIPSD_GW_MARK_LOW_WATER) {
        fips_node_addr n = gw_node_n(i); struct in6_addr m = gw_mesh_n(i);
        fipsd_gw_pool_error e = alloc_at(p, &n, &m, now, &al);
        if (!e) i++;
        else if (e == FIPSD_GW_POOL_RATE_LIMITED) now += MILLIS(100);
        else CHECKM(false, "unexpected refusal %s", fipsd_gw_pool_error_reason(e));
    }
    gw_sessions none = {0};
    CHECK(tick(p, now, &none, NULL) == 0);
    fipsd_gw_pool_state again;
    CHECKM(!state_mark(p, &again), "control: exactly MARK_LOW_WATER left is not yet a reason to write");
    fipsd_gw_instant start = now + SECS(5);
    uint32_t admitted = 0;
    for (uint32_t step = 0; step <= 250; ++step) {
        fipsd_gw_instant at = start + MILLIS(100) * step;
        for (;;) {
            fips_node_addr n = gw_node_n(i); struct in6_addr m = gw_mesh_n(i);
            fipsd_gw_pool_error e = alloc_at(p, &n, &m, at, &al);
            if (!e) { admitted++; i++; continue; }
            if (e == FIPSD_GW_POOL_RATE_LIMITED) break;
            CHECKM(false, "allocation %u of a 25 s late tick at full rate was refused: %s", admitted + 1, fipsd_gw_pool_error_reason(e));
        }
    }
    CHECKM(admitted == FIPSD_GW_MAPPING_BURST + FIPSD_GW_MAPPING_RATE * 25, "control: a full bucket and 25 s at the full rate (%u)", admitted);
    fipsd_gw_pool_state_clear(&state); fipsd_gw_pool_free(p);
}
static fipsd_gw_pool_state make_state(const char *pool, uint32_t total, uint32_t from, uint32_t span, const uint32_t (*held)[2], size_t nheld, uint64_t hold)
{
    fipsd_gw_pool_state s; memset(&s, 0, sizeof s);
    s.version = 1; snprintf(s.pool, sizeof s.pool, "%s", pool); s.total = total; s.from = from; s.span = span; s.hold_secs = hold;
    if (nheld) { s.held = calloc(nheld, sizeof *s.held); CHECK(s.held); memcpy(s.held, held, nheld * sizeof *s.held); s.held_len = nheld; }
    return s;
}
static void a_run_of_mapped_addresses_ahead_of_the_cursor_does_not_starve_the_mark(void)
{
    fipsd_gw_instant t0 = gw_t0();
    const uint32_t held[][2] = {{1, 300}};
    fipsd_gw_pool_state state = make_state("fd01::/112", 65535, 1, 0, held, 1, 600);
    fipsd_gw_pool_begin r = restored(&state);
    fipsd_gw_pool *p = started("fd01::/112", 600, 600, &r, t0);
    fipsd_gw_pool_state request; CHECK(state_mark(p, &request));
    CHECK(request.from == 1);
    CHECKM(request.span == 300 + FIPSD_GW_MARK_RESERVE, "the mark covers the run plus MARK_RESERVE free offsets (%u)", request.span);
    fipsd_gw_pool_confirm_mark(p, &request);
    fips_node_addr n = gw_node_n(1); struct in6_addr m = gw_mesh_n(1); fipsd_gw_allocation al;
    CHECKM(alloc_at(p, &n, &m, t0, &al) == 0, "allocation continues");
    CHECK(offset_of(p, &al.virtual_ip) == 301);
    fipsd_gw_pool_state_clear(&state); fipsd_gw_pool_state_clear(&request); fipsd_gw_pool_free(p);
}
static void restart_holds_release_after_the_loaded_hold_and_not_before(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool_state state = make_state("fd01::/120", 255, 1, 255, NULL, 0, 10);
    fipsd_gw_pool_begin r = restored(&state);
    sim s; sim_started(&s, "fd01::/120", 5, 5, &r, t0);
    CHECKM(sim_alloc(&s, 1, NULL) == FIPSD_GW_POOL_EXHAUSTED, "every offset is held from the previous run");
    sim_advance(&s, SECS(9));
    CHECK(sim_alloc(&s, 1, NULL) == FIPSD_GW_POOL_EXHAUSTED);
    sim_advance(&s, SECS(10 * FIPSD_GW_MAX_ABSENT_READS));
    CHECKM(sim_alloc(&s, 1, NULL) == 0, "released after the hold and the reads");
    sim_free(&s);
    /* Past the hold with one read too few, nothing is released. */
    fipsd_gw_pool *p = started("fd01::/120", 5, 5, &r, t0);
    fipsd_gw_pool_state request; CHECK(state_mark(p, &request)); fipsd_gw_pool_confirm_mark(p, &request); fipsd_gw_pool_state_clear(&request);
    gw_sessions read = {0};
    for (uint32_t k = 1; k < FIPSD_GW_MAX_ABSENT_READS; ++k) CHECK(tick(p, t0 + SECS(k), &read, NULL) == 0);
    fipsd_gw_instant past = t0 + SECS(60);
    fips_node_addr n = gw_node_n(1); struct in6_addr m = gw_mesh_n(1); fipsd_gw_allocation al;
    CHECK(alloc_at(p, &n, &m, past, &al) == FIPSD_GW_POOL_EXHAUSTED);
    CHECK(tick(p, past, &read, NULL) == 0);
    CHECK(state_mark(p, &request)); fipsd_gw_pool_confirm_mark(p, &request); fipsd_gw_pool_state_clear(&request);
    CHECK(alloc_at(p, &n, &m, past, &al) == 0);
    fipsd_gw_pool_free(p); fipsd_gw_pool_state_clear(&state);
}
static void a_restored_pool_writes_a_hold_no_shorter_than_what_remains_of_its_loaded_one(void)
{
    fipsd_gw_instant t0 = gw_t0();
    const uint32_t held[][2] = {{1, 10}};
    fipsd_gw_pool_state loaded = make_state("fd01::/120", 255, 1, 0, held, 1, 600);
    fipsd_gw_pool_begin r = restored(&loaded);
    sim a; sim_started(&a, "fd01::/120", 5, 5, &r, t0);
    sim_advance(&a, SECS(100));
    uint64_t remaining = 500;
    fipsd_gw_pool_state crash, clean;
    CHECKM(state_mark(a.pool, &crash), "a small pool asks again");
    CHECK(fipsd_gw_pool_shutdown_state(a.pool, &clean) == 0);
    CHECKM(crash.hold_secs >= remaining, "the tick state holds offsets 1-10 for %llu s", (unsigned long long)crash.hold_secs);
    CHECKM(clean.hold_secs >= remaining, "the shutdown state holds offsets 1-10 for %llu s", (unsigned long long)clean.hold_secs);
    uint64_t waited = 10 + 10 * FIPSD_GW_MAX_ABSENT_READS + 10;
    fipsd_gw_pool_begin rc = restored(&crash);
    sim c; sim_started(&c, "fd01::/120", 5, 5, &rc, a.now);
    sim_advance(&c, SECS(waited));
    for (uint32_t i = 0; i < 255; ++i) {
        fipsd_gw_allocation al;
        fipsd_gw_pool_error e = sim_alloc(&c, i, &al);
        if (!e) {
            uint32_t o = sim_offset(&c, al.virtual_ip);
            CHECKM(o < 1 || o > 10, "offset %u, held by the earlier run for %llu s more, was reissued after %llu s", o,
                   (unsigned long long)remaining, (unsigned long long)waited);
        } else if (e == FIPSD_GW_POOL_EXHAUSTED) break;
        else if (e == FIPSD_GW_POOL_RATE_LIMITED || e == FIPSD_GW_POOL_AWAITING_MARK) sim_advance(&c, MILLIS(100));
        else CHECKM(false, "name %u refused: %s", i, fipsd_gw_pool_error_reason(e));
    }
    sim_advance(&c, SECS(remaining + 10 * FIPSD_GW_MAX_ABSENT_READS));
    CHECKM(sim_alloc(&c, 1000, NULL) == 0, "the hold never ended");
    fipsd_gw_pool_state_clear(&crash); fipsd_gw_pool_state_clear(&clean); fipsd_gw_pool_state_clear(&loaded);
    sim_free(&a); sim_free(&c);
}
static void a_clean_shutdown_state_holds_only_live_addresses(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool_begin b = fresh(1);
    sim a; sim_started(&a, "fd01::/120", 60, 60, &b, t0);
    struct in6_addr live[3];
    for (uint32_t i = 1; i <= 3; ++i) live[i - 1] = sim_alloc_waiting(&a, i).virtual_ip;
    fipsd_gw_pool_state clean; CHECK(fipsd_gw_pool_shutdown_state(a.pool, &clean) == 0);
    fipsd_gw_pool_begin r = restored(&clean);
    sim bs; sim_started(&bs, "fd01::/120", 60, 60, &r, a.now);
    fipsd_gw_allocation al;
    CHECKM(sim_alloc(&bs, 10, &al) == 0, "after a clean stop a new name resolves at once");
    for (int i = 0; i < 3; ++i) CHECK(!fipsd_gw_ip6_eq(&al.virtual_ip, &live[i]));
    fipsd_gw_pool_state crash; CHECK(state_mark(a.pool, &crash));
    fipsd_gw_pool_begin rc = restored(&crash);
    sim c; sim_started(&c, "fd01::/120", 60, 60, &rc, a.now);
    CHECK(sim_alloc(&c, 10, NULL) == FIPSD_GW_POOL_EXHAUSTED);
    fipsd_gw_pool_state_clear(&clean); fipsd_gw_pool_state_clear(&crash);
    sim_free(&a); sim_free(&bs); sim_free(&c);
}
static void a_new_name_at_the_ceiling_replaces_the_oldest_never_used_mapping(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = limited_pool(4, 5, 1);
    struct in6_addr vips[4];
    for (uint8_t i = 1; i <= 4; ++i) vips[i - 1] = alloc_ok(p, i, t0 + SECS(i - 1));
    for (int i = 0; i < 4; ++i) for (int j = i + 1; j < 4; ++j) CHECKM(!fipsd_gw_ip6_eq(&vips[i], &vips[j]), "control: four distinct addresses");
    fips_node_addr n = gw_node(5); struct in6_addr m = gw_mesh(5); fipsd_gw_allocation al;
    CHECKM(alloc_at(p, &n, &m, t0 + SECS(4), &al) == 0, "a LAN host holding the ceiling with unused names must not lock every other client out");
    struct in6_addr m1 = gw_mesh(1);
    CHECK(al.evicted.present && fipsd_gw_ip6_eq(&al.evicted.virtual_ip, &vips[0]) && fipsd_gw_ip6_eq(&al.evicted.mesh_addr, &m1));
    CHECK(!fipsd_gw_pool_lookup_virtual_ip(p, &vips[0]));
    for (int i = 1; i < 4; ++i) CHECK(fipsd_gw_pool_lookup_virtual_ip(p, &vips[i]));
    fipsd_gw_pool_free(p);
}
static void a_lan_host_requerying_a_thousand_unused_names_does_not_hold_the_ceiling(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = pool_new("fd01::/112", 60, 60);
    gw_sessions empty = {0};
    fipsd_gw_instant *last = calloc(FIPSD_GW_MAPPING_CEILING, sizeof *last); CHECK(last);
    size_t nlast = 0;
    fipsd_gw_instant next_tick = t0, now = t0;
    fipsd_gw_allocation al;
    while (nlast < FIPSD_GW_MAPPING_CEILING) {
        if (now >= next_tick) { CHECK(tick(p, now, &empty, NULL) == 0); next_tick += SECS(10); }
        for (size_t k = 0; k < nlast; ++k) {
            if (now - last[k] >= SECS(30)) {
                fips_node_addr n = gw_node_n((uint32_t)k + 1); struct in6_addr m = gw_mesh_n((uint32_t)k + 1);
                CHECKM(alloc_at(p, &n, &m, now, &al) == 0, "an existing name resolves");
                last[k] = now;
            }
        }
        uint32_t i = (uint32_t)nlast + 1;
        fips_node_addr n = gw_node_n(i); struct in6_addr m = gw_mesh_n(i);
        fipsd_gw_pool_error e = alloc_at(p, &n, &m, now, &al);
        if (!e) last[nlast++] = now;
        else CHECKM(e == FIPSD_GW_POOL_RATE_LIMITED, "name %u refused below the ceiling: %s", i, fipsd_gw_pool_error_reason(e));
        now += MILLIS(100);
    }
    CHECKM(now < t0 + SECS(120), "control: the fill finished before the first name's TTL and grace");
    CHECK(fipsd_gw_pool_len(p) == FIPSD_GW_MAPPING_CEILING);
    now += SECS(1);
    fips_node_addr n1 = gw_node_n(1);
    const fipsd_gw_mapping *first = fipsd_gw_pool_lookup_node(p, &n1); CHECKM(first, "name 1 is live");
    struct in6_addr first_vip = first->virtual_ip;
    uint32_t next = FIPSD_GW_MAPPING_CEILING + 1;
    fips_node_addr n = gw_node_n(next); struct in6_addr m = gw_mesh_n(next);
    fipsd_gw_pool_error e = alloc_at(p, &n, &m, now, &al);
    CHECKM(e != FIPSD_GW_POOL_AT_CEILING, "one LAN host re-querying names nobody uses holds the ceiling");
    CHECKM(e == 0, "the new name is allocated");
    struct in6_addr mesh1 = gw_mesh_n(1);
    CHECK(al.evicted.present && fipsd_gw_ip6_eq(&al.evicted.virtual_ip, &first_vip) && fipsd_gw_ip6_eq(&al.evicted.mesh_addr, &mesh1));
    free(last); fipsd_gw_pool_free(p);
}
/* A mesh address inside the fd01::/16 pool prefix, for index i. */
static struct in6_addr mesh_in_pool(uint32_t i)
{
    struct in6_addr a; memset(&a, 0, sizeof a);
    uint16_t w[8] = {0xfd01, (uint16_t)(i >> 16), (uint16_t)i, 0x5678, 0, 0, 0, 9};
    for (int k = 0; k < 8; ++k) { a.s6_addr[2 * k] = (uint8_t)(w[k] >> 8); a.s6_addr[2 * k + 1] = (uint8_t)w[k]; }
    return a;
}
static void mesh_addresses_inside_a_wide_pool_prefix_cannot_hold_the_ceiling(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = limited("fd01::/16", 60, 60, FIPSD_GW_MAPPING_CEILING, 10000, 10000);
    fipsd_gw_ct_snapshot replies; fipsd_gw_ct_snapshot_init_read(&replies);
    fipsd_gw_allocation al;
    for (uint32_t i = 1; i <= FIPSD_GW_MAPPING_CEILING; ++i) {
        fips_node_addr n = gw_node_n(i); struct in6_addr m = mesh_in_pool(i);
        if (fipsd_gw_pool_allocate_at(p, &n, &m, "x.fips", t0, &al) == 0) CHECK(fipsd_gw_ct_snapshot_record_binding(&replies, &al.virtual_ip, &m, true) == 0);
    }
    fipsd_gw_events ev = {0};
    CHECK(fipsd_gw_pool_tick(p, t0 + SECS(10), &replies, &ev) == 0);
    fipsd_gw_events_clear(&ev); fipsd_gw_ct_snapshot_clear(&replies);
    uint32_t next = FIPSD_GW_MAPPING_CEILING + 1;
    fips_node_addr n = gw_node_n(next); struct in6_addr m = gw_mesh_n(next);
    CHECKM(fipsd_gw_pool_allocate_at(p, &n, &m, "y.fips", t0 + SECS(11), &al) == 0,
           "names whose mesh address lies inside the pool prefix hold the ceiling against a name outside it");
    n = gw_node_n(1); m = mesh_in_pool(1);
    CHECKM(fipsd_gw_pool_allocate_at(p, &n, &m, "x.fips", t0, &al) == FIPSD_GW_POOL_INSIDE_POOL, "a mesh address inside the pool prefix is not refused as such");
    fipsd_gw_pool_free(p);
}
static void a_mesh_address_outside_the_pool_prefix_still_maps_when_it_shares_a_wider_prefix(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/112", 60, 60);
    fips_node_addr n = gw_node_n(1); struct in6_addr m = gw_v6("fd01::1:9"); fipsd_gw_allocation al;
    CHECKM(fipsd_gw_pool_allocate_at(p, &n, &m, "x.fips", gw_t0(), &al) == 0 && al.is_new, "fd01::1:9 lies outside fd01::/112 and maps");
    fipsd_gw_pool_free(p);
}
static void requerying_a_never_used_name_does_not_keep_it_past_ttl_and_grace(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = limited_pool(100, 10, 1);
    gw_sessions empty = {0};
    struct in6_addr vip = alloc_ok(p, 1, t0), again; bool is_new;
    CHECK(alloc(p, 1, t0 + SECS(59), &again, &is_new) == 0);
    CHECKM(fipsd_gw_ip6_eq(&again, &vip) && !is_new, "control: the re-query returns the same address");
    CHECK(tick_events(p, t0 + SECS(70), &empty) == 0);
    fipsd_gw_events ev = {0};
    CHECK(tick(p, t0 + SECS(131), &empty, &ev) == 0);
    CHECKM(!fipsd_gw_pool_lookup_virtual_ip(p, &vip), "a re-query kept a name nobody used mapped past its TTL and grace");
    CHECK(ev.len == 1 && ev.items[0].kind == FIPSD_GW_EVENT_MAPPING_REMOVED);
    fipsd_gw_events_clear(&ev); fipsd_gw_pool_free(p);
}
static void an_evicted_address_is_not_reissued_before_its_answer_deadline(void)
{
    fipsd_gw_instant t = gw_t0();
    fipsd_gw_pool *p = fast_pool(4, 60);
    fipsd_gw_allocation al;
    for (uint32_t i = 1; i <= 4; ++i) { fips_node_addr n = gw_node_n(i); struct in6_addr m = gw_mesh_n(i); CHECK(alloc_at(p, &n, &m, t, &al) == 0); }
    fipsd_gw_instant t1 = t + SECS(1);
    struct in6_addr first = gw_at(1);
    for (uint32_t i = 5; i <= 255; ++i) {
        fips_node_addr n = gw_node_n(i); struct in6_addr m = gw_mesh_n(i);
        CHECKM(alloc_at(p, &n, &m, t1, &al) == 0, "name %u", i);
        CHECKM(!fipsd_gw_ip6_eq(&al.virtual_ip, &first), "name %u took the evicted address", i);
        CHECKM(al.evicted.present, "at the ceiling a name is replaced");
        fipsd_gw_pool_nat_removed(p, &al.evicted.virtual_ip, &al.evicted.mesh_addr);
    }
    gw_sessions empty = {0};
    for (uint32_t k = 1; k <= FIPSD_GW_MAX_ABSENT_READS; ++k) CHECK(tick(p, t1 + SECS(k), &empty, NULL) == 0);
    CHECKM(fipsd_gw_pool_test_offset_at(p, fipsd_gw_pool_test_cursor(p)) == 1, "control: the cursor lapped");
    fips_node_addr n = gw_node_n(256); struct in6_addr m = gw_mesh_n(256);
    CHECKM(alloc_at(p, &n, &m, t1 + SECS(5), &al) == FIPSD_GW_POOL_EXHAUSTED, "an evicted address was issued before its answer deadline");
    fipsd_gw_instant after = t + SECS(121);
    CHECK(tick(p, after, &empty, NULL) == 0);
    CHECKM(alloc_at(p, &n, &m, after, &al) == 0, "the address returns after its deadline");
    CHECK(fipsd_gw_ip6_eq(&al.virtual_ip, &first));
    fipsd_gw_pool_free(p);
}
static void a_reply_from_the_virtual_ip_itself_does_not_mark_a_mapping_used(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(10, 60);
    gw_sessions ct = {0}; gw_sessions_bind(&ct, gw_at(1), gw_at(1), true);
    CHECK(tick(p, t0, &ct, NULL) == 0);
    struct in6_addr vip = alloc_ok(p, 1, t0), want = gw_at(1);
    CHECKM(fipsd_gw_ip6_eq(&vip, &want), "control: the name got the address the entry names");
    CHECK(tick(p, t0 + SECS(10), &ct, NULL) == 0);
    CHECK(!mapping(p, 1)->used);
    fipsd_gw_pool_free(p);
}
static void a_reply_from_a_previous_holders_mesh_address_does_not_mark_the_new_mapping_used(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(10, 60);
    gw_sessions ct = {0}; gw_sessions_bind(&ct, gw_at(1), gw_mesh(1), true);
    CHECK(tick(p, t0, &ct, NULL) == 0);
    struct in6_addr vip = alloc_ok(p, 2, t0), want = gw_at(1);
    CHECKM(fipsd_gw_ip6_eq(&vip, &want), "control: the new holder got the address");
    CHECK(tick(p, t0 + SECS(10), &ct, NULL) == 0);
    CHECK(!mapping(p, 2)->used);
    fipsd_gw_pool_free(p);
}
/* The pool after node 1's mapping to the first address was replaced, its
 * removal reported and the cursor sent round, then read ticks that each hold
 * a forged reply on the old binding when reads[k] and miss it otherwise. */
static fipsd_gw_pool *pool_with_a_surviving_binding(const bool *reads, size_t nreads, fipsd_gw_instant *now_out)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(1, 5);
    fips_node_addr n1 = gw_node(1); struct in6_addr m1 = gw_mesh(1); fipsd_gw_allocation al;
    CHECK(alloc_at(p, &n1, &m1, t0, &al) == 0);
    struct in6_addr first = gw_at(1); CHECK(fipsd_gw_ip6_eq(&al.virtual_ip, &first));
    for (uint32_t i = 2; i <= 255; ++i) {
        fips_node_addr n = gw_node_n(i); struct in6_addr m = gw_mesh_n(i);
        CHECK(alloc_at(p, &n, &m, t0, &al) == 0);
        CHECKM(al.evicted.present, "ceiling 1 replaces every name");
        fipsd_gw_pool_nat_removed(p, &al.evicted.virtual_ip, &al.evicted.mesh_addr);
    }
    CHECKM(fipsd_gw_pool_test_offset_at(p, fipsd_gw_pool_test_cursor(p)) == 1, "control: the cursor lapped");
    gw_sessions forged = {0}, partial = {0};
    gw_sessions_bind(&forged, gw_at(1), gw_mesh(1), true);
    fipsd_gw_instant now = t0;
    for (size_t k = 0; k < nreads; ++k) { now += SECS(10); CHECK(tick(p, now, reads[k] ? &forged : &partial, NULL) == 0); }
    *now_out = now;
    return p;
}
static void an_address_is_not_given_back_to_a_node_whose_old_binding_to_it_survives(void)
{
    const bool reads[] = {true, true, true};
    fipsd_gw_instant now, unused;
    fipsd_gw_pool *attacked = pool_with_a_surviving_binding(reads, 3, &now), *control = pool_with_a_surviving_binding(reads, 3, &unused);
    fips_node_addr n2 = gw_node(2), n1 = gw_node(1); struct in6_addr m2 = gw_mesh(2), m1 = gw_mesh(1), first = gw_at(1);
    fipsd_gw_allocation al;
    CHECK(alloc_at(control, &n2, &m2, now, &al) == 0);
    CHECKM(fipsd_gw_ip6_eq(&al.virtual_ip, &first), "control: another node gets the address");
    CHECK(alloc_at(attacked, &n1, &m1, now, &al) == 0);
    CHECKM(!fipsd_gw_ip6_eq(&al.virtual_ip, &first), "the node was given back an address its forged old binding still names");
    gw_sessions forged = {0}; gw_sessions_bind(&forged, first, m1, true);
    CHECK(tick(attacked, now + SECS(10), &forged, NULL) == 0);
    CHECK(!mapping(attacked, 1)->used);
    fipsd_gw_pool_free(attacked); fipsd_gw_pool_free(control);
}
static void a_binding_missed_by_one_partial_read_still_blocks_the_reissue(void)
{
    const bool reads[] = {true, true, true, false};
    fipsd_gw_instant now;
    fipsd_gw_pool *p = pool_with_a_surviving_binding(reads, 4, &now);
    fips_node_addr n1 = gw_node(1); struct in6_addr m1 = gw_mesh(1), first = gw_at(1);
    fipsd_gw_allocation al;
    CHECK(alloc_at(p, &n1, &m1, now, &al) == 0);
    CHECKM(!fipsd_gw_ip6_eq(&al.virtual_ip, &first), "one partial read dropped the binding");
    gw_sessions forged = {0}; gw_sessions_bind(&forged, first, m1, true);
    CHECK(tick(p, now + SECS(10), &forged, NULL) == 0);
    CHECK(!mapping(p, 1)->used);
    fipsd_gw_pool_free(p);
}
static fipsd_gw_instant follow_unread(fipsd_gw_pool *p, fipsd_gw_instant now)
{
    for (uint32_t k = 1; k < FIPSD_GW_MAX_UNREAD_TICKS; ++k) { now += SECS(10); tick_unread(p, now); }
    now += SECS(10);
    gw_sessions none = {0};
    CHECK(tick(p, now, &none, NULL) == 0);
    return now;
}
static void unread_ticks_do_not_age_a_surviving_binding(void)
{
    const bool reads[] = {true, true, true};
    fipsd_gw_instant now, then;
    fipsd_gw_pool *p = pool_with_a_surviving_binding(reads, 3, &now);
    now = follow_unread(p, now);
    fipsd_gw_pool *control = pool_with_a_surviving_binding(reads, 3, &then);
    then = follow_unread(control, then);
    CHECKM(fipsd_gw_pool_test_evidence_trusted(control), "control: evidence is still on");
    fips_node_addr n2 = gw_node(2), n1 = gw_node(1); struct in6_addr m2 = gw_mesh(2), m1 = gw_mesh(1), first = gw_at(1);
    fipsd_gw_allocation al;
    CHECK(alloc_at(control, &n2, &m2, then, &al) == 0);
    CHECKM(fipsd_gw_ip6_eq(&al.virtual_ip, &first), "control: the cursor is at the address and it is free");
    CHECK(alloc_at(p, &n1, &m1, now, &al) == 0);
    CHECKM(!fipsd_gw_ip6_eq(&al.virtual_ip, &first), "unread ticks and one partial read dropped the binding");
    fipsd_gw_pool_free(p); fipsd_gw_pool_free(control);
}
static void a_removed_address_waits_for_its_nat_removal_and_later_reads(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(1, 60);
    struct in6_addr x = alloc_ok(p, 1, t0);
    fips_node_addr n2 = gw_node(2); struct in6_addr m2 = gw_mesh(2), m1 = gw_mesh(1); fipsd_gw_allocation al;
    CHECK(alloc_at(p, &n2, &m2, t0, &al) == 0);
    CHECKM(al.evicted.present, "control: the first name was replaced");
    uint32_t off = offset_of(p, &x);
    gw_sessions read = {0};
    fipsd_gw_instant past = t0 + SECS(121);
    CHECK(tick(p, past, &read, NULL) == 0);
    CHECKM(!fipsd_gw_pool_test_free_contains(p, off), "free before its NAT removal");
    fipsd_gw_pool_nat_removed(p, &x, &m1);
    tick_unread(p, past + SECS(10));
    CHECKM(!fipsd_gw_pool_test_free_contains(p, off), "free after an unread tick");
    for (uint32_t k = 1; k < FIPSD_GW_MAX_ABSENT_READS; ++k) {
        CHECK(tick(p, past + SECS(10 + 10 * k), &read, NULL) == 0);
        CHECKM(!fipsd_gw_pool_test_free_contains(p, off), "free after %u read(s)", k);
    }
    CHECK(tick(p, past + SECS(10 + 10 * FIPSD_GW_MAX_ABSENT_READS), &read, NULL) == 0);
    CHECKM(fipsd_gw_pool_test_free_contains(p, off), "free after the last read");
    fipsd_gw_pool_free(p);
}
static void a_pinned_never_used_address_is_held_for_grace_after_its_last_answer(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(1, 60);
    struct in6_addr x = alloc_ok(p, 1, t0);
    gw_sessions pinned = {0}; gw_sessions_set(&pinned, x, 1); gw_sessions_bind(&pinned, x, gw_mesh(1), false);
    fipsd_gw_instant now = t0;
    while (now < t0 + SECS(200)) { now += SECS(10); CHECK(tick(p, now, &pinned, NULL) == 0); }
    fips_node_addr n1 = gw_node(1), n2 = gw_node(2); struct in6_addr m1 = gw_mesh(1), m2 = gw_mesh(2); fipsd_gw_allocation al;
    CHECK(alloc_at(p, &n1, &m1, now, &al) == 0);
    CHECKM(fipsd_gw_ip6_eq(&al.virtual_ip, &x) && al.ttl == 0, "control: TTL 0 past its deadline");
    fipsd_gw_instant t = now;
    CHECK(alloc_at(p, &n2, &m2, t + SECS(1), &al) == 0);
    CHECKM(al.evicted.present, "control: the pinned name was replaced");
    fipsd_gw_pool_nat_removed(p, &al.evicted.virtual_ip, &al.evicted.mesh_addr);
    uint32_t off = offset_of(p, &x);
    gw_sessions read = {0};
    for (uint64_t k = 1; k <= 5; ++k) CHECK(tick(p, t + SECS(1 + k), &read, NULL) == 0);
    CHECKM(!fipsd_gw_pool_test_free_contains(p, off), "released before grace after its last answer, which a client may still cache");
    CHECK(tick(p, t + SECS(61), &read, NULL) == 0);
    CHECKM(fipsd_gw_pool_test_free_contains(p, off), "released after grace");
    fipsd_gw_pool_free(p);
}
static void evidence_recovers_after_a_failed_start_probe_once_reads_succeed(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(10, 600);
    fipsd_gw_pool_set_evidence(p, false);
    struct in6_addr before = alloc_ok(p, 1, t0);
    CHECKM(mapping(p, 1)->blind, "a mapping made with no readable source is blind");
    fipsd_gw_instant now = t0;
    gw_sessions none = {0};
    for (uint32_t k = 0; k < FIPSD_GW_MAX_ABSENT_READS; ++k) { now += SECS(10); CHECK(tick(p, now, &none, NULL) == 0); }
    CHECKM(fipsd_gw_pool_test_evidence_trusted(p), "evidence is not trusted after MAX_ABSENT_READS good reads that followed a failed start probe");
    struct in6_addr after = alloc_ok(p, 2, now);
    gw_sessions replies = {0}; gw_sessions_bind(&replies, before, gw_mesh(1), true); gw_sessions_bind(&replies, after, gw_mesh(2), true);
    now += SECS(10);
    CHECK(tick(p, now, &replies, NULL) == 0);
    CHECKM(mapping(p, 2)->used, "a replied mapping made after the source became readable is not used");
    CHECKM(!mapping(p, 1)->used, "a mapping made while no source was readable became used");
    fipsd_gw_pool_free(p);
}
static void a_mapping_made_before_a_started_pool_has_enough_reads_cannot_become_used(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool_begin b = fresh(1);
    fipsd_gw_pool *p = started("fd01::/112", 600, 600, &b, t0);
    fipsd_gw_pool_state state; CHECKM(state_mark(p, &state), "a fresh pool asks for a mark"); fipsd_gw_pool_confirm_mark(p, &state); fipsd_gw_pool_state_clear(&state);
    gw_sessions none = {0};
    CHECK(tick(p, t0, &none, NULL) == 0);
    struct in6_addr reissued = alloc_ok(p, 1, t0);
    gw_sessions old = {0}; gw_sessions_bind(&old, reissued, gw_mesh(1), true);
    fipsd_gw_instant now = t0;
    for (uint32_t k = 1; k < FIPSD_GW_MAX_ABSENT_READS; ++k) { now += SECS(10); CHECK(tick(p, now, &old, NULL) == 0); }
    CHECKM(!mapping(p, 1)->used, "a mapping made after one start read was marked used by a binding that read missed");
    struct in6_addr fresh_vip = alloc_ok(p, 2, now);
    gw_sessions_bind(&old, fresh_vip, gw_mesh(2), true);
    now += SECS(10);
    CHECK(tick(p, now, &old, NULL) == 0);
    CHECKM(mapping(p, 2)->used, "a replied mapping made after MAX_ABSENT_READS reads is not used");
    fipsd_gw_pool_free(p);
}
static void without_evidence_removed_addresses_wait_only_for_their_deadline_and_nat_removal(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(1, 60);
    fipsd_gw_pool_set_evidence(p, false);
    struct in6_addr x = alloc_ok(p, 1, t0), m1 = gw_mesh(1);
    alloc_ok(p, 2, t0);
    uint32_t off = offset_of(p, &x);
    fipsd_gw_instant past = t0 + SECS(121);
    tick_unread(p, past);
    CHECKM(!fipsd_gw_pool_test_free_contains(p, off), "free before its NAT removal");
    fipsd_gw_pool_nat_removed(p, &x, &m1);
    tick_unread(p, past + SECS(10));
    CHECKM(fipsd_gw_pool_test_free_contains(p, off), "free at the next tick, with no read");
    fipsd_gw_pool_free(p);
}
static void evidence_turns_off_after_unread_ticks_and_back_on_after_enough_reads(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(10, 600);
    struct in6_addr before = alloc_ok(p, 1, t0);
    fipsd_gw_instant now = t0;
    for (uint32_t k = 0; k < FIPSD_GW_MAX_UNREAD_TICKS; ++k) { now += SECS(10); tick_unread(p, now); }
    CHECKM(!fipsd_gw_pool_test_evidence_trusted(p), "off after MAX_UNREAD_TICKS unread ticks");
    struct in6_addr during = alloc_ok(p, 2, now);
    CHECK(mapping(p, 2)->blind);
    now += SECS(10);
    gw_sessions none = {0};
    CHECK(tick(p, now, &none, NULL) == 0);
    CHECKM(!fipsd_gw_pool_test_evidence_trusted(p), "still off after one read");
    struct in6_addr recovering = alloc_ok(p, 3, now);
    gw_sessions replies = {0};
    gw_sessions_bind(&replies, before, gw_mesh(1), true); gw_sessions_bind(&replies, during, gw_mesh(2), true);
    gw_sessions_bind(&replies, recovering, gw_mesh(3), true);
    for (uint32_t k = 1; k < FIPSD_GW_MAX_ABSENT_READS; ++k) { now += SECS(10); CHECK(tick(p, now, &replies, NULL) == 0); }
    CHECKM(fipsd_gw_pool_test_evidence_trusted(p), "back on after MAX_ABSENT_READS reads");
    CHECKM(mapping(p, 1)->used, "a mapping from before the outage is marked by a reply");
    CHECKM(!mapping(p, 2)->used, "a mapping created while evidence was off became used");
    CHECKM(!mapping(p, 3)->used, "a mapping created while evidence was recovering became used");
    fipsd_gw_pool_free(p);
}
static void a_used_mapping_stays_protected_while_evidence_is_off(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(4, 600);
    struct in6_addr vip = alloc_ok(p, 1, t0);
    mark_used(p, 1, t0);
    fipsd_gw_instant now = t0;
    for (uint32_t k = 0; k < FIPSD_GW_MAX_UNREAD_TICKS; ++k) { now += SECS(10); tick_unread(p, now); }
    CHECKM(!fipsd_gw_pool_test_evidence_trusted(p), "control: evidence is off");
    fipsd_gw_allocation al;
    for (uint32_t i = 100; i < 100 + 2 * 4; ++i) {
        fips_node_addr n = gw_node_n(i); struct in6_addr m = gw_mesh_n(i);
        CHECK(alloc_at(p, &n, &m, now, &al) == 0);
        CHECK(!(al.evicted.present && fipsd_gw_ip6_eq(&al.evicted.virtual_ip, &vip)));
    }
    fipsd_gw_instant before = mapping(p, 1)->last_referenced;
    struct in6_addr again; bool is_new;
    CHECK(alloc(p, 1, now + SECS(1), &again, &is_new) == 0);
    CHECK(fipsd_gw_ip6_eq(&again, &vip) && !is_new);
    CHECK(mapping(p, 1)->last_referenced > before);
    fipsd_gw_pool_free(p);
}
static void an_eviction_spends_a_token(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = limited_pool(2, 2, 1);
    alloc_ok(p, 1, t0); alloc_ok(p, 2, t0);
    CHECK(alloc(p, 3, t0, NULL, NULL) == FIPSD_GW_POOL_RATE_LIMITED);
    CHECKM(fipsd_gw_pool_len(p) == 2, "nothing was replaced");
    CHECK(fipsd_gw_pool_test_evicted_total(p) == 0);
    fipsd_gw_pool_free(p);
}
static uint32_t requery_ttl(fipsd_gw_pool *p, fipsd_gw_instant t0, uint64_t secs)
{
    fips_node_addr n = gw_node(1); struct in6_addr m = gw_mesh(1); fipsd_gw_allocation al;
    CHECK(alloc_at(p, &n, &m, t0 + SECS(secs), &al) == 0);
    return al.ttl;
}
static void a_never_used_answer_never_outlives_its_ttl_from_creation(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(10, 60);
    alloc_ok(p, 1, t0);
    CHECK(requery_ttl(p, t0, 40) == 20);
    CHECK(requery_ttl(p, t0, 100) == 0);
    mark_used(p, 1, t0 + SECS(101));
    CHECKM(requery_ttl(p, t0, 102) == 60, "a used mapping gets the full TTL");
    fipsd_gw_pool_free(p);
}
static void a_used_mapping_survives_a_flood_that_fills_the_ceiling(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(4, 600);
    struct in6_addr vip = alloc_ok(p, 1, t0);
    mark_used(p, 1, t0);
    fipsd_gw_allocation al;
    for (uint32_t i = 100; i < 100 + 2 * 4; ++i) {
        fips_node_addr n = gw_node_n(i); struct in6_addr m = gw_mesh_n(i);
        CHECK(alloc_at(p, &n, &m, t0, &al) == 0);
        CHECK(!(al.evicted.present && fipsd_gw_ip6_eq(&al.evicted.virtual_ip, &vip)));
    }
    struct in6_addr again; bool is_new;
    CHECK(alloc(p, 1, t0, &again, &is_new) == 0 && fipsd_gw_ip6_eq(&again, &vip) && !is_new);
    CHECK(fipsd_gw_pool_len(p) == 4);
    fipsd_gw_pool_free(p);
}
static void a_pinned_never_used_mapping_stays_mapped_below_the_ceiling(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = fast_pool(10, 60);
    struct in6_addr vip = alloc_ok(p, 1, t0);
    gw_sessions pinned = {0}; gw_sessions_set(&pinned, vip, 1); gw_sessions_bind(&pinned, vip, gw_mesh(1), false);
    fipsd_gw_instant now = t0;
    while (now < t0 + SECS(300)) { now += SECS(10); CHECK(tick_events(p, now, &pinned) == 0); }
    fips_node_addr n = gw_node(1); struct in6_addr m = gw_mesh(1); fipsd_gw_allocation al;
    CHECK(alloc_at(p, &n, &m, now, &al) == 0);
    CHECK(fipsd_gw_ip6_eq(&al.virtual_ip, &vip) && al.ttl == 0);
    CHECK(!mapping(p, 1)->used);
    fipsd_gw_pool_free(p);
}
static void test_mapping_lifecycle_allocated_to_free(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 1, 1);
    gw_sessions ct = {0};
    fipsd_gw_instant now = gw_t0();
    alloc_ok(p, 1, now);
    CHECK(tick_events(p, now, &ct) == 0); CHECK(fipsd_gw_pool_len(p) == 1);
    fipsd_gw_instant later = now + SECS(2);
    CHECK(tick_events(p, later, &ct) == 0); CHECK(fipsd_gw_pool_len(p) == 1);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_DRAINING);
    fipsd_gw_instant after_grace = later + SECS(2);
    fipsd_gw_events ev = {0};
    CHECK(tick(p, after_grace, &ct, &ev) == 0);
    CHECK(ev.len == 1 && ev.items[0].kind == FIPSD_GW_EVENT_MAPPING_REMOVED);
    CHECK(fipsd_gw_pool_len(p) == 0);
    fipsd_gw_pool_nat_removed(p, &ev.items[0].virtual_ip, &ev.items[0].mesh_addr);
    fipsd_gw_events_clear(&ev);
    for (uint32_t k = 1; k <= FIPSD_GW_MAX_ABSENT_READS; ++k) CHECK(tick(p, after_grace + SECS(k), &ct, NULL) == 0);
    CHECK(fipsd_gw_pool_test_free_len(p) == 255);
    fipsd_gw_pool_free(p);
}
static void dns_renewal_preserves_the_full_ttl_after_draining(void)
{
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = limited("fd01::/120", 60, 60, 1, 1, 1);
    gw_sessions ct = {0};
    fips_node_addr node = gw_node(1); struct in6_addr mesh = gw_mesh(1); fipsd_gw_allocation al;
    CHECK(alloc_at(p, &node, &mesh, t0, &al) == 0);
    struct in6_addr vip = al.virtual_ip;
    mark_used(p, 1, t0);
    CHECK(tick(p, t0 + SECS(61), &ct, NULL) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_DRAINING);
    fipsd_gw_instant renewed = t0 + SECS(120);
    CHECK(alloc_at(p, &node, &mesh, renewed, &al) == 0);
    CHECK(fipsd_gw_ip6_eq(&al.virtual_ip, &vip) && !al.is_new);
    CHECKM(al.ttl == 60, "a used mapping is answered with the full TTL");
    CHECK(fipsd_gw_pool_test_tokens(p) == 0);
    CHECK(tick_events(p, t0 + SECS(122), &ct) == 0);
    CHECK(tick_events(p, renewed + SECS(60), &ct) == 0);
    const fipsd_gw_mapping *m = fipsd_gw_pool_lookup_virtual_ip(p, &vip);
    CHECK(m && !memcmp(&m->node_addr, &node, sizeof node));
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_ALLOCATED);
    fipsd_gw_instant drained = renewed + SECS(61);
    CHECK(tick_events(p, drained, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_DRAINING);
    CHECK(tick_events(p, drained + SECS(60), &ct) == 0);
    fipsd_gw_events ev = {0};
    CHECK(tick(p, drained + SECS(61), &ct, &ev) == 0);
    CHECK(ev.len == 1 && ev.items[0].kind == FIPSD_GW_EVENT_MAPPING_REMOVED);
    fipsd_gw_events_clear(&ev);
    CHECK(!fipsd_gw_pool_lookup_virtual_ip(p, &vip));
    fipsd_gw_pool_free(p);
}
static void dns_refresh_without_an_address_cancels_draining(void)
{
    fipsd_gw_instant now = gw_t0();
    fipsd_gw_pool *p = pool_new("fd01::/120", 60, 10);
    gw_sessions ct = {0};
    fips_node_addr node = gw_node(1), other = gw_node(2);
    fipsd_gw_instant created = now - SECS(62);
    alloc_ok(p, 1, created);
    mark_used(p, 1, created);
    CHECK(tick(p, now - SECS(1), &ct, NULL) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_DRAINING);
    CHECK(fipsd_gw_pool_refresh_at(p, &node, now));
    CHECK(!fipsd_gw_pool_refresh_at(p, &other, now));
    CHECK(fipsd_gw_pool_len(p) == 1);
    CHECK(tick_events(p, now + SECS(11), &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_ALLOCATED);
    CHECK(!mapping(p, 1)->has_drain_start);
    fipsd_gw_pool_free(p);
}
static void test_mapping_lifecycle_active_draining_free(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 1, 1);
    gw_sessions ct = {0};
    fipsd_gw_instant now = gw_t0();
    struct in6_addr vip = alloc_ok(p, 1, now);
    gw_sessions_set(&ct, vip, 3);
    CHECK(tick_events(p, now, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_ACTIVE);
    fipsd_gw_instant later = now + SECS(2);
    gw_sessions_set(&ct, vip, 0);
    CHECK(tick_events(p, later, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_DRAINING);
    CHECK(tick_events(p, later, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_DRAINING);
    CHECK(tick_events(p, later + SECS(2), &ct) == 1);
    CHECK(fipsd_gw_pool_len(p) == 0);
    fipsd_gw_pool_free(p);
}
static void test_active_traffic_never_reclaimed(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 1, 1);
    gw_sessions ct = {0};
    fipsd_gw_instant t = gw_t0();
    struct in6_addr vip = alloc_ok(p, 1, t);
    gw_sessions_set(&ct, vip, 2);
    CHECK(tick_events(p, t, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_ACTIVE);
    for (int k = 0; k < 10; ++k) {
        t += SECS(5);
        CHECKM(tick_events(p, t, &ct) == 0, "mapping must not be reclaimed");
        CHECKM(mapping(p, 1)->state == FIPSD_GW_MAPPING_ACTIVE, "mapping must stay Active while traffic flows");
    }
    CHECK(fipsd_gw_pool_len(p) == 1);
    fipsd_gw_pool_free(p);
}
static void test_bursty_draining_recovers_to_active(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 1, 5);
    gw_sessions ct = {0};
    fipsd_gw_instant now = gw_t0();
    struct in6_addr vip = alloc_ok(p, 1, now);
    gw_sessions_set(&ct, vip, 1);
    CHECK(tick_events(p, now, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_ACTIVE);
    fipsd_gw_instant drained = now + SECS(2);
    gw_sessions_set(&ct, vip, 0);
    CHECK(tick_events(p, drained, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_DRAINING);
    gw_sessions_set(&ct, vip, 3);
    CHECK(tick_events(p, drained + SECS(2), &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_ACTIVE);
    CHECK(!mapping(p, 1)->has_drain_start);
    CHECK(fipsd_gw_pool_len(p) == 1);
    fipsd_gw_pool_free(p);
}
static void test_redrain_honors_fresh_grace_window(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 1, 5);
    gw_sessions ct = {0};
    fipsd_gw_instant now = gw_t0();
    struct in6_addr vip = alloc_ok(p, 1, now);
    gw_sessions_set(&ct, vip, 1);
    CHECK(tick_events(p, now, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_ACTIVE);
    fipsd_gw_instant first_drain = now + SECS(2);
    gw_sessions_set(&ct, vip, 0);
    CHECK(tick_events(p, first_drain, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_DRAINING);
    fipsd_gw_instant recover = first_drain + SECS(2);
    gw_sessions_set(&ct, vip, 2);
    CHECK(tick_events(p, recover, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_ACTIVE);
    fipsd_gw_instant second_drain = recover + SECS(2);
    gw_sessions_set(&ct, vip, 0);
    CHECK(tick_events(p, second_drain, &ct) == 0);
    CHECK(mapping(p, 1)->state == FIPSD_GW_MAPPING_DRAINING);
    CHECKM(tick_events(p, second_drain + SECS(4), &ct) == 0, "fresh grace window must be honored");
    CHECK(fipsd_gw_pool_len(p) == 1);
    CHECK(tick_events(p, second_drain + SECS(6), &ct) == 1);
    CHECK(fipsd_gw_pool_len(p) == 0);
    fipsd_gw_pool_free(p);
}
static void test_pool_status(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 60, 60);
    fipsd_gw_pool_status s;
    fipsd_gw_pool_get_status(p, &s);
    CHECK(s.total == 255 && s.free == 255 && s.allocated == 0);
    alloc_ok(p, 1, gw_t0());
    fipsd_gw_pool_get_status(p, &s);
    CHECK(s.allocated == 1 && s.free == 254);
    fipsd_gw_pool_free(p);
}
static void test_lookup_virtual_ip(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/120", 60, 60);
    fips_node_addr node = gw_node(1); struct in6_addr mesh = gw_mesh(1);
    struct in6_addr vip = alloc_ok(p, 1, gw_t0());
    const fipsd_gw_mapping *m = fipsd_gw_pool_lookup_virtual_ip(p, &vip);
    CHECK(m && !memcmp(&m->node_addr, &node, sizeof node) && fipsd_gw_ip6_eq(&m->mesh_addr, &mesh));
    struct in6_addr unknown = gw_v6("fd01::ff");
    CHECK(!fipsd_gw_pool_lookup_virtual_ip(p, &unknown));
    fipsd_gw_pool_free(p);
}
static void test_large_prefix_capped(void)
{
    fipsd_gw_pool *p = pool_new("fd01::/96", 60, 60);
    CHECK(fipsd_gw_pool_test_total(p) == 65535);
    fipsd_gw_pool_free(p);
}
static void the_mapping_info_reports_every_live_mapping(void)
{
    /* Upstream's MappingInfo, as the control socket's show_mappings reads it. */
    fipsd_gw_instant t0 = gw_t0();
    fipsd_gw_pool *p = pool_new("fd01::/120", 60, 60);
    alloc_ok(p, 1, t0); alloc_ok(p, 2, t0 + SECS(3));
    fipsd_gw_mapping_info *info; size_t n;
    CHECK(fipsd_gw_pool_mapping_info(p, t0 + SECS(10), &info, &n) == 0);
    CHECK(n == 2);
    for (size_t i = 0; i < n; ++i) {
        CHECK(!strcmp(info[i].dns_name, "test.fips"));
        CHECK(info[i].age_secs == (info[i].node_addr.bytes[0] == 1 ? 10u : 7u));
        CHECK(info[i].state == FIPSD_GW_MAPPING_ALLOCATED && !info[i].used);
    }
    free(info); fipsd_gw_pool_free(p);
}

/* The pool reserves its maps before it changes anything, so the insert at the
 * end of an allocation cannot fail; a reserved map must not grow on set. */
static void a_reserved_map_takes_the_next_key_without_growing(void)
{
    fipsd_gw_map m; fipsd_gw_map_init(&m, 4);
    for (uint32_t k = 0; k < 200; ++k) {
        CHECK(fipsd_gw_map_reserve(&m) == 0);
        size_t cap = m.cap;
        if (k % 3 == 2) { uint32_t gone = k - 1; CHECK(fipsd_gw_map_remove(&m, &gone)); }
        CHECK(fipsd_gw_map_set(&m, &k, k) == 0);
        CHECKM(m.cap == cap, "key %u grew a reserved map", k);
    }
    fipsd_gw_map_clear(&m);
}
int gw_suite_pool(void)
{
    /* The /112 restart test cycles through 65,535 addresses with explicit
     * simulated time. Per-address INFO output otherwise dominates the test
     * through CTest's capture pipe, especially under sanitizer/build load.
     * Keep warnings/errors and case timings; logging has its own suite. */
    fipsd_log *previous_log = fipsd_log_default();
    fipsd_log *log = fipsd_log_create(FIPSD_LOG_WARN, NULL);
    CHECK(log);
    fipsd_log_set_default(log);
    RUN(a_reserved_map_takes_the_next_key_without_growing);
    RUN(pool_error_reasons_are_the_kebab_case_log_values);
    RUN(test_parse_cidr);
    RUN(test_parse_cidr_invalid);
    RUN(a_pool_cidr_with_host_bits_set_issues_only_addresses_inside_its_prefix);
    RUN(a_pool_with_no_addresses_is_refused_at_start);
    RUN(test_pool_creation);
    RUN(test_pool_allocation);
    RUN(test_pool_idempotent);
    RUN(test_pool_exhaustion);
    RUN(ceiling_refuses_only_when_every_mapping_has_carried_traffic);
    RUN(ceiling_is_checked_before_the_rate_limit);
    RUN(rate_limit_refuses_a_burst_keeps_existing_names_and_refills);
    RUN(exhausted_pool_takes_no_token);
    RUN(a_restored_pool_does_not_reissue_the_previous_runs_addresses);
    RUN(a_restored_pool_holds_a_live_mapping_from_before_the_last_lap);
    RUN(a_restored_slash_112_pool_holds_a_live_mapping_just_past_the_reserve);
    RUN(a_full_lap_stretch_survives_a_restart);
    RUN(the_mark_is_never_passed_until_the_next_one_is_confirmed);
    RUN(a_late_tick_at_full_rate_never_hits_the_mark);
    RUN(a_run_of_mapped_addresses_ahead_of_the_cursor_does_not_starve_the_mark);
    RUN(restart_holds_release_after_the_loaded_hold_and_not_before);
    RUN(a_restored_pool_writes_a_hold_no_shorter_than_what_remains_of_its_loaded_one);
    RUN(a_clean_shutdown_state_holds_only_live_addresses);
    RUN(a_new_name_at_the_ceiling_replaces_the_oldest_never_used_mapping);
    RUN(a_lan_host_requerying_a_thousand_unused_names_does_not_hold_the_ceiling);
    RUN(mesh_addresses_inside_a_wide_pool_prefix_cannot_hold_the_ceiling);
    RUN(a_mesh_address_outside_the_pool_prefix_still_maps_when_it_shares_a_wider_prefix);
    RUN(requerying_a_never_used_name_does_not_keep_it_past_ttl_and_grace);
    RUN(an_evicted_address_is_not_reissued_before_its_answer_deadline);
    RUN(a_reply_from_the_virtual_ip_itself_does_not_mark_a_mapping_used);
    RUN(a_reply_from_a_previous_holders_mesh_address_does_not_mark_the_new_mapping_used);
    RUN(an_address_is_not_given_back_to_a_node_whose_old_binding_to_it_survives);
    RUN(a_binding_missed_by_one_partial_read_still_blocks_the_reissue);
    RUN(unread_ticks_do_not_age_a_surviving_binding);
    RUN(a_removed_address_waits_for_its_nat_removal_and_later_reads);
    RUN(a_pinned_never_used_address_is_held_for_grace_after_its_last_answer);
    RUN(evidence_recovers_after_a_failed_start_probe_once_reads_succeed);
    RUN(a_mapping_made_before_a_started_pool_has_enough_reads_cannot_become_used);
    RUN(without_evidence_removed_addresses_wait_only_for_their_deadline_and_nat_removal);
    RUN(evidence_turns_off_after_unread_ticks_and_back_on_after_enough_reads);
    RUN(a_used_mapping_stays_protected_while_evidence_is_off);
    RUN(an_eviction_spends_a_token);
    RUN(a_never_used_answer_never_outlives_its_ttl_from_creation);
    RUN(a_used_mapping_survives_a_flood_that_fills_the_ceiling);
    RUN(a_pinned_never_used_mapping_stays_mapped_below_the_ceiling);
    RUN(test_mapping_lifecycle_allocated_to_free);
    RUN(dns_renewal_preserves_the_full_ttl_after_draining);
    RUN(dns_refresh_without_an_address_cancels_draining);
    RUN(test_mapping_lifecycle_active_draining_free);
    RUN(test_active_traffic_never_reclaimed);
    RUN(test_bursty_draining_recovers_to_active);
    RUN(test_redrain_honors_fresh_grace_window);
    RUN(test_pool_status);
    RUN(test_lookup_virtual_ip);
    RUN(test_large_prefix_capped);
    RUN(the_mapping_info_reports_every_live_mapping);
    fipsd_log_set_default(previous_log);
    fipsd_log_destroy(log);
    return 0;
}
