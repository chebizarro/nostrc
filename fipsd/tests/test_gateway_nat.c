/* NAT table tests (nostrc-iq1n9.3), from upstream src/gateway/nat.rs and
 * tests/nat.rs: the rendered libnftables batch against golden JSON, the rule
 * shapes upstream asserts one by one, the pending-rebuild and removal
 * bookkeeping over a stand-in backend, the outcome latches, and the worker
 * over a fake table. Set FIPSD_GATEWAY_REGEN_GOLDEN=1 to rewrite the goldens. */
#include "test_gateway.h"
#include "gateway/nat.h"
#include <jansson.h>
#include <nsync.h>
#include <pthread.h>

#ifndef FIPSD_GATEWAY_GOLDEN_DIR
#error FIPSD_GATEWAY_GOLDEN_DIR must name the golden directory
#endif

/* ---- a stand-in backend --------------------------------------------------- */
typedef struct backend { int fail; int calls; char *last; } backend;
static int backend_apply(void *user, const char *batch, char *err, size_t cap)
{
    backend *b = user;
    b->calls++;
    free(b->last); b->last = strdup(batch);
    if (b->fail) { snprintf(err, cap, "Could not process rule: Operation not permitted"); return -1; }
    return 0;
}
static fipsd_gw_nat *nat_state(backend *b, const char *lan, const char *tun, const char *pool, uint8_t len)
{
    struct in6_addr net = gw_v6(pool);
    fipsd_gw_nat *n = fipsd_gw_nat_new_state(FIPSD_GW_NAT_TABLE, lan, tun, &net, len, (fipsd_gw_nft_backend){backend_apply, b});
    CHECK(n);
    return n;
}
static fipsd_gw_port_forward forward(uint16_t listen, fipsd_gw_proto proto, const char *target)
{
    fipsd_gw_port_forward f; memset(&f, 0, sizeof f);
    f.listen_port = listen; f.proto = proto;
    CHECK(fipsd_gw_parse_target(target, &f.target, &f.target_port));
    return f;
}
static json_t *parse_batch(const char *text) { json_error_t e; json_t *j = json_loads(text, 0, &e); CHECKM(j, "%s", e.text); return j; }
static json_t *commands(json_t *batch) { json_t *c = json_object_get(batch, "nftables"); CHECK(json_is_array(c)); return c; }

static void golden(const char *name, const char *rendered)
{
    char path[1024]; snprintf(path, sizeof path, "%s/%s", FIPSD_GATEWAY_GOLDEN_DIR, name);
    json_t *got = parse_batch(rendered);
    const char *regen = getenv("FIPSD_GATEWAY_REGEN_GOLDEN");
    if (regen && !strcmp(regen, "1")) { CHECK(json_dump_file(got, path, JSON_INDENT(2)) == 0); json_decref(got); return; }
    json_error_t e;
    json_t *want = json_load_file(path, 0, &e);
    CHECKM(want, "cannot read golden %s: %s", path, e.text);
    if (!json_equal(got, want)) {
        char *g = json_dumps(got, JSON_INDENT(2)), *w = json_dumps(want, JSON_INDENT(2));
        fprintf(stderr, "rendered batch differs from %s\n--- rendered\n%s\n--- golden\n%s\n", path, g, w);
        free(g); free(w);
        CHECK(false);
    }
    json_decref(got); json_decref(want);
}
/* The full table of the goldens: two mappings added out of order, a TCP and
 * a UDP forward. */
static fipsd_gw_nat *full_table(backend *b)
{
    fipsd_gw_nat *n = nat_state(b, "br-lan", "fips0", "fd01::", 112);
    struct in6_addr v2 = gw_v6("fd01::2"), m2 = gw_v6("fd9a::2"), v1 = gw_v6("fd01::1"), m1 = gw_v6("fd9a::1");
    fipsd_gw_nat_test_set_mapping(n, &v2, &m2);
    fipsd_gw_nat_test_set_mapping(n, &v1, &m1);
    fipsd_gw_port_forward f[] = {forward(8080, FIPSD_GW_PROTO_TCP, "[fd02::5]:80"), forward(5353, FIPSD_GW_PROTO_UDP, "[fd02::6]:53")};
    fipsd_gw_nat_error err;
    CHECK(fipsd_gw_nat_set_port_forwards(n, f, 2, &err) == 0);
    return n;
}

/* The commands of a batch as "verb kind[:chain]" strings, for order checks. */
static size_t find_cmd(json_t *cmds, size_t from, const char *verb, const char *kind, const char *chain)
{
    for (size_t i = from; i < json_array_size(cmds); ++i) {
        json_t *obj = json_object_get(json_object_get(json_array_get(cmds, i), verb), kind);
        if (!obj) continue;
        if (chain && strcmp(json_string_value(json_object_get(obj, "chain")) ? json_string_value(json_object_get(obj, "chain")) : "", chain)) continue;
        return i;
    }
    return SIZE_MAX;
}
static size_t count_cmd(json_t *cmds, const char *verb, const char *kind, const char *chain)
{
    size_t n = 0, at = 0;
    while ((at = find_cmd(cmds, at, verb, kind, chain)) != SIZE_MAX) { n++; at++; }
    return n;
}
/* The rule's expression list as compact text, to look for a statement in it. */
static char *rule_text(json_t *cmds, size_t i) { return json_dumps(json_object_get(json_object_get(json_object_get(json_array_get(cmds, i), "add"), "rule"), "expr"), JSON_COMPACT); }

static void rebuild_matches_the_golden_batch_with_mappings_and_forwards(void)
{
    backend b = {0};
    fipsd_gw_nat *n = full_table(&b);
    char *r = fipsd_gw_nat_render(n);
    golden("nat_full.json", r);
    CHECKM(b.last && !strcmp(b.last, r), "the backend was handed the rendered batch");
    free(r); free(b.last); fipsd_gw_nat_free(n);
}
static void an_empty_table_matches_its_golden_and_sends_no_element_list(void)
{
    backend b = {0};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    char *r = fipsd_gw_nat_render(n);
    golden("nat_empty.json", r);
    json_t *j = parse_batch(r);
    CHECKM(count_cmd(commands(j), "add", "element", NULL) == 0, "no element list is sent without mappings");
    CHECKM(count_cmd(commands(j), "add", "set", NULL) == 1, "the set exists whatever the mappings");
    CHECK(count_cmd(commands(j), "add", "rule", "postrouting") == 2);
    json_decref(j); free(r); fipsd_gw_nat_free(n);
}
static void the_cleanup_batch_matches_its_golden_and_is_idempotent(void)
{
    backend b = {0};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    char *r = fipsd_gw_nat_render_cleanup(n);
    golden("nat_cleanup.json", r);
    json_t *j = parse_batch(r), *c = commands(j);
    CHECKM(find_cmd(c, 0, "add", "table", NULL) < find_cmd(c, 0, "delete", "table", NULL), "the add gives the delete a target");
    CHECK(count_cmd(c, "add", "rule", NULL) == 0);
    fipsd_gw_nat_error err;
    CHECK(fipsd_gw_nat_cleanup(n, &err) == 0 && b.calls == 1);
    json_decref(j); free(r); free(b.last); fipsd_gw_nat_free(n);
}
static void rebuild_deletes_and_recreates_the_table_inside_one_batch(void)
{
    backend b = {0};
    fipsd_gw_nat *n = full_table(&b);
    char *r = fipsd_gw_nat_render(n);
    json_t *j = parse_batch(r), *c = commands(j);
    CHECK(json_object_get(json_array_get(c, 0), "metainfo"));
    CHECK(find_cmd(c, 0, "add", "table", NULL) == 1 && find_cmd(c, 0, "delete", "table", NULL) == 2 && find_cmd(c, 2, "add", "table", NULL) == 3);
    CHECKM(count_cmd(c, "delete", "table", NULL) == 1, "the table is deleted exactly once");
    CHECKM(find_cmd(c, 0, "delete", "table", NULL) < find_cmd(c, 0, "add", "rule", NULL), "and before every rule");
    json_decref(j); free(r); free(b.last); fipsd_gw_nat_free(n);
}
static void rebuild_adds_the_chains_with_their_hooks(void)
{
    backend b = {0};
    fipsd_gw_nat *n = full_table(&b);
    char *r = fipsd_gw_nat_render(n);
    json_t *j = parse_batch(r), *c = commands(j);
    static const struct { const char *name, *type, *hook; int prio; } chains[] = {
        {"prerouting", "nat", "prerouting", -100}, {"output", "nat", "output", -100}, {"postrouting", "nat", "postrouting", 100},
        {"forward", "filter", "forward", 0}, {"raw_prerouting", "filter", "prerouting", -300}, {"input", "filter", "input", 0}};
    for (size_t k = 0; k < 6; ++k) {
        bool found = false;
        for (size_t i = 0; i < json_array_size(c); ++i) {
            json_t *ch = json_object_get(json_object_get(json_array_get(c, i), "add"), "chain");
            if (!ch || strcmp(json_string_value(json_object_get(ch, "name")), chains[k].name)) continue;
            found = true;
            CHECK(!strcmp(json_string_value(json_object_get(ch, "type")), chains[k].type));
            CHECK(!strcmp(json_string_value(json_object_get(ch, "hook")), chains[k].hook));
            CHECK(json_integer_value(json_object_get(ch, "prio")) == chains[k].prio);
            CHECKM(i < find_cmd(c, 0, "add", "rule", chains[k].name), "chain %s is added before its rules", chains[k].name);
        }
        CHECKM(found, "chain %s", chains[k].name);
    }
    CHECKM(find_cmd(c, 0, "add", "set", NULL) < find_cmd(c, 0, "add", "rule", "output"), "the output DNAT is added after the set and chain it uses");
    json_decref(j); free(r); free(b.last); fipsd_gw_nat_free(n);
}
static void rebuild_emits_a_dnat_output_dnat_and_snat_for_every_mapping(void)
{
    backend b = {0};
    fipsd_gw_nat *n = full_table(&b);
    char *r = fipsd_gw_nat_render(n);
    json_t *j = parse_batch(r), *c = commands(j);
    /* Two mappings, two forwards. */
    CHECK(count_cmd(c, "add", "rule", "output") == 2);
    CHECK(count_cmd(c, "add", "rule", "prerouting") == 4);
    CHECK(count_cmd(c, "add", "rule", "postrouting") == 2 + 1 + 2);
    const char *vips[] = {"fd01::1", "fd01::2"}, *mesh[] = {"fd9a::1", "fd9a::2"};
    for (int k = 0; k < 2; ++k) {
        char dnat[128], snat[128], match_vip[128];
        snprintf(dnat, sizeof dnat, "{\"dnat\":{\"family\":\"ip6\",\"addr\":\"%s\"}}", mesh[k]);
        snprintf(snat, sizeof snat, "{\"snat\":{\"family\":\"ip6\",\"addr\":\"%s\"}}", vips[k]);
        snprintf(match_vip, sizeof match_vip, "\"right\":\"%s\"", vips[k]);
        size_t pre = 0, out = 0, post = 0;
        for (size_t i = 0; i < json_array_size(c); ++i) {
            json_t *rule = json_object_get(json_object_get(json_array_get(c, i), "add"), "rule");
            if (!rule) continue;
            char *t = rule_text(c, i);
            const char *chain = json_string_value(json_object_get(rule, "chain"));
            if (strstr(t, dnat) && strstr(t, match_vip)) {
                if (!strcmp(chain, "prerouting")) { pre++; CHECKM(strstr(t, "\"iifname\"}},\"right\":\"br-lan\""), "every mapping DNAT matches only the LAN interface"); }
                if (!strcmp(chain, "output")) { out++; CHECKM(!strstr(t, "iifname") && !strstr(t, "oifname"), "every output DNAT matches no interface"); }
            }
            if (strstr(t, snat) && !strcmp(chain, "postrouting")) post++;
            free(t);
        }
        CHECKM(pre == 1 && out == 1 && post == 1, "mapping %s: %zu dnat %zu output %zu snat", vips[k], pre, out, post);
    }
    json_decref(j); free(r); free(b.last); fipsd_gw_nat_free(n);
}
static void the_masquerades_precede_every_snat_and_the_lan_masquerade_comes_once(void)
{
    backend b = {0};
    fipsd_gw_nat *n = full_table(&b);
    char *r = fipsd_gw_nat_render(n);
    json_t *j = parse_batch(r), *c = commands(j);
    size_t lan_masq = 0, first_snat = SIZE_MAX, last_masq = 0, fips_masq = SIZE_MAX, host_masq = SIZE_MAX;
    for (size_t i = 0; i < json_array_size(c); ++i) {
        json_t *rule = json_object_get(json_object_get(json_array_get(c, i), "add"), "rule");
        if (!rule || strcmp(json_string_value(json_object_get(rule, "chain")), "postrouting")) continue;
        char *t = rule_text(c, i);
        if (strstr(t, "masquerade")) {
            last_masq = i;
            if (strstr(t, "{\"match\":{\"op\":\"==\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":\"fips0\"}}")) lan_masq++;
            else if (strstr(t, "@fips_mesh_sources")) host_masq = i;
            else fips_masq = i;
        }
        if (strstr(t, "\"snat\"") && first_snat == SIZE_MAX) first_snat = i;
        free(t);
    }
    CHECKM(lan_masq == 1, "the LAN masquerade is emitted once when port forwards exist");
    CHECKM(fips_masq < host_masq, "the LAN's fips0 masquerade precedes the host's");
    CHECKM(last_masq < first_snat, "every masquerade precedes every per-mapping SNAT");
    json_decref(j); free(r); free(b.last); fipsd_gw_nat_free(n);
    /* Without forwards, no LAN-side masquerade. */
    backend b2 = {0};
    fipsd_gw_nat *m = nat_state(&b2, "br-lan", "fips0", "fd01::", 112);
    r = fipsd_gw_nat_render(m);
    CHECK(!strstr(r, "\"iifname\"}},\"right\":\"fips0\"}},{\"match\":{\"op\":\"==\",\"left\":{\"meta\":{\"key\":\"oifname\"}},\"right\":\"br-lan\""));
    free(r); fipsd_gw_nat_free(m);
}
static void the_mesh_source_set_holds_every_mapped_mesh_address_once(void)
{
    backend b = {0};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    /* Two virtual IPs for one node (a reissue) and one for another. */
    struct in6_addr v1 = gw_v6("fd01::1"), v2 = gw_v6("fd01::2"), v3 = gw_v6("fd01::3"), m1 = gw_v6("fd9a::7"), m2 = gw_v6("fd9a::3");
    fipsd_gw_nat_test_set_mapping(n, &v1, &m1); fipsd_gw_nat_test_set_mapping(n, &v2, &m1); fipsd_gw_nat_test_set_mapping(n, &v3, &m2);
    char *r = fipsd_gw_nat_render(n);
    json_t *j = parse_batch(r), *c = commands(j);
    size_t at = find_cmd(c, 0, "add", "element", NULL);
    CHECK(at != SIZE_MAX);
    json_t *elem = json_object_get(json_object_get(json_object_get(json_array_get(c, at), "add"), "element"), "elem");
    CHECK(json_array_size(elem) == 2);
    CHECK(!strcmp(json_string_value(json_array_get(elem, 0)), "fd9a::3") && !strcmp(json_string_value(json_array_get(elem, 1)), "fd9a::7"));
    json_decref(j); free(r); fipsd_gw_nat_free(n);
}
static void the_filter_rules_keep_their_shape(void)
{
    backend b = {0};
    fipsd_gw_nat *n = full_table(&b);
    char *r = fipsd_gw_nat_render(n);
    json_t *j = parse_batch(r), *c = commands(j);
    size_t raw = find_cmd(c, 0, "add", "rule", "raw_prerouting"), forged = find_cmd(c, raw + 1, "add", "rule", "raw_prerouting");
    size_t fwd = find_cmd(c, 0, "add", "rule", "forward");
    size_t accept = find_cmd(c, 0, "add", "rule", "input"), reject = find_cmd(c, accept + 1, "add", "rule", "input");
    char *t = rule_text(c, raw);
    CHECKM(strstr(t, "\"iifname\"}},\"right\":\"br-lan\"") && strstr(t, "\"right\":\"lo\"") && strstr(t, "\"prefix\":{\"addr\":\"fd01::\",\"len\":112}") &&
           strstr(t, "{\"drop\":null}"), "traffic to the pool from other interfaces is dropped before conntrack: %s", t);
    free(t); t = rule_text(c, forged);
    CHECKM(strstr(t, "\"op\":\"!=\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":\"fips0\"") && strstr(t, "\"right\":\"lo\"") &&
           strstr(t, "\"field\":\"saddr\"}},\"right\":\"@fips_mesh_sources\"") && strstr(t, "{\"drop\":null}"),
           "forged mesh sources are dropped unless they arrive on fips0 or loopback: %s", t);
    free(t); t = rule_text(c, fwd);
    CHECKM(strstr(t, "\"oifname\"}},\"right\":\"fips0\"") && strstr(t, "\"op\":\"!=\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":\"br-lan\"") &&
           strstr(t, "{\"match\":{\"op\":\"!\",\"left\":{\"ct\":{\"key\":\"state\"}},\"right\":[\"established\",\"related\"]}}"),
           "new flows into fips0 from other interfaces are dropped: %s", t);
    free(t); t = rule_text(c, accept);
    CHECKM(strstr(t, "nd-neighbor-solicit") && strstr(t, "{\"accept\":null}"), "a neighbour solicitation to the pool is accepted before the reject: %s", t);
    free(t); t = rule_text(c, reject);
    CHECKM(strstr(t, "{\"reject\":{\"type\":\"icmpx\",\"expr\":\"port-unreachable\"}}") && strstr(t, "\"len\":112"),
           "traffic still addressed to the pool at input is rejected: %s", t);
    free(t);
    json_decref(j); free(r); free(b.last); fipsd_gw_nat_free(n);
}
static void the_pool_drop_matches_a_pool_configured_with_host_bits_set(void)
{
    backend b = {0};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::1", 112);
    char *r = fipsd_gw_nat_render(n);
    CHECKM(strstr(r, "\"prefix\":{\"addr\":\"fd01::\",\"len\":112}") && !strstr(r, "\"addr\":\"fd01::1\",\"len\""), "the drop matches the prefix, not the address as written");
    free(r); fipsd_gw_nat_free(n);
}
static void every_tun_match_uses_the_configured_tun_name(void)
{
    backend b = {0};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fipsx9", "fd01::", 112);
    struct in6_addr v = gw_v6("fd01::1"), m = gw_v6("fd9a::1");
    fipsd_gw_nat_test_set_mapping(n, &v, &m);
    fipsd_gw_port_forward f = forward(22, FIPSD_GW_PROTO_TCP, "[fd02::5]:22");
    fipsd_gw_nat_error err;
    CHECK(fipsd_gw_nat_set_port_forwards(n, &f, 1, &err) == 0);
    char *r = fipsd_gw_nat_render(n);
    CHECKM(!strstr(r, "\"fips0\""), "a TUN match still names fips0");
    size_t uses = 0;
    for (const char *p = r; (p = strstr(p, "\"fipsx9\"")); ++p) uses++;
    CHECKM(uses == 6, "the forged-source drop, the forward drop, both fips0 masquerades, the LAN masquerade and the forward DNAT (%zu)", uses);
    free(r); free(b.last); fipsd_gw_nat_free(n);
}
static void a_failed_rebuild_stays_pending_and_retries_the_latest_desired_state(void)
{
    backend b = {.fail = 1};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    fipsd_gw_nat_command add = {FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::1")};
    fipsd_gw_nat_applied applied;
    CHECK(fipsd_gw_nat_apply(n, &add, 1, &applied) == 0);
    CHECK(applied.outcomes[0].kind == FIPSD_GW_NAT_NFTABLES && strstr(applied.outcomes[0].message, "Operation not permitted"));
    fipsd_gw_nat_applied_clear(&applied);
    CHECKM(fipsd_gw_nat_rebuild_pending(n), "a failure leaves the rebuild pending");
    fipsd_gw_nat_command add2 = {FIPSD_GW_NAT_ADD, gw_v6("fd01::2"), gw_v6("fd9a::2")};
    CHECK(fipsd_gw_nat_apply(n, &add2, 1, &applied) == 0); fipsd_gw_nat_applied_clear(&applied);
    b.fail = 0;
    fipsd_gw_pairs removed = {0}; fipsd_gw_nat_error err;
    CHECKM(fipsd_gw_nat_retry_pending(n, &removed, &err) == 1, "the retry rebuilds");
    CHECKM(strstr(b.last, "fd01::1") && strstr(b.last, "fd01::2"), "the retry carries the latest desired state");
    CHECKM(!fipsd_gw_nat_rebuild_pending(n), "a success clears the pending flag");
    int calls = b.calls;
    CHECKM(fipsd_gw_nat_retry_pending(n, &removed, &err) == 0 && b.calls == calls, "a clean table is not retried");
    fipsd_gw_pairs_clear(&removed); free(b.last); fipsd_gw_nat_free(n);
}
static void a_failed_port_forward_rebuild_remains_pending(void)
{
    backend b = {.fail = 1};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    fipsd_gw_port_forward f = forward(22, FIPSD_GW_PROTO_TCP, "[fd02::5]:22");
    fipsd_gw_nat_error err;
    CHECK(fipsd_gw_nat_set_port_forwards(n, &f, 1, &err) != 0 && err.kind == FIPSD_GW_NAT_NFTABLES);
    CHECK(fipsd_gw_nat_rebuild_pending(n));
    free(b.last); fipsd_gw_nat_free(n);
}
static bool has_pair(const fipsd_gw_pairs *p, const char *v, const char *m)
{
    struct in6_addr a = gw_v6(v), b = gw_v6(m);
    for (size_t i = 0; i < p->len; ++i) if (fipsd_gw_ip6_eq(&p->items[i].virtual_ip, &a) && fipsd_gw_ip6_eq(&p->items[i].mesh_addr, &b)) return true;
    return false;
}
static void a_removal_is_reported_only_once_the_rebuild_succeeded(void)
{
    backend b = {0};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    fipsd_gw_nat_command add = {FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::1")}, rm = add;
    rm.kind = FIPSD_GW_NAT_REMOVE;
    fipsd_gw_nat_applied applied;
    CHECK(fipsd_gw_nat_apply(n, &add, 1, &applied) == 0); fipsd_gw_nat_applied_clear(&applied);
    b.fail = 1;
    CHECK(fipsd_gw_nat_apply(n, &rm, 1, &applied) == 0);
    CHECKM(applied.removed.len == 0, "a removal the kernel refused is not reported");
    fipsd_gw_nat_applied_clear(&applied);
    b.fail = 0;
    fipsd_gw_pairs removed = {0}; fipsd_gw_nat_error err;
    CHECK(fipsd_gw_nat_retry_pending(n, &removed, &err) == 1);
    CHECKM(removed.len == 1 && has_pair(&removed, "fd01::1", "fd9a::1"), "the retry that applied it reports it");
    /* A second remove of the gone pair has no rules and is reported at once. */
    fipsd_gw_pairs_clear(&removed);
    CHECK(fipsd_gw_nat_apply(n, &rm, 1, &applied) == 0);
    CHECK(applied.outcomes[0].kind == FIPSD_GW_NAT_RULE_NOT_FOUND && applied.removed.len == 1);
    fipsd_gw_nat_applied_clear(&applied); free(b.last); fipsd_gw_nat_free(n);
}
static void a_stale_remove_does_not_unmap_a_reissued_address(void)
{
    backend b = {0};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    fipsd_gw_nat_command cmds[] = {{FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::2")}};
    fipsd_gw_nat_applied applied;
    CHECK(fipsd_gw_nat_apply(n, cmds, 1, &applied) == 0); fipsd_gw_nat_applied_clear(&applied);
    /* The address was node 1's; its removal arrives after the reissue. */
    fipsd_gw_nat_command stale = {FIPSD_GW_NAT_REMOVE, gw_v6("fd01::1"), gw_v6("fd9a::1")};
    CHECK(fipsd_gw_nat_apply(n, &stale, 1, &applied) == 0);
    CHECK(applied.outcomes[0].kind == FIPSD_GW_NAT_RULE_NOT_FOUND);
    CHECKM(fipsd_gw_nat_mapping_count(n) == 1, "the new holder keeps its mapping");
    fipsd_gw_retry_log log = {0};
    CHECKM(fipsd_gw_nat_report_apply(&log, &stale, 1, &applied) == FIPSD_GW_RETRY_CLEAN, "a batch of stale removes rebuilt nothing");
    fipsd_gw_nat_applied_clear(&applied); free(b.last); fipsd_gw_nat_free(n);
}
static void a_reissued_address_reports_the_old_pair_once_the_new_table_is_in(void)
{
    backend b = {0};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    fipsd_gw_nat_command first = {FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::1")}, second = {FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::2")};
    fipsd_gw_nat_applied applied;
    CHECK(fipsd_gw_nat_apply(n, &first, 1, &applied) == 0); fipsd_gw_nat_applied_clear(&applied);
    CHECK(fipsd_gw_nat_apply(n, &second, 1, &applied) == 0);
    CHECKM(has_pair(&applied.removed, "fd01::1", "fd9a::1"), "the replaced pair is gone from the accepted table");
    fipsd_gw_nat_applied_clear(&applied); free(b.last); fipsd_gw_nat_free(n);
}
static void retry_log_warns_on_a_new_failure_and_reports_recovery_once(void)
{
    fipsd_gw_retry_log log = {0};
    CHECK(fipsd_gw_retry_log_observe(&log, NULL) == FIPSD_GW_RETRY_CLEAN);
    CHECK(fipsd_gw_retry_log_observe(&log, "nftables error: a") == FIPSD_GW_RETRY_FAILED);
    CHECK(fipsd_gw_retry_log_observe(&log, "nftables error: a") == FIPSD_GW_RETRY_REPEATED);
    CHECK(fipsd_gw_retry_log_unchanged(&log) == FIPSD_GW_RETRY_REPEATED);
    CHECK(fipsd_gw_retry_log_observe(&log, "nftables error: b") == FIPSD_GW_RETRY_FAILED);
    CHECK(fipsd_gw_retry_log_observe(&log, NULL) == FIPSD_GW_RETRY_RECOVERED);
    CHECK(fipsd_gw_retry_log_observe(&log, NULL) == FIPSD_GW_RETRY_CLEAN);
    CHECK(fipsd_gw_retry_log_unchanged(&log) == FIPSD_GW_RETRY_CLEAN);
}
static void apply_failures_log_once_per_change_of_outcome(void)
{
    backend b = {.fail = 1};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    fipsd_gw_retry_log log = {0};
    fipsd_gw_retry_report reports[3];
    for (int k = 0; k < 3; ++k) {
        if (k == 2) b.fail = 0;
        char v[48]; snprintf(v, sizeof v, "fd01::%d", k + 1);
        fipsd_gw_nat_command add = {FIPSD_GW_NAT_ADD, gw_v6(v), gw_v6("fd9a::1")};
        fipsd_gw_nat_applied applied;
        CHECK(fipsd_gw_nat_apply(n, &add, 1, &applied) == 0);
        reports[k] = fipsd_gw_nat_report_apply(&log, &add, 1, &applied);
        fipsd_gw_nat_applied_clear(&applied);
    }
    CHECK(reports[0] == FIPSD_GW_RETRY_FAILED && reports[1] == FIPSD_GW_RETRY_REPEATED && reports[2] == FIPSD_GW_RETRY_RECOVERED);
    free(b.last); fipsd_gw_nat_free(n);
}
static void a_batch_of_stale_removes_during_a_failure_does_not_report_recovery(void)
{
    backend b = {.fail = 1};
    fipsd_gw_nat *n = nat_state(&b, "br-lan", "fips0", "fd01::", 112);
    fipsd_gw_retry_log log = {0};
    fipsd_gw_nat_command add = {FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::1")};
    fipsd_gw_nat_applied applied;
    CHECK(fipsd_gw_nat_apply(n, &add, 1, &applied) == 0);
    CHECK(fipsd_gw_nat_report_apply(&log, &add, 1, &applied) == FIPSD_GW_RETRY_FAILED);
    fipsd_gw_nat_applied_clear(&applied);
    fipsd_gw_nat_command stale = {FIPSD_GW_NAT_REMOVE, gw_v6("fd01::9"), gw_v6("fd9a::9")};
    CHECK(fipsd_gw_nat_apply(n, &stale, 1, &applied) == 0);
    CHECKM(fipsd_gw_nat_report_apply(&log, &stale, 1, &applied) == FIPSD_GW_RETRY_REPEATED, "nothing was rebuilt, so the failure stands");
    fipsd_gw_nat_applied_clear(&applied); free(b.last); fipsd_gw_nat_free(n);
}
static void the_target_parser_takes_only_bracketed_ipv6_socket_addresses(void)
{
    struct in6_addr a; uint16_t p;
    CHECK(fipsd_gw_parse_target("[fd02::5]:80", &a, &p) && p == 80);
    CHECK(!fipsd_gw_parse_target("192.0.2.1:80", &a, &p));
    CHECK(!fipsd_gw_parse_target("[fd02::5]", &a, &p));
    CHECK(!fipsd_gw_parse_target("[fd02::5]:70000", &a, &p));
    CHECK(!fipsd_gw_parse_target("fd02::5:80", &a, &p));
}
static void nat_error_text_matches_upstream(void)
{
    char t[256];
    fipsd_gw_nat_error e = {.kind = FIPSD_GW_NAT_RULE_NOT_FOUND}; e.addr = gw_v6("fd01::1");
    CHECK(!strcmp(fipsd_gw_nat_error_text(&e, t, sizeof t), "rule not found for virtual IP fd01::1"));
    e.kind = FIPSD_GW_NAT_WORKER_STOPPED;
    CHECK(!strcmp(fipsd_gw_nat_error_text(&e, t, sizeof t), "the NAT worker has stopped"));
    e.kind = FIPSD_GW_NAT_NFTABLES; snprintf(e.message, sizeof e.message, "x");
    CHECK(!strcmp(fipsd_gw_nat_error_text(&e, t, sizeof t), "nftables error: x"));
}

/* ---- the worker over a fake table ------------------------------------------ */
typedef struct fake_table {
    nsync_mu mu; nsync_cv cv;
    fipsd_gw_nat_command log[64]; size_t nlog;   /* every command applied */
    size_t batches;                              /* apply calls */
    bool hold_apply, hold_cleanup, in_apply, in_cleanup;
    int cleanups;
    size_t count;
    bool destroyed;
} fake_table;
static void fake_wait_released(fake_table *f, bool *hold, bool *in)
{
    nsync_mu_lock(&f->mu);
    *in = true; nsync_cv_broadcast(&f->cv);
    while (*hold) nsync_cv_wait(&f->cv, &f->mu);
    *in = false;
    nsync_mu_unlock(&f->mu);
}
static int fake_apply(void *self, const fipsd_gw_nat_command *c, size_t n, fipsd_gw_nat_applied *out)
{
    fake_table *f = self;
    fake_wait_released(f, &f->hold_apply, &f->in_apply);
    memset(out, 0, sizeof *out);
    out->commands = calloc(n, sizeof *out->commands); out->outcomes = calloc(n, sizeof *out->outcomes); out->len = n;
    nsync_mu_lock(&f->mu);
    f->batches++;
    for (size_t i = 0; i < n; ++i) {
        out->commands[i] = c[i];
        if (f->nlog < 64) f->log[f->nlog++] = c[i];
        if (c[i].kind == FIPSD_GW_NAT_ADD) f->count++;
        else { if (f->count) f->count--; (void)fipsd_gw_pairs_push(&out->removed, &c[i].virtual_ip, &c[i].mesh_addr); }
    }
    nsync_cv_broadcast(&f->cv);
    nsync_mu_unlock(&f->mu);
    return 0;
}
static int fake_retry(void *self, fipsd_gw_pairs *removed, fipsd_gw_nat_error *err) { (void)self; (void)removed; (void)err; return 0; }
static size_t fake_count(void *self) { fake_table *f = self; nsync_mu_lock(&f->mu); size_t c = f->count; nsync_mu_unlock(&f->mu); return c; }
static int fake_cleanup(void *self, fipsd_gw_nat_error *err)
{
    (void)err;
    fake_table *f = self;
    fake_wait_released(f, &f->hold_cleanup, &f->in_cleanup);
    nsync_mu_lock(&f->mu); f->cleanups++; nsync_mu_unlock(&f->mu);
    return 0;
}
static void fake_destroy(void *self) { fake_table *f = self; nsync_mu_lock(&f->mu); f->destroyed = true; nsync_cv_broadcast(&f->cv); nsync_mu_unlock(&f->mu); }
static fipsd_gw_nat_table fake_of(fake_table *f) { return (fipsd_gw_nat_table){fake_apply, fake_retry, fake_count, fake_cleanup, fake_destroy, f}; }
static void fake_init(fake_table *f) { memset(f, 0, sizeof *f); nsync_mu_init(&f->mu); nsync_cv_init(&f->cv); }
/* Wait (on the table's own signal) until pred holds. */
static void fake_wait(fake_table *f, bool (*pred)(fake_table *))
{
    nsync_mu_lock(&f->mu);
    while (!pred(f)) nsync_cv_wait(&f->cv, &f->mu);
    nsync_mu_unlock(&f->mu);
}
static size_t wanted;
static bool logged_enough(fake_table *f) { return f->nlog >= wanted; }
static bool applying(fake_table *f) { return f->in_apply; }
static bool gone(fake_table *f) { return f->destroyed; }
typedef struct exit_flag { nsync_mu mu; nsync_cv cv; int calls; } exit_flag;
static void on_exit_cb(void *user) { exit_flag *e = user; nsync_mu_lock(&e->mu); e->calls++; nsync_cv_broadcast(&e->cv); nsync_mu_unlock(&e->mu); }

static void nat_worker_applies_add_remove_and_readd_in_order(void)
{
    fake_table f; fake_init(&f);
    exit_flag ex = {0}; nsync_mu_init(&ex.mu); nsync_cv_init(&ex.cv);
    fipsd_gw_nat_driver *d = fipsd_gw_nat_driver_start(fake_of(&f), on_exit_cb, &ex);
    CHECK(d);
    fipsd_gw_nat_command add = {FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::1")}, rm = add;
    rm.kind = FIPSD_GW_NAT_REMOVE;
    CHECK(fipsd_gw_nat_driver_submit(d, &add) == 0 && fipsd_gw_nat_driver_submit(d, &rm) == 0 && fipsd_gw_nat_driver_submit(d, &add) == 0);
    wanted = 3; fake_wait(&f, logged_enough);
    CHECK(f.log[0].kind == FIPSD_GW_NAT_ADD && f.log[1].kind == FIPSD_GW_NAT_REMOVE && f.log[2].kind == FIPSD_GW_NAT_ADD);
    fipsd_gw_pairs removed = {0};
    fipsd_gw_nat_driver_take_removed(d, &removed);
    CHECKM(removed.len == 1, "the removal the table applied is reported");
    fipsd_gw_pairs_clear(&removed);
    fipsd_gw_nat_error err;
    CHECK(fipsd_gw_nat_driver_shutdown(d, 5000, &err) == 0);
    CHECKM(f.cleanups == 1 && f.destroyed && ex.calls == 0, "a requested stop is not reported as an exit");
}
static void queued_changes_are_applied_in_one_rebuild(void)
{
    fake_table f; fake_init(&f);
    f.hold_apply = true;
    fipsd_gw_nat_driver *d = fipsd_gw_nat_driver_start(fake_of(&f), NULL, NULL);
    fipsd_gw_nat_command add = {FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::1")};
    CHECK(fipsd_gw_nat_driver_submit(d, &add) == 0);
    fake_wait(&f, applying);
    /* The worker is inside the first apply; these queue behind it. */
    for (int k = 2; k <= 5; ++k) { char v[32]; snprintf(v, sizeof v, "fd01::%d", k); add.virtual_ip = gw_v6(v); CHECK(fipsd_gw_nat_driver_submit(d, &add) == 0); }
    nsync_mu_lock(&f.mu); f.hold_apply = false; nsync_cv_broadcast(&f.cv); nsync_mu_unlock(&f.mu);
    wanted = 5; fake_wait(&f, logged_enough);
    CHECKM(f.batches == 2, "the four queued changes went in one rebuild (%zu batches)", f.batches);
    CHECK(fipsd_gw_nat_driver_mapping_count(d) == 5);
    fipsd_gw_nat_error err;
    CHECK(fipsd_gw_nat_driver_shutdown(d, 5000, &err) == 0 && f.cleanups == 1);
}
static void a_stop_queued_behind_changes_still_runs_cleanup(void)
{
    fake_table f; fake_init(&f);
    f.hold_apply = true;
    fipsd_gw_nat_driver *d = fipsd_gw_nat_driver_start(fake_of(&f), NULL, NULL);
    fipsd_gw_nat_command add = {FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::1")};
    CHECK(fipsd_gw_nat_driver_submit(d, &add) == 0);
    fake_wait(&f, applying);
    add.virtual_ip = gw_v6("fd01::2");
    CHECK(fipsd_gw_nat_driver_submit(d, &add) == 0);
    /* Release from a helper once shutdown has queued its stop. */
    nsync_mu_lock(&f.mu); f.hold_apply = false; nsync_cv_broadcast(&f.cv); nsync_mu_unlock(&f.mu);
    fipsd_gw_nat_error err;
    CHECK(fipsd_gw_nat_driver_shutdown(d, 5000, &err) == 0);
    CHECK(f.cleanups == 1 && f.nlog == 2);
}
static void stop_returns_after_the_timeout_when_the_worker_hangs(void)
{
    static fake_table f; fake_init(&f);
    f.hold_cleanup = true;
    fipsd_gw_nat_driver *d = fipsd_gw_nat_driver_start(fake_of(&f), NULL, NULL);
    fipsd_gw_nat_error err;
    fipsd_gw_instant started = fipsd_gw_now();
    CHECKM(fipsd_gw_nat_driver_shutdown(d, 200, &err) == 1, "a timeout is not an error, but reports the worker left behind");
    fipsd_gw_instant took = fipsd_gw_since(fipsd_gw_now(), started);
    CHECKM(took >= MILLIS(200) && took < SECS(5), "shutdown waited %llu ms", (unsigned long long)(took / 1000000u));
    /* Let the abandoned worker finish, and wait for it to let go of the table. */
    nsync_mu_lock(&f.mu); f.hold_cleanup = false; nsync_cv_broadcast(&f.cv); nsync_mu_unlock(&f.mu);
    fake_wait(&f, gone);
    CHECK(f.cleanups == 1);
}
static void a_dropped_driver_ends_its_worker_without_cleanup(void)
{
    fake_table f; fake_init(&f);
    exit_flag ex = {0}; nsync_mu_init(&ex.mu); nsync_cv_init(&ex.cv);
    fipsd_gw_nat_driver *d = fipsd_gw_nat_driver_start(fake_of(&f), on_exit_cb, &ex);
    fipsd_gw_nat_driver_drop(d);
    CHECK(f.cleanups == 0 && f.destroyed && ex.calls == 0);
}
/* A table whose apply runs out of memory, as the real one reports it. */
static int failing_apply(void *self, const fipsd_gw_nat_command *c, size_t n, fipsd_gw_nat_applied *out)
{
    (void)c; (void)n;
    memset(out, 0, sizeof *out);
    fake_table *f = self;
    nsync_mu_lock(&f->mu); f->batches++; nsync_mu_unlock(&f->mu);
    return -1;
}
static void a_worker_out_of_memory_stops_and_reports_its_exit(void)
{
    fake_table f; fake_init(&f);
    exit_flag ex = {0}; nsync_mu_init(&ex.mu); nsync_cv_init(&ex.cv);
    fipsd_gw_nat_table table = fake_of(&f);
    table.apply = failing_apply;
    fipsd_gw_nat_driver *d = fipsd_gw_nat_driver_start(table, on_exit_cb, &ex);
    fipsd_gw_nat_command add = {FIPSD_GW_NAT_ADD, gw_v6("fd01::1"), gw_v6("fd9a::1")};
    CHECK(fipsd_gw_nat_driver_submit(d, &add) == 0);
    nsync_mu_lock(&ex.mu);
    while (!ex.calls) nsync_cv_wait(&ex.cv, &ex.mu);
    nsync_mu_unlock(&ex.mu);
    CHECKM(fipsd_gw_nat_driver_submit(d, &add) != 0, "a stopped worker takes no more changes");
    fipsd_gw_nat_error err;
    CHECK(fipsd_gw_nat_driver_shutdown(d, 5000, &err) == -1 && err.kind == FIPSD_GW_NAT_WORKER_STOPPED);
    /* The worker is joined: every count below is final. */
    CHECKM(ex.calls == 1, "the unasked exit is reported once");
    CHECKM(f.cleanups == 0 && f.destroyed, "the table is left for the restart to replace");
}
int gw_suite_nat(void)
{
    RUN(rebuild_matches_the_golden_batch_with_mappings_and_forwards);
    RUN(an_empty_table_matches_its_golden_and_sends_no_element_list);
    RUN(the_cleanup_batch_matches_its_golden_and_is_idempotent);
    RUN(rebuild_deletes_and_recreates_the_table_inside_one_batch);
    RUN(rebuild_adds_the_chains_with_their_hooks);
    RUN(rebuild_emits_a_dnat_output_dnat_and_snat_for_every_mapping);
    RUN(the_masquerades_precede_every_snat_and_the_lan_masquerade_comes_once);
    RUN(the_mesh_source_set_holds_every_mapped_mesh_address_once);
    RUN(the_filter_rules_keep_their_shape);
    RUN(the_pool_drop_matches_a_pool_configured_with_host_bits_set);
    RUN(every_tun_match_uses_the_configured_tun_name);
    RUN(a_failed_rebuild_stays_pending_and_retries_the_latest_desired_state);
    RUN(a_failed_port_forward_rebuild_remains_pending);
    RUN(a_removal_is_reported_only_once_the_rebuild_succeeded);
    RUN(a_stale_remove_does_not_unmap_a_reissued_address);
    RUN(a_reissued_address_reports_the_old_pair_once_the_new_table_is_in);
    RUN(retry_log_warns_on_a_new_failure_and_reports_recovery_once);
    RUN(apply_failures_log_once_per_change_of_outcome);
    RUN(a_batch_of_stale_removes_during_a_failure_does_not_report_recovery);
    RUN(the_target_parser_takes_only_bracketed_ipv6_socket_addresses);
    RUN(nat_error_text_matches_upstream);
    RUN(nat_worker_applies_add_remove_and_readd_in_order);
    RUN(queued_changes_are_applied_in_one_rebuild);
    RUN(a_stop_queued_behind_changes_still_runs_cleanup);
    RUN(stop_returns_after_the_timeout_when_the_worker_hangs);
    RUN(a_dropped_driver_ends_its_worker_without_cleanup);
    RUN(a_worker_out_of_memory_stops_and_reports_its_exit);
    return 0;
}
