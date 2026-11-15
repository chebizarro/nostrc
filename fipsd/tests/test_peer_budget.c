#include "peer_test.h"
#include "peer/budget.h"
int peer_budget_tests(void)
{
    fipsd_transport_caps limits[] = {{{1}, {1, 1, 1}}, {{2}, {2, 2, 1}}};
    fipsd_peer_budget *b = fipsd_peer_budget_create((fipsd_peer_caps){2, 2, 2}, limits, 2, 3);
    CHECK(b);
    fipsd_peer_ticket a = {0}, c = {0}, d = {0}, other = {0};
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){1}, true, &a) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){1}, true, &other) == FIPSD_ADMIT_TRANSPORT);
    CHECK(!other.owner);
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){2}, true, &c) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){3}, true, &d) == FIPSD_ADMIT_TOTAL);
    CHECK(fipsd_peer_budget_promote(&a) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_budget_promote(&c) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_budget_used(b).connections == 0 && fipsd_peer_budget_used(b).peers == 2);
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){3}, false, &d) == FIPSD_ADMIT_TOTAL);
    CHECK(!fipsd_peer_budget_destroy(b));
    CHECK(fipsd_peer_budget_rebind(&c, (fips_transport_id){1}) == FIPSD_ADMIT_TRANSPORT);
    CHECK(c.transport.value == 2);
    fipsd_peer_budget_release(&a);
    CHECK(fipsd_peer_budget_rebind(&c, (fips_transport_id){1}) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){1}, false, &d) == FIPSD_ADMIT_TRANSPORT);
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){3}, false, &d) == FIPSD_ADMIT_OK);
    fipsd_peer_budget_release(&d); fipsd_peer_budget_release(&d);
    /* Unknown-transport churn reclaims empty slots instead of leaking capacity. */
    for (uint32_t id = 4; id < 100; ++id) {
        CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){id}, false, &d) == FIPSD_ADMIT_OK);
        fipsd_peer_budget_release(&d);
    }
    fipsd_peer_budget_release(&c); CHECK(fipsd_peer_budget_destroy(b));
    /* Each total limb independently binds; promotion refusal preserves pending. */
    b = fipsd_peer_budget_create((fipsd_peer_caps){0, 1, 0}, NULL, 0, 1);
    CHECK(b);
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){1}, false, &a) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){1}, true, &c) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_budget_promote(&c) == FIPSD_ADMIT_TOTAL && c.pending);
    CHECK(fipsd_peer_budget_used(b).connections == 1);
    fipsd_peer_budget_release(&a); CHECK(fipsd_peer_budget_promote(&c) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_budget_acquire(b, (fips_transport_id){2}, true, &d) == FIPSD_ADMIT_STORAGE);
    fipsd_peer_budget_release(&c); CHECK(fipsd_peer_budget_destroy(b));
    limits[1].transport = limits[0].transport;
    CHECK(!fipsd_peer_budget_create((fipsd_peer_caps){0}, limits, 2, 2));
    /* Upstream per-peer routing-error and earned coordinate-hint budgets. */
    fipsd_peer_tokens tokens;
    fipsd_peer_tokens_init(&tokens, 20, 50, 50, fips_test_instant(0));
    CHECK(fipsd_peer_tokens_available(&tokens, fips_test_instant(0)) == 50);
    CHECK(fipsd_peer_tokens_spend(&tokens, 50, fips_test_instant(0)));
    CHECK(!fipsd_peer_tokens_spend(&tokens, 1, fips_test_instant(49)));
    CHECK(fipsd_peer_tokens_spend(&tokens, 1, fips_test_instant(50)));
    CHECK(fipsd_peer_tokens_available(&tokens, fips_test_instant(UINT64_MAX)) == 50);
    CHECK(!fipsd_peer_tokens_spend(&tokens, UINT64_MAX, fips_test_instant(UINT64_MAX)));
    fipsd_peer_tokens_init(&tokens, 64 * 16, 256 * 16, 0, fips_test_instant(0));
    CHECK(!fipsd_peer_tokens_available(&tokens, fips_test_instant(0)));
    CHECK(fipsd_peer_tokens_available(&tokens, fips_test_instant(4000)) == 4096);
    CHECK(fipsd_peer_tokens_spend(&tokens, 4096, fips_test_instant(4000)));
    return 0;
}
