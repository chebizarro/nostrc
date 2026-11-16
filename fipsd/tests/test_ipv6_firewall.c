/* Listening-socket enumeration and the inet fips classifier (nostrc-9nxwk.2),
 * ported from upstream src/ipv6tun/{listening,firewall_state}.rs tests and
 * tests/firewall_state.rs: /proc row decoding, reachability filtering, every
 * classification and scope case against captured nft JSON, and the panel
 * rendering. No nft process is run; the live query is exercised only for its
 * no-table fallback. */
#include "ipv6/listening.h"
#include "ipv6/firewall_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void listening_tests(void)
{
    assert(!strcmp(fipsd_ipv6_proto_str(FIPSD_IPV6_PROTO_TCP), "tcp") && !strcmp(fipsd_ipv6_proto_str(FIPSD_IPV6_PROTO_UDP), "udp"));
    /* /proc/net/tcp6 rows: the header is refused; a LISTEN row on [::]:22 and a
     * row bound to fd97::1 (kernel word order) decode to their addresses. */
    uint8_t local[16], remote[16]; uint16_t port, rport; unsigned state; uint64_t inode;
    assert(!fipsd_ipv6_listening_parse_row("  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode", local, &port, remote, &rport, &state, &inode));
    assert(fipsd_ipv6_listening_parse_row("   0: 00000000000000000000000000000000:0016 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 12345 1 0000000000000000 100 0 0 10 0", local, &port, remote, &rport, &state, &inode));
    { uint8_t zero[16] = {0}; assert(!memcmp(local, zero, 16) && port == 22 && state == FIPSD_IPV6_TCP_LISTEN && inode == 12345 && rport == 0); }
    /* fd97::1 is printed as four host-order words: 000097FD 00000000 00000000 01000000 on a little-endian host */
    { uint32_t w0, w3; uint8_t a[16] = {0xfd, 0x97, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
      memcpy(&w0, a, 4); memcpy(&w3, a + 12, 4); char row[256];
      snprintf(row, sizeof row, "   1: %08X0000000000000000%08X:1F90 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 777 1 0000000000000000 100 0 0 10 0", w0, w3);
      assert(fipsd_ipv6_listening_parse_row(row, local, &port, remote, &rport, &state, &inode) && !memcmp(local, a, 16) && port == 8080 && inode == 777); }
    assert(!fipsd_ipv6_listening_parse_row("   2: 0000:0016 0000:0000 0A x", local, &port, remote, &rport, &state, &inode));
    /* build_entry_filters_non_fips0_binds */
    uint8_t fips0[16] = {0xfd, 0x97, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, zero[16] = {0}, lo[16] = {0}, other[16] = {0x20, 0x01, 0x0d, 0xb8};
    lo[15] = 1; other[15] = 1;
    fipsd_ipv6_listening_socket s;
    assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, zero, 22, 0, NULL, fips0, &s) && s.wildcard_bind && !s.has_pid);
    assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, fips0, 22, 0, NULL, fips0, &s) && !s.wildcard_bind);
    assert(!fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, lo, 22, 0, NULL, fips0, &s));
    assert(!fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, other, 22, 0, NULL, fips0, &s));
    assert(!fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, false, zero, 22, 0, NULL, fips0, &s)); /* IPv4 */
    /* the owning process is paired by inode */
    fipsd_ipv6_inode_owner owners[] = {{12345, 4242, "sshd"}, {777, 99, "nginx"}};
    fipsd_ipv6_inode_map map = {owners, 2, 2};
    assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, zero, 22, 12345, &map, fips0, &s) && s.has_pid && s.pid == 4242 && !strcmp(s.process, "sshd"));
    assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_UDP, true, zero, 5353, 1, &map, fips0, &s) && !s.has_pid);
    /* stable order: proto, port, pid (unknown first) */
    fipsd_ipv6_listening_socket rows[4];
    assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_UDP, true, zero, 53, 1, &map, fips0, &rows[0]));
    assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, zero, 80, 777, &map, fips0, &rows[1]));
    assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, zero, 22, 12345, &map, fips0, &rows[2]));
    assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, fips0, 22, 1, &map, fips0, &rows[3]));
    fipsd_ipv6_listening_sort(rows, 4);
    assert(rows[0].port == 22 && !rows[0].has_pid && rows[1].port == 22 && rows[1].pid == 4242 && rows[2].port == 80 && rows[3].proto == FIPSD_IPV6_PROTO_UDP);
    /* enumerate_runs_without_panicking: a valid (possibly empty) list on Linux, none elsewhere */
    fipsd_ipv6_listening_socket live[64];
    size_t n = fipsd_ipv6_listening_enumerate(fips0, live, 64);
#ifdef __linux__
    for (size_t i = 0; i < n; ++i) assert(live[i].wildcard_bind || !memcmp(live[i].local_addr, fips0, 16));
#else
    assert(n == 0);
#endif
    /* render: rows classified against a table that polices the TUN, unprotected otherwise */
    json_error_t err;
    json_t *nft = json_loads("{\"nftables\":["
        "{\"rule\":{\"family\":\"inet\",\"table\":\"fips\",\"chain\":\"inbound\",\"expr\":[{\"match\":{\"op\":\"!=\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":\"fips0\"}},{\"return\":null}]}},"
        "{\"rule\":{\"family\":\"inet\",\"table\":\"fips\",\"chain\":\"inbound\",\"expr\":[{\"match\":{\"op\":\"==\",\"left\":{\"payload\":{\"protocol\":\"tcp\",\"field\":\"dport\"}},\"right\":22}},{\"accept\":null}]}},"
        "{\"rule\":{\"family\":\"inet\",\"table\":\"fips\",\"chain\":\"inbound\",\"expr\":[{\"counter\":{\"packets\":0,\"bytes\":0}},{\"drop\":null}]}}]}", 0, &err);
    assert(nft);
    fipsd_ipv6_filter_classifier *c = fipsd_ipv6_filter_from_nft_json(nft); assert(c);
    json_t *reply = fipsd_ipv6_listening_render(c, "fips0", fips0, rows, 4); assert(reply);
    assert(json_is_true(json_object_get(reply, "firewall_active")));
    assert(!strcmp(json_string_value(json_object_get(reply, "fips0_addr")), "fd97::1"));
    json_t *sockets = json_object_get(reply, "sockets"); assert(json_array_size(sockets) == 4);
    assert(!strcmp(json_string_value(json_object_get(json_array_get(sockets, 0), "filter")), "accept"));   /* tcp 22, listening, accepted */
    assert(json_is_null(json_object_get(json_array_get(sockets, 0), "pid")));
    assert(json_integer_value(json_object_get(json_array_get(sockets, 1), "pid")) == 4242);
    assert(!strcmp(json_string_value(json_object_get(json_array_get(sockets, 1), "process")), "sshd"));
    assert(!strcmp(json_string_value(json_object_get(json_array_get(sockets, 2), "filter")), "drop"));     /* tcp 80, listening, not opened */
    assert(!strcmp(json_string_value(json_object_get(json_array_get(sockets, 3), "filter")), "drop"));     /* udp 53 */
    assert(!strcmp(json_string_value(json_object_get(json_array_get(sockets, 3), "local_addr")), "::"));
    assert(json_is_true(json_object_get(json_array_get(sockets, 3), "wildcard_bind")));
    json_decref(reply);
    reply = fipsd_ipv6_listening_render(c, "mesh0", fips0, rows, 4); assert(reply); /* scoped to fips0: mesh0 is unprotected */
    assert(json_is_false(json_object_get(reply, "firewall_active")));
    assert(!strcmp(json_string_value(json_object_get(json_array_get(json_object_get(reply, "sockets"), 0), "filter")), "no_firewall"));
    json_decref(reply); fipsd_ipv6_filter_destroy(c); json_decref(nft);
}

/* ===== firewall_state.rs ===== */
/* A classifier over the inbound chain rules given as a JSON array of expr arrays. */
static fipsd_ipv6_filter_classifier *make_classifier(const char *rules_json)
{
    json_error_t err; json_t *rules = json_loads(rules_json, 0, &err);
    if (!rules) { fprintf(stderr, "json: %s at %d: %s\n", err.text, err.line, rules_json); abort(); }
    json_t *list = json_array(); size_t i; json_t *expr;
    json_array_foreach(rules, i, expr)
        json_array_append_new(list, json_pack("{s:{s:s,s:s,s:s,s:O}}", "rule", "family", "inet", "table", "fips", "chain", "inbound", "expr", expr));
    json_t *nft = json_pack("{s:o}", "nftables", list);
    fipsd_ipv6_filter_classifier *c = fipsd_ipv6_filter_from_nft_json(nft); assert(c);
    json_decref(nft); json_decref(rules);
    return c;
}
#define TCP FIPSD_IPV6_PROTO_TCP
#define UDP FIPSD_IPV6_PROTO_UDP
#define ACCEPT FIPSD_IPV6_FILTER_ACCEPT
#define DROP FIPSD_IPV6_FILTER_DROP
#define UNKNOWN FIPSD_IPV6_FILTER_UNKNOWN
#define DPORT(proto, right) "{\"match\":{\"op\":\"==\",\"left\":{\"payload\":{\"protocol\":\"" proto "\",\"field\":\"dport\"}},\"right\":" right "}}"
#define SCOPE_RULE(rhs) "[{\"match\":{\"op\":\"!=\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":" rhs "}},{\"return\":null}]"
#define CT_RULE "[{\"match\":{\"op\":\"in\",\"left\":{\"ct\":{\"key\":\"state\"}},\"right\":[\"established\",\"related\"]}},{\"accept\":null}]"
#define ICMP_RULE "[{\"match\":{\"op\":\"==\",\"left\":{\"payload\":{\"protocol\":\"icmpv6\",\"field\":\"type\"}},\"right\":\"echo-request\"}},{\"accept\":null}]"
#define DROP_RULE "[{\"counter\":{\"packets\":0,\"bytes\":0}},{\"drop\":null}]"
static fipsd_ipv6_filter_classifier *baseline_chain(const char *scope_rhs)
{
    char text[1024]; snprintf(text, sizeof text, "[" SCOPE_RULE("%s") "," CT_RULE "," ICMP_RULE "," DROP_RULE "]", scope_rhs);
    return make_classifier(text);
}
static fipsd_ipv6_filter_classifier *scoped_by(const char *scope_rule)
{
    char text[1024]; snprintf(text, sizeof text, "[%s," DROP_RULE "]", scope_rule);
    return make_classifier(text);
}
static void expect_scope(fipsd_ipv6_filter_classifier *c, fipsd_ipv6_scope_kind kind, const char *name)
{
    fipsd_ipv6_scope s = fipsd_ipv6_filter_scope(c);
    assert(s.kind == kind && (!name || !strcmp(s.name, name)));
}
static void firewall_tests(void)
{
    fipsd_ipv6_filter_classifier *c = fipsd_ipv6_filter_no_firewall();            /* no_firewall_means_no_firewall */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == FIPSD_IPV6_FILTER_NO_FIREWALL && fipsd_ipv6_filter_classify(c, UDP, 5353) == FIPSD_IPV6_FILTER_NO_FIREWALL);
    assert(!strcmp(fipsd_ipv6_filter_state_str(FIPSD_IPV6_FILTER_NO_FIREWALL), "no_firewall") && !strcmp(fipsd_ipv6_filter_state_str(UNKNOWN), "unknown"));
    expect_scope(c, FIPSD_IPV6_SCOPE_NONE, NULL); assert(!fipsd_ipv6_filter_polices(c, "fips0") && !fipsd_ipv6_filter_foreign_scope(c, "fips0", NULL, 0)); /* no_table_polices_nothing */
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[]");                                                     /* empty_chain_drops_everything */
    assert(fipsd_ipv6_filter_is_active(c) && fipsd_ipv6_filter_classify(c, TCP, 22) == DROP && fipsd_ipv6_filter_classify(c, UDP, 5353) == DROP);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[" DPORT("tcp", "22") ",{\"accept\":null}]]");           /* canonical_tcp_dport_accept */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == ACCEPT && fipsd_ipv6_filter_classify(c, TCP, 80) == DROP && fipsd_ipv6_filter_classify(c, UDP, 22) == DROP);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[" DPORT("udp", "5353") ",{\"accept\":null}]]");         /* canonical_udp_dport_accept */
    assert(fipsd_ipv6_filter_classify(c, UDP, 5353) == ACCEPT && fipsd_ipv6_filter_classify(c, TCP, 5353) == DROP);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[" DPORT("tcp", "{\"set\":[22,80,443]}") ",{\"accept\":null}]]"); /* dport_set_accept */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == ACCEPT && fipsd_ipv6_filter_classify(c, TCP, 80) == ACCEPT &&
           fipsd_ipv6_filter_classify(c, TCP, 443) == ACCEPT && fipsd_ipv6_filter_classify(c, TCP, 25) == DROP);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[" DPORT("tcp", "{\"range\":[22,25]}") ",{\"accept\":null}]]"); /* dport_range_accept */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == ACCEPT && fipsd_ipv6_filter_classify(c, TCP, 25) == ACCEPT && fipsd_ipv6_filter_classify(c, TCP, 26) == DROP);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[{\"match\":{\"op\":\"==\",\"left\":{\"payload\":{\"protocol\":\"ip6\",\"field\":\"saddr\"}},\"right\":{\"prefix\":{\"addr\":\"fd97::\",\"len\":64}}}},"
                        DPORT("tcp", "22") ",{\"accept\":null}]]");                /* saddr_restricted_is_unknown */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == UNKNOWN && fipsd_ipv6_filter_classify(c, TCP, 80) == DROP);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[" DPORT("tcp", "22") ",{\"jump\":{\"target\":\"some_chain\"}}]]"); /* jump_verdict_is_unknown */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == UNKNOWN);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[" DPORT("tcp", "22") ",{\"drop\":null}]]");             /* explicit_drop_classifies_as_drop */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == DROP);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[" SCOPE_RULE("\"fips0\"") "," CT_RULE "," ICMP_RULE "]"); /* unrelated_rules_dont_affect_port */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == DROP && fipsd_ipv6_filter_classify(c, UDP, 5353) == DROP);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[{\"match\":{\"op\":\"==\",\"left\":{\"meta\":{\"key\":\"l4proto\"}},\"right\":\"tcp\"}}," DPORT("tcp", "22") ",{\"accept\":null}]]"); /* l4proto_then_dport_accept */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == ACCEPT && fipsd_ipv6_filter_classify(c, UDP, 22) == DROP);
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[" DPORT("tcp", "22") ",{\"accept\":null}],"             /* first_accept_match_wins */
                        "[{\"match\":{\"op\":\"==\",\"left\":{\"payload\":{\"protocol\":\"ip6\",\"field\":\"saddr\"}},\"right\":\"fd00::1\"}}," DPORT("tcp", "22") ",{\"drop\":null}]]");
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == ACCEPT);
    fipsd_ipv6_filter_destroy(c);
    char foreign[64];
    c = baseline_chain("\"fips0\"");                                               /* inet_fips_scoped_to_fips0_does_not_police_a_tun_named_mesh0 */
    assert(fipsd_ipv6_filter_is_active(c)); expect_scope(c, FIPSD_IPV6_SCOPE_IFACE, "fips0");
    assert(!fipsd_ipv6_filter_polices(c, "mesh0") && fipsd_ipv6_filter_foreign_scope(c, "mesh0", foreign, sizeof foreign) && !strcmp(foreign, "fips0"));
    assert(fipsd_ipv6_filter_polices(c, "fips0") && !fipsd_ipv6_filter_foreign_scope(c, "fips0", NULL, 0)); /* ..._polices_a_tun_named_fips0 */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == DROP && fipsd_ipv6_filter_classify(c, UDP, 5353) == DROP);
    fipsd_ipv6_filter_warn_scope(c, "mesh0"); fipsd_ipv6_filter_warn_scope(c, "fips0"); /* tests/firewall_state.rs: warns only for a foreign scope (text asserted above) */
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[" CT_RULE "," DROP_RULE "]");                            /* chain_with_no_iifname_return_polices_every_tun */
    expect_scope(c, FIPSD_IPV6_SCOPE_UNSCOPED, NULL);
    assert(fipsd_ipv6_filter_polices(c, "fips0") && fipsd_ipv6_filter_polices(c, "mesh0") && !fipsd_ipv6_filter_foreign_scope(c, "mesh0", NULL, 0));
    fipsd_ipv6_filter_destroy(c);
    c = baseline_chain("{\"set\":[\"mesh0\",\"fips0\"]}");                         /* scoping_rule_with_a_set_is_reported_as_not_policing */
    expect_scope(c, FIPSD_IPV6_SCOPE_UNREADABLE, NULL);
    assert(!fipsd_ipv6_filter_polices(c, "fips0") && !fipsd_ipv6_filter_polices(c, "mesh0") && !fipsd_ipv6_filter_foreign_scope(c, "mesh0", NULL, 0));
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[{\"match\":{\"op\":\"==\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":\"fips0\"}},{\"return\":null}]]"); /* ..._matches_the_name_rather_than_excluding_it */
    expect_scope(c, FIPSD_IPV6_SCOPE_UNREADABLE, NULL); assert(!fipsd_ipv6_filter_polices(c, "fips0"));
    fipsd_ipv6_filter_destroy(c);
    c = scoped_by("[{\"vmap\":{\"key\":{\"meta\":{\"key\":\"iifname\"}},\"data\":{\"set\":[[\"fips0\",{\"continue\":null}],[\"*\",{\"return\":null}]]}}}]"); /* vmap */
    expect_scope(c, FIPSD_IPV6_SCOPE_UNREADABLE, NULL);
    assert(!fipsd_ipv6_filter_polices(c, "mesh0") && !fipsd_ipv6_filter_polices(c, "fips0") && !fipsd_ipv6_filter_foreign_scope(c, "mesh0", NULL, 0));
    fipsd_ipv6_filter_destroy(c);
    c = scoped_by("[{\"match\":{\"op\":\"!=\",\"left\":{\"meta\":{\"key\":\"iif\"}},\"right\":\"fips0\"}},{\"return\":null}]"); /* meta iif */
    expect_scope(c, FIPSD_IPV6_SCOPE_UNREADABLE, NULL); assert(!fipsd_ipv6_filter_polices(c, "mesh0") && !fipsd_ipv6_filter_foreign_scope(c, "mesh0", NULL, 0));
    fipsd_ipv6_filter_destroy(c);
    c = scoped_by("[{\"match\":{\"op\":\"!=\",\"left\":{\"meta\":{\"key\":\"iifgroup\"}},\"right\":7}},{\"return\":null}]"); /* iifgroup */
    expect_scope(c, FIPSD_IPV6_SCOPE_UNREADABLE, NULL); assert(!fipsd_ipv6_filter_polices(c, "mesh0"));
    fipsd_ipv6_filter_destroy(c);
    c = scoped_by("[{\"match\":{\"op\":\"==\",\"left\":{\"payload\":{\"protocol\":\"ip6\",\"field\":\"saddr\"}},\"right\":\"fd00::1\"}},{\"return\":null}]"); /* return on a non-interface match */
    expect_scope(c, FIPSD_IPV6_SCOPE_UNREADABLE, NULL); assert(!fipsd_ipv6_filter_polices(c, "fips0"));
    fipsd_ipv6_filter_destroy(c);
    c = baseline_chain("\"fips*\"");                                               /* wildcard_scope_is_unreadable_and_does_not_name_a_foreign_interface */
    expect_scope(c, FIPSD_IPV6_SCOPE_UNREADABLE, NULL); assert(!fipsd_ipv6_filter_foreign_scope(c, "fips0", NULL, 0) && !fipsd_ipv6_filter_polices(c, "fips0"));
    fipsd_ipv6_filter_destroy(c);
    c = baseline_chain("\"@meshifs\"");                                            /* named_set_scope_is_unreadable_not_an_interface_called_at_set */
    expect_scope(c, FIPSD_IPV6_SCOPE_UNREADABLE, NULL); assert(!fipsd_ipv6_filter_foreign_scope(c, "fips0", NULL, 0) && !fipsd_ipv6_filter_polices(c, "fips0"));
    fipsd_ipv6_filter_destroy(c);
    c = scoped_by("[{\"match\":{\"op\":\"!=\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":\"fips0\"}},{\"counter\":{\"packets\":0,\"bytes\":0}},{\"return\":null}]"); /* counter keeps the shape */
    expect_scope(c, FIPSD_IPV6_SCOPE_IFACE, "fips0"); assert(fipsd_ipv6_filter_polices(c, "fips0") && !fipsd_ipv6_filter_polices(c, "mesh0"));
    fipsd_ipv6_filter_destroy(c);
    c = make_classifier("[[{\"match\":{\"op\":\"==\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":{\"set\":[\"fips0\",\"mesh0\"]}}}," DPORT("tcp", "22") ",{\"accept\":null}]]"); /* allow rule with an iifname set */
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == UNKNOWN);
    fipsd_ipv6_filter_destroy(c);
    /* tests/firewall_state.rs: a full nft listing (table, chain, rule objects) parses to the fips0 scope */
    json_t *full = json_loads("{\"nftables\":[{\"table\":{\"family\":\"inet\",\"name\":\"fips\",\"handle\":1}},"
        "{\"chain\":{\"family\":\"inet\",\"table\":\"fips\",\"name\":\"inbound\",\"handle\":1,\"type\":\"filter\",\"hook\":\"input\",\"prio\":0,\"policy\":\"accept\"}},"
        "{\"rule\":{\"family\":\"inet\",\"table\":\"fips\",\"chain\":\"inbound\",\"handle\":2,\"expr\":[{\"match\":{\"op\":\"!=\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":\"fips0\"}},{\"return\":null}]}}]}", 0, NULL);
    assert(full); c = fipsd_ipv6_filter_from_nft_json(full); json_decref(full);
    expect_scope(c, FIPSD_IPV6_SCOPE_IFACE, "fips0"); assert(fipsd_ipv6_filter_foreign_scope(c, "mesh0", foreign, sizeof foreign) && !strcmp(foreign, "fips0"));
    fipsd_ipv6_filter_destroy(c);
    /* rules of another table or chain are ignored */
    json_t *other = json_loads("{\"nftables\":[{\"rule\":{\"family\":\"inet\",\"table\":\"filter\",\"chain\":\"inbound\",\"expr\":[" DPORT("tcp", "22") ",{\"accept\":null}]}},"
        "{\"rule\":{\"family\":\"inet\",\"table\":\"fips\",\"chain\":\"forward\",\"expr\":[" DPORT("tcp", "22") ",{\"accept\":null}]}}]}", 0, NULL);
    assert(other); c = fipsd_ipv6_filter_from_nft_json(other); json_decref(other);
    assert(fipsd_ipv6_filter_classify(c, TCP, 22) == DROP);
    fipsd_ipv6_filter_destroy(c);
    /* the live query without a loaded table (or without nft, or off Linux) is the no-firewall classifier */
    c = fipsd_ipv6_filter_query(); assert(c);
#ifndef __linux__
    assert(!fipsd_ipv6_filter_is_active(c));
#endif
    fipsd_ipv6_filter_destroy(c);
}
int main(void)
{
    listening_tests(); firewall_tests();
    puts("IPv6 shim firewall: /proc rows, reachability filter, nft classification/scope vectors, listening panel: ok");
    return 0;
}
