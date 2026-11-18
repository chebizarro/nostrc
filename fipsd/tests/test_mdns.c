/* mDNS LAN rendezvous, sans-IO (nostrc-9nxwk.5): wire vectors, the port of
 * upstream src/mdns/tests.rs (every test that needs no daemon), browser and
 * responder table tests on explicit time, and the LAN candidate set of
 * upstream src/node/peering/lan.rs. */
#include "mdns/advert.h"
#include "mdns/browser.h"
#include "mdns/lan.h"
#include "mdns/records.h"
#include "mdns/responder.h"
#include "host_os.h"
#include <fips/identity/encoding.h>
#include <fips/identity/local.h>
#include <fips/identity/peer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

/* ---- deterministic randomness ------------------------------------- */
static uint32_t fixed_value;
static uint32_t fixed(void *u, uint32_t bound) { (void)u; return fixed_value % bound; }
static uint64_t xs = 88172645463325252ull;
static uint32_t xorshift(void *u, uint32_t bound) { (void)u; xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return (uint32_t)(xs % bound); }
static const fipsd_mdns_rng fixed_rng = {fixed, NULL}, seeded_rng = {xorshift, NULL};

static fips_secp *secp;
static void npub_of_seed(uint8_t seed, char *out)
{
    uint8_t secret[32]; memset(secret, seed, sizeof secret);
    fips_identity *id = NULL;
    CHECK(fips_identity_from_secret_bytes(secp, fipsd_host_os_entropy(), secret, &id) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_npub(id, out, 128) == FIPS_IDENTITY_OK);
    fips_identity_destroy(id);
}
static fips_peer_identity identity_of(const char *npub)
{
    fips_peer_identity p;
    CHECK(fips_peer_identity_from_npub(secp, npub, strlen(npub), &p) == FIPS_IDENTITY_OK);
    return p;
}
static fipsd_mdns_name N(const char *text) { fipsd_mdns_name n; CHECK(fipsd_mdns_name_from_text(text, &n)); return n; }

/* ---- packets built the way a FIPS responder (or mdns-sd) sends them ---- */
typedef struct advert_spec {
    const char *service, *label, *host;
    uint16_t port;
    const fipsd_mdns_txt_entry *txt; size_t ntxt;
    const uint8_t (*v4)[4]; size_t nv4;
    const uint8_t (*v6)[16]; size_t nv6;
    uint32_t ttl_ptr, ttl_srv, ttl_txt, ttl_addr;
} advert_spec;
static size_t announce(const advert_spec *a, uint8_t *buf)
{
    static fipsd_mdns_rr rr;
    fipsd_mdns_name service = N(a->service), instance, host = N(a->host);
    CHECK(fipsd_mdns_name_prepend(a->label, &service, &instance));
    fipsd_mdns_writer w; fipsd_mdns_writer_init(&w, buf, FIPSD_MDNS_MAX_PACKET, 0, FIPSD_MDNS_FLAG_QR | FIPSD_MDNS_FLAG_AA);
    fipsd_mdns_rr_ptr(&rr, &service, &instance, a->ttl_ptr); CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    fipsd_mdns_rr_srv(&rr, &instance, a->port, &host, a->ttl_srv, true); CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    CHECK(fipsd_mdns_rr_txt(&rr, &instance, a->txt, a->ntxt, a->ttl_txt, true)); CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    for (size_t i = 0; i < a->nv4; ++i) { fipsd_mdns_rr_a(&rr, &host, a->v4[i], a->ttl_addr, true); CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ADDITIONAL, &rr, 0, false)); }
    for (size_t i = 0; i < a->nv6; ++i) { fipsd_mdns_rr_aaaa(&rr, &host, a->v6[i], a->ttl_addr, true); CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ADDITIONAL, &rr, 0, false)); }
    size_t n = fipsd_mdns_writer_finish(&w); CHECK(n);
    return n;
}
static fipsd_mdns_message *decode(const uint8_t *buf, size_t n)
{
    static fipsd_mdns_message m;
    CHECK(fipsd_mdns_decode(buf, n, &m));
    return &m;
}

/* ---- records --------------------------------------------------------- */
static void test_records(void)
{
    static fipsd_mdns_rr rr;
    uint8_t buf[FIPSD_MDNS_MAX_PACKET];
    /* An A record with the cache-flush bit, byte for byte. */
    fipsd_mdns_name h = N("h.local.");
    fipsd_mdns_writer w; fipsd_mdns_writer_init(&w, buf, sizeof buf, 0, FIPSD_MDNS_FLAG_QR | FIPSD_MDNS_FLAG_AA);
    fipsd_mdns_rr_a(&rr, &h, (const uint8_t[4]){10, 0, 0, 1}, 120, true);
    CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    static const uint8_t want_a[] = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0,
        1, 'h', 5, 'l', 'o', 'c', 'a', 'l', 0, 0, 1, 0x80, 1, 0, 0, 0, 120, 0, 4, 10, 0, 0, 1};
    CHECK(fipsd_mdns_writer_finish(&w) == sizeof want_a && !memcmp(buf, want_a, sizeof want_a));
    /* Legacy unicast: TTL capped, cache-flush cleared. */
    fipsd_mdns_writer_init(&w, buf, sizeof buf, 0x1234, FIPSD_MDNS_FLAG_QR | FIPSD_MDNS_FLAG_AA);
    CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 10, true));
    CHECK(fipsd_mdns_writer_finish(&w) == sizeof want_a);
    CHECK(buf[0] == 0x12 && buf[1] == 0x34 && buf[23] == 0 && buf[24] == 1 && buf[28] == 10);
    /* A compressed response as mdns-sd writes one: PTR target and SRV owner
     * and target all point back into the first name. */
    static const uint8_t compressed[] = {0, 0, 0x84, 0, 0, 0, 0, 2, 0, 0, 0, 0,
        5, '_', 'f', 'i', 'p', 's', 4, '_', 'u', 'd', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0, 12, 0, 1, 0, 0, 0x11, 0x94, 0, 4, 1, 'x', 0xC0, 0x0C,
        0xC0, 0x28, 0, 33, 0x80, 1, 0, 0, 0, 120, 0, 10, 0, 0, 0, 0, 0x08, 0x49, 1, 'h', 0xC0, 0x17};
    fipsd_mdns_message *m = decode(compressed, sizeof compressed);
    CHECK(fipsd_mdns_is_response(m) && m->ancount == 2 && !m->truncated);
    fipsd_mdns_name t; uint16_t port; char text[256];
    CHECK(fipsd_mdns_ptr_target(&m->records[0], &t));
    CHECK(fipsd_mdns_name_to_text(&t, text, sizeof text) && !strcmp(text, "x._fips._udp.local."));
    CHECK(m->records[0].ttl == 4500 && !m->records[0].cache_flush);
    CHECK(m->records[1].rtype == FIPSD_MDNS_TYPE_SRV && m->records[1].cache_flush);
    CHECK(fipsd_mdns_name_to_text(&m->records[1].name, text, sizeof text) && !strcmp(text, "x._fips._udp.local."));
    CHECK(fipsd_mdns_srv_fields(&m->records[1], &port, &t) && port == 2121);
    CHECK(fipsd_mdns_name_to_text(&t, text, sizeof text) && !strcmp(text, "h.local."));
    char label[64];
    CHECK(fipsd_mdns_name_child_of(&m->records[1].name, &(fipsd_mdns_name){0}, label) == false);
    fipsd_mdns_name svc = N("_FIPS._udp.LOCAL");
    CHECK(fipsd_mdns_name_child_of(&m->records[1].name, &svc, label) && !strcmp(label, "x"));
    /* Malformed: a forward pointer, a pointer to itself, rdata past the end,
     * an A record of the wrong length, a truncated header. */
    uint8_t bad[64];
    memcpy(bad, compressed, 12); bad[5] = 1; bad[7] = 0; bad[12] = 0xC0; bad[13] = 0xFF; bad[14] = 0; bad[15] = 1; bad[16] = 0; bad[17] = 1;
    static fipsd_mdns_message scratch;
    CHECK(!fipsd_mdns_decode(bad, 18, &scratch));
    bad[13] = 0x0C; CHECK(!fipsd_mdns_decode(bad, 18, &scratch));
    CHECK(!fipsd_mdns_decode(compressed, sizeof compressed - 1, &scratch));
    memcpy(bad, want_a, sizeof want_a); bad[30] = 3; CHECK(!fipsd_mdns_decode(bad, sizeof want_a - 1, &scratch));
    CHECK(!fipsd_mdns_decode(want_a, 11, &scratch));
    /* A question with the QU bit. */
    fipsd_mdns_writer_init(&w, buf, sizeof buf, 0, 0);
    CHECK(fipsd_mdns_put_question(&w, &h, FIPSD_MDNS_TYPE_ANY, true));
    size_t n = fipsd_mdns_writer_finish(&w);
    m = decode(buf, n);
    CHECK(!fipsd_mdns_is_response(m) && m->qdcount == 1 && m->questions[0].unicast && m->questions[0].qtype == 255 &&
          m->questions[0].qclass == FIPSD_MDNS_CLASS_IN);
    /* Writer order: a question after a record is refused. */
    fipsd_mdns_writer_init(&w, buf, sizeof buf, 0, 0);
    CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ADDITIONAL, &rr, 0, false));
    CHECK(!fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false) && !fipsd_mdns_writer_finish(&w));
    /* TXT: key=value, bare key, empty value; the empty record. */
    fipsd_mdns_txt_entry e[] = {{"npub", "npub1x"}, {"flag", NULL}, {"v", ""}};
    CHECK(fipsd_mdns_rr_txt(&rr, &h, e, 3, 4500, true));
    static const uint8_t want_txt[] = {11, 'n', 'p', 'u', 'b', '=', 'n', 'p', 'u', 'b', '1', 'x', 4, 'f', 'l', 'a', 'g', 2, 'v', '='};
    CHECK(rr.rdlen == sizeof want_txt && !memcmp(rr.rdata, want_txt, sizeof want_txt));
    size_t pos = 0; const uint8_t *k, *v; size_t kl, vl; bool hv;
    CHECK(fipsd_mdns_txt_next(rr.rdata, rr.rdlen, &pos, &k, &kl, &v, &vl, &hv) && kl == 4 && hv && vl == 6);
    CHECK(fipsd_mdns_txt_next(rr.rdata, rr.rdlen, &pos, &k, &kl, &v, &vl, &hv) && kl == 4 && !hv);
    CHECK(fipsd_mdns_txt_next(rr.rdata, rr.rdlen, &pos, &k, &kl, &v, &vl, &hv) && kl == 1 && hv && !vl);
    CHECK(!fipsd_mdns_txt_next(rr.rdata, rr.rdlen, &pos, &k, &kl, &v, &vl, &hv));
    CHECK(fipsd_mdns_rr_txt(&rr, &h, NULL, 0, 4500, true) && rr.rdlen == 1 && rr.rdata[0] == 0);
    /* Names compare without ASCII case. */
    fipsd_mdns_name a = N("Fips-X.Local."), b = N("fips-x.local");
    CHECK(fipsd_mdns_name_equal(&a, &b));
    printf("records: ok\n");
}

/* ---- upstream tests.rs -------------------------------------------- */
static fipsd_mdns_resolved *resolved_advert(const char *host, const uint8_t v4[4], uint16_t port,
                                            const fipsd_mdns_txt_entry *txt, size_t n)
{
    static fipsd_mdns_resolved r;
    static fipsd_mdns_rr rr;
    memset(&r, 0, sizeof r);
    snprintf(r.fullname, sizeof r.fullname, "evil._fips._udp.local.");
    snprintf(r.host, sizeof r.host, "%s", host);
    r.port = port;
    fipsd_mdns_name owner = N("evil._fips._udp.local.");
    CHECK(fipsd_mdns_rr_txt(&rr, &owner, txt, n, 4500, true));
    r.txt_len = rr.rdlen; memcpy(r.txt, rr.rdata, rr.rdlen);
    r.addr_count = 1; r.addrs[0].ip.width = 4; memcpy(r.addrs[0].ip.bytes, v4, 4);
    return &r;
}
static void fips_host(const char *npub, char *out) { char l[64]; fipsd_mdns_advert_label(npub, l, sizeof l); snprintf(out, 160, "%s.local.", l); }
static const uint8_t ten_two[4] = {10, 2, 0, 5};
static void test_upstream(void)
{
    fipsd_mdns_scoped_ip s = {.ip = {.width = 4, .bytes = {192, 168, 178, 91}}};
    fipsd_mdns_sockaddr a; char text[80];
    /* scoped_ipv4_advert_becomes_socket_addr */
    CHECK(fipsd_mdns_socket_addr(&s, 51820, &a) && fipsd_mdns_sockaddr_text(&a, text, sizeof text) && !strcmp(text, "192.168.178.91:51820"));
    /* scope_less_ipv6_link_local_advert_is_skipped */
    fipsd_mdns_scoped_ip ll = {.ip = {.width = 16, .bytes = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x32, 0xc5, 0x99, 0xff, 0xfe, 0xa7, 0x5f, 0xe9}}};
    CHECK(!fipsd_mdns_socket_addr(&ll, 51820, &a));
    ll.scope = 3;
    CHECK(fipsd_mdns_socket_addr(&ll, 51820, &a) && a.scope_id == 3 && fipsd_mdns_sockaddr_text(&a, text, sizeof text) &&
          !strcmp(text, "[fe80::32c5:99ff:fea7:5fe9%3]:51820"));
    /* non_link_local_ipv6_advert_is_preserved */
    fipsd_mdns_scoped_ip lo6 = {.ip = {.width = 16, .bytes = {[15] = 1}}};
    CHECK(fipsd_mdns_socket_addr(&lo6, 51820, &a) && fipsd_mdns_sockaddr_text(&a, text, sizeof text) && !strcmp(text, "[::1]:51820"));
    /* an_ipv4_record_carries_every_interface_it_arrived_on */
    uint32_t ifs[FIPSD_MDNS_MAX_IFACES];
    fipsd_mdns_scoped_ip v4 = {.ip = {.width = 4, .bytes = {192, 168, 1, 7}}, .ifaces = {2}, .iface_count = 1};
    CHECK(fipsd_mdns_arrival_interfaces(&v4, ifs) == 1 && ifs[0] == 2);
    v4.ifaces[0] = 0;
    CHECK(fipsd_mdns_arrival_interfaces(&v4, ifs) == 0);
    CHECK(fipsd_mdns_arrival_interfaces(&lo6, ifs) == 0);

    char npub[128], other[128], own[128], host[160];
    npub_of_seed(0x31, npub); npub_of_seed(0x32, other); npub_of_seed(0x33, own);
    fipsd_mdns_advert_fields f;
    /* an_advert_whose_service_host_is_another_machine_yields_no_lan_candidate */
    fipsd_mdns_txt_entry t1[] = {{"npub", npub}};
    CHECK(fipsd_mdns_check_advert(resolved_advert("printer.local.", ten_two, 22, t1, 1), own, NULL, &f) == FIPSD_MDNS_SKIP_FOREIGN_HOST);
    fips_host(npub, host);
    CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 22, t1, 1), own, NULL, &f) == FIPSD_MDNS_ADVERT_OK);
    CHECK(!strcmp(f.npub, npub) && f.port == 22);
    /* an_advert_naming_another_fips_nodes_host_yields_no_lan_candidate */
    fips_host(other, host);
    CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 2121, t1, 1), own, NULL, &f) == FIPSD_MDNS_SKIP_FOREIGN_HOST);
    /* a_fips_responders_host_name_matches_in_any_case_and_after_a_conflict_rename */
    char label[64], h[200]; fipsd_mdns_advert_label(npub, label, sizeof label);
    const char *good[] = {"%s.local.", "%s.local", "%s-2.local.", "%s-17.local."};
    for (size_t i = 0; i < 4; ++i) { snprintf(h, sizeof h, good[i], label); CHECK(fipsd_mdns_host_is_advertisers(h, npub, strlen(npub))); }
    snprintf(h, sizeof h, "%s.local.", label);
    for (char *c = h; *c; ++c) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    CHECK(fipsd_mdns_host_is_advertisers(h, npub, strlen(npub)));
    const char *bad[] = {"%s-.local.", "%s-x.local.", "%sx.local.", "%s.example.", "x%s.local."};
    for (size_t i = 0; i < 5; ++i) { snprintf(h, sizeof h, bad[i], label); CHECK(!fipsd_mdns_host_is_advertisers(h, npub, strlen(npub))); }
    CHECK(!fipsd_mdns_host_is_advertisers("printer.local.", npub, strlen(npub)));
    const char *nonascii = "npub1\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
    CHECK(!fipsd_mdns_host_is_advertisers("fips-npub1\xc3\xa9\xc3\xa9\xc3\xa9.local.", nonascii, strlen(nonascii)));
    /* the_advert_checks_keep_their_other_refusals */
    fips_host(npub, host);
    fipsd_mdns_txt_entry nonpub[] = {{"v", "1"}}, scoped[] = {{"npub", npub}, {"scope", "b"}};
    CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 2121, nonpub, 1), own, NULL, &f) == FIPSD_MDNS_SKIP_NO_NPUB);
    CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 2121, t1, 1), npub, NULL, &f) == FIPSD_MDNS_SKIP_OWN);
    CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 2121, scoped, 2), own, "a", &f) == FIPSD_MDNS_SKIP_CROSS_SCOPE);
    CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 2121, scoped, 2), own, "b", &f) == FIPSD_MDNS_ADVERT_OK &&
          f.has_scope && !strcmp(f.scope, "b"));
    CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 2121, t1, 1), own, "b", &f) == FIPSD_MDNS_SKIP_CROSS_SCOPE);
    CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 0, t1, 1), own, NULL, &f) == FIPSD_MDNS_SKIP_ZERO_PORT);
    /* a_cross_scope_advert_with_a_non_ascii_npub_is_logged_without_a_panic */
    const char *wide = "npub1aaaaaaaaaa\xc3\xa9zzzz";
    fipsd_mdns_txt_entry w[] = {{"npub", wide}, {"scope", "b"}};
    fipsd_mdns_resolved *r = resolved_advert("anything.local.", ten_two, 2121, w, 2);
    fipsd_mdns_skip skip = fipsd_mdns_check_advert(r, own, "a", &f);
    CHECK(skip == FIPSD_MDNS_SKIP_CROSS_SCOPE);
    char line[1024];
    fipsd_mdns_skip_line(r, skip, &f, "a", line, sizeof line);
    CHECK(strstr(line, "npub=npub1aaaaaaaaaa ") && strstr(line, "cross-scope"));
    CHECK(strstr(line, "their_scope=Some(\"b\")") && strstr(line, "our_scope=Some(\"a\")"));
    /* an_advert_under_another_npub_sharing_a_fips_hosts_label_passes_the_host_check */
    fips_peer_identity target = identity_of(npub), twin_id;
    fips_xonly_pubkey key = target.pubkey; bool found = false;
    for (unsigned flip = 1; flip <= 255 && !found; ++flip) {
        key.bytes[31] ^= (uint8_t)flip;
        found = fips_peer_identity_from_pubkey(secp, &key, &twin_id) == FIPS_IDENTITY_OK;
        if (!found) key.bytes[31] ^= (uint8_t)flip;
    }
    CHECK(found);
    char twin[128], la[32], lb[32]; CHECK(fips_encode_npub(&key, twin, sizeof twin) == FIPS_IDENTITY_OK);
    CHECK(strcmp(twin, npub) && fipsd_mdns_advert_label_of(twin, strlen(twin), la) && fipsd_mdns_advert_label_of(npub, strlen(npub), lb) && !strcmp(la, lb));
    fipsd_mdns_txt_entry tw[] = {{"npub", twin}};
    CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 2121, tw, 1), own, NULL, &f) == FIPSD_MDNS_ADVERT_OK && !strcmp(f.npub, twin));
    /* ... and the node refuses it once the label is a known peer's (node::tests::lan_label). */
    fipsd_lan_known_labels *known = calloc(1, sizeof *known);
    fipsd_lan_known_labels_add(known, npub, &target.node_addr);
    CHECK(fipsd_lan_known_labels_is_impostor(known, twin, &twin_id.node_addr));
    CHECK(!fipsd_lan_known_labels_is_impostor(known, npub, &target.node_addr));
    fips_peer_identity other_id = identity_of(other);
    CHECK(!fipsd_lan_known_labels_is_impostor(known, other, &other_id.node_addr));
    free(known);
    /* an_advert_carrying_our_npub_in_another_case_is_skipped_as_our_own */
    char upper[128], mixed[128];
    for (size_t i = 0; ; ++i) {
        char c = own[i];
        upper[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
        mixed[i] = i % 2 == 0 ? upper[i] : c;
        if (!c) break;
    }
    const char *spellings[] = {own, upper, mixed};
    fips_host(own, host);
    for (size_t i = 0; i < 3; ++i) {
        fipsd_mdns_txt_entry e[] = {{"npub", spellings[i]}};
        CHECK(fipsd_mdns_check_advert(resolved_advert(host, ten_two, 2121, e, 1), own, NULL, &f) == FIPSD_MDNS_SKIP_OWN);
    }
    /* lan_rendezvous_scope */
    char sc[64];
    CHECK(fipsd_mdns_resolve_scope("  lan-x ", "app", sc, sizeof sc) && !strcmp(sc, "lan-x"));
    CHECK(fipsd_mdns_resolve_scope(NULL, "fips-overlay-v1", sc, sizeof sc) && !strcmp(sc, "fips-overlay-v1"));
    CHECK(fipsd_mdns_resolve_scope("", "fips-overlay-v1:mesh-a", sc, sizeof sc) && !strcmp(sc, "mesh-a"));
    CHECK(!fipsd_mdns_resolve_scope(" ", "fips-overlay-v1: ", sc, sizeof sc));
    CHECK(!fipsd_mdns_resolve_scope(NULL, "", sc, sizeof sc));
    printf("upstream tests.rs: ok\n");
}

/* ---- browser -------------------------------------------------------- */
static void test_browser(void)
{
    char npub[128], label[64], host[160];
    npub_of_seed(0x41, npub); fipsd_mdns_advert_label(npub, label, sizeof label); fips_host(npub, host);
    fipsd_mdns_txt_entry txt[] = {{"npub", npub}, {"scope", "s"}, {"v", "1"}};
    const uint8_t v4a[1][4] = {{192, 168, 1, 7}}, v4b[1][4] = {{192, 168, 1, 8}};
    const uint8_t v6[1][16] = {{0xfe, 0x80, [15] = 9}};
    advert_spec spec = {FIPSD_MDNS_SERVICE_TYPE, label, host, 2121, txt, 3, v4a, 1, v6, 1, 4500, 120, 4500, 120};
    uint8_t buf[FIPSD_MDNS_MAX_PACKET], q[FIPSD_MDNS_MAX_PACKET];
    size_t n = announce(&spec, buf);
    static fipsd_mdns_resolved r;
    fixed_value = 50;
    fipsd_mdns_browser *b = fipsd_mdns_browser_create(FIPSD_MDNS_SERVICE_TYPE, fixed_rng, 0);
    CHECK(b);
    /* Continuous querying: first query 20 + 50 ms after start, then 1 s,
     * 2 s, 4 s apart. */
    CHECK(fipsd_mdns_browser_deadline(b) == 70);
    CHECK(!fipsd_mdns_browser_query(b, 69, q, sizeof q));
    size_t qn = fipsd_mdns_browser_query(b, 70, q, sizeof q);
    fipsd_mdns_message *m = decode(q, qn);
    CHECK(qn && m->qdcount == 1 && m->questions[0].qtype == FIPSD_MDNS_TYPE_PTR && !m->ancount && !m->questions[0].unicast);
    CHECK(fipsd_mdns_browser_deadline(b) == 1070);
    CHECK(fipsd_mdns_browser_query(b, 1070, q, sizeof q) && fipsd_mdns_browser_deadline(b) == 3070);
    /* One announcement resolves the instance, with the interface it came on. */
    fipsd_mdns_browser_ingest(b, decode(buf, n), 3, 2000);
    CHECK(fipsd_mdns_browser_next_resolved(b, &r));
    CHECK(r.port == 2121 && !strcmp(r.host, host) && r.addr_count == 2);
    CHECK(r.addrs[0].ip.width == 4 && r.addrs[0].iface_count == 1 && r.addrs[0].ifaces[0] == 3);
    CHECK(r.addrs[1].ip.width == 16 && r.addrs[1].scope == 3);
    fipsd_mdns_advert_fields f; char own[128]; npub_of_seed(0x42, own);
    CHECK(fipsd_mdns_check_advert(&r, own, "s", &f) == FIPSD_MDNS_ADVERT_OK);
    /* Duplicate suppression: the same announcement again reports nothing. */
    fipsd_mdns_browser_ingest(b, decode(buf, n), 3, 2100);
    CHECK(!fipsd_mdns_browser_next_resolved(b, &r));
    /* The IPv4 record seen on a second interface is reported again, with both. */
    fipsd_mdns_browser_ingest(b, decode(buf, n), 5, 2200);
    CHECK(fipsd_mdns_browser_next_resolved(b, &r));
    CHECK(r.addrs[0].iface_count == 2 && r.addrs[0].ifaces[1] == 5 && r.addr_count == 3);
    /* The next browse query carries the PTR as a known answer (over half its
     * TTL left), with the TTL that remains. */
    qn = fipsd_mdns_browser_query(b, 3070, q, sizeof q);
    m = decode(q, qn);
    CHECK(m->ancount == 1 && m->records[0].rtype == FIPSD_MDNS_TYPE_PTR && m->records[0].ttl == 4500 - 1);
    /* Cache flush: a new address with the flush bit a second later expires
     * the old one within a second; it leaves resolution at once. */
    spec.v4 = v4b; spec.nv6 = 0;
    n = announce(&spec, buf);
    fipsd_mdns_browser_ingest(b, decode(buf, n), 3, 4000);
    CHECK(fipsd_mdns_browser_next_resolved(b, &r));
    {   /* Only the A rrset is flushed; the AAAA records stay. */
        size_t v4s = 0, v6s = 0;
        for (size_t i = 0; i < r.addr_count; ++i) {
            if (r.addrs[i].ip.width == 4) { ++v4s; CHECK(r.addrs[i].ip.bytes[3] == 8); }
            else ++v6s;
        }
        CHECK(v4s == 1 && v6s == 2);
    }
    /* Goodbye: TTL 0 on the PTR removes the instance at once, the records a
     * second later. */
    size_t before = fipsd_mdns_browser_cached(b);
    spec.ttl_ptr = 0;
    n = announce(&spec, buf);
    fipsd_mdns_browser_ingest(b, decode(buf, n), 3, 5000);
    char name[256];
    CHECK(fipsd_mdns_browser_next_removed(b, name, sizeof name) && strstr(name, label));
    CHECK(!fipsd_mdns_browser_next_resolved(b, &r));
    CHECK(fipsd_mdns_browser_cached(b) == before);
    CHECK(fipsd_mdns_browser_deadline(b) <= 6000);
    fipsd_mdns_browser_expire(b, 6000);
    CHECK(fipsd_mdns_browser_cached(b) < before);
    /* It returns: reported in full again. */
    spec.ttl_ptr = 4500;
    n = announce(&spec, buf);
    fipsd_mdns_browser_ingest(b, decode(buf, n), 3, 7000);
    CHECK(fipsd_mdns_browser_next_resolved(b, &r) && r.addr_count >= 1);
    /* TTL expiry: the 120 s SRV and address records go, the instance with them. */
    fipsd_mdns_browser_expire(b, 7000 + 120000);
    CHECK(fipsd_mdns_browser_next_removed(b, name, sizeof name));
    /* Refresh at 80% of a record's TTL: a query naming it is due. */
    fipsd_mdns_browser_destroy(b);
    fixed_value = 0;
    b = fipsd_mdns_browser_create(FIPSD_MDNS_SERVICE_TYPE, fixed_rng, 0);
    spec.ttl_srv = 100; spec.ttl_addr = 100;
    n = announce(&spec, buf);
    fipsd_mdns_browser_ingest(b, decode(buf, n), 3, 0);
    CHECK(fipsd_mdns_browser_next_resolved(b, &r));
    (void)fipsd_mdns_browser_query(b, 20, q, sizeof q);
    CHECK(fipsd_mdns_browser_deadline(b) == 1020); /* the browse backoff comes first */
    qn = fipsd_mdns_browser_query(b, 79999, q, sizeof q);
    if (qn) { m = decode(q, qn); for (size_t i = 0; i < m->qdcount; ++i) CHECK(m->questions[i].qtype == FIPSD_MDNS_TYPE_PTR); }
    qn = fipsd_mdns_browser_query(b, 80000, q, sizeof q);
    m = decode(q, qn);
    bool asks_srv = false, asks_a = false;
    for (size_t i = 0; i < m->qdcount; ++i) {
        asks_srv |= m->questions[i].qtype == FIPSD_MDNS_TYPE_SRV;
        asks_a |= m->questions[i].qtype == FIPSD_MDNS_TYPE_A;
    }
    CHECK(asks_srv && asks_a);
    /* Not cached: a probe's authority records and a query's known answers. */
    fipsd_mdns_browser_destroy(b);
    b = fipsd_mdns_browser_create(FIPSD_MDNS_SERVICE_TYPE, fixed_rng, 0);
    static fipsd_mdns_message qm;
    qm = *decode(buf, n);
    qm.flags = 0;
    fipsd_mdns_browser_ingest(b, &qm, 3, 0);
    CHECK(!fipsd_mdns_browser_cached(b));
    /* An instance missing its records is asked for them at once, then one
     * and three seconds later. */
    fipsd_mdns_writer w; fipsd_mdns_writer_init(&w, buf, sizeof buf, 0, FIPSD_MDNS_FLAG_QR | FIPSD_MDNS_FLAG_AA);
    static fipsd_mdns_rr rr;
    fipsd_mdns_name svc = N(FIPSD_MDNS_SERVICE_TYPE), inst;
    CHECK(fipsd_mdns_name_prepend(label, &svc, &inst));
    fipsd_mdns_rr_ptr(&rr, &svc, &inst, 4500); CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    n = fipsd_mdns_writer_finish(&w);
    fipsd_mdns_browser_ingest(b, decode(buf, n), 3, 10);
    CHECK(!fipsd_mdns_browser_next_resolved(b, &r));
    qn = fipsd_mdns_browser_query(b, 10, q, sizeof q);
    m = decode(q, qn);
    bool srv = false, tx = false;
    for (size_t i = 0; i < m->qdcount; ++i) { srv |= m->questions[i].qtype == FIPSD_MDNS_TYPE_SRV; tx |= m->questions[i].qtype == FIPSD_MDNS_TYPE_TXT; }
    CHECK(srv && tx);
    CHECK(fipsd_mdns_browser_deadline(b) <= 1010);
    /* Forgetting an interface drops what was learnt only there. */
    fipsd_mdns_browser_forget_interface(b, 3);
    CHECK(!fipsd_mdns_browser_cached(b));
    fipsd_mdns_browser_destroy(b);
    printf("browser: ok\n");
}

/* ---- responder ------------------------------------------------------ */
static fipsd_mdns_out outs[16];
static size_t poll_at(fipsd_mdns_responder *r, uint64_t now) { return fipsd_mdns_responder_poll(r, now, outs, 16); }
static void test_responder(void)
{
    char npub[128], label[64];
    npub_of_seed(0x51, npub); fipsd_mdns_advert_label(npub, label, sizeof label);
    fipsd_mdns_txt_entry txt[] = {{"npub", npub}, {"v", "1"}};
    fixed_value = 100;
    fipsd_mdns_responder_config cfg = {FIPSD_MDNS_SERVICE_TYPE, label, txt, 2, fixed_rng};
    fipsd_mdns_responder *r = fipsd_mdns_responder_create(&cfg);
    CHECK(r);
    fipsd_mdns_iface_addrs ifc = {.index = 4, .v4_count = 1, .v4 = {{192, 168, 1, 20}}, .v6_count = 1, .v6 = {{0xfe, 0x80, [15] = 0x20}}};
    fipsd_mdns_responder_set_interfaces(r, &ifc, 1, 0);
    /* No port yet: idle. */
    CHECK(fipsd_mdns_responder_deadline(r) == UINT64_MAX && !poll_at(r, 1000));
    fipsd_mdns_responder_set_port(r, 2121, 1000);
    CHECK(fipsd_mdns_responder_deadline(r) == 1100);
    /* Three probes 250 ms apart, QU on the first, our records as authority. */
    for (int i = 0; i < 3; ++i) {
        uint64_t t = 1100 + 250u * (unsigned)i;
        CHECK(!poll_at(r, t - 1));
        CHECK(poll_at(r, t) == 1);
        fipsd_mdns_message *m = decode(outs[0].bytes, outs[0].len);
        CHECK(!fipsd_mdns_is_response(m) && m->qdcount == 2 && m->questions[0].qtype == FIPSD_MDNS_TYPE_ANY);
        CHECK(m->questions[0].unicast == (i == 0) && m->nscount == 4);
    }
    CHECK(!fipsd_mdns_responder_announced(r, 4));
    /* 250 ms after the third probe, two announcements one second apart. */
    CHECK(poll_at(r, 1850) == 1 && fipsd_mdns_responder_announced(r, 4));
    fipsd_mdns_message *m = decode(outs[0].bytes, outs[0].len);
    CHECK(fipsd_mdns_is_response(m) && m->ancount == 5);
    CHECK(m->records[0].rtype == FIPSD_MDNS_TYPE_PTR && m->records[0].ttl == 4500 && !m->records[0].cache_flush);
    CHECK(m->records[1].rtype == FIPSD_MDNS_TYPE_SRV && m->records[1].ttl == 120 && m->records[1].cache_flush);
    CHECK(m->records[2].rtype == FIPSD_MDNS_TYPE_TXT && m->records[2].ttl == 4500);
    uint16_t port; fipsd_mdns_name t; char text[256];
    CHECK(fipsd_mdns_srv_fields(&m->records[1], &port, &t) && port == 2121);
    CHECK(fipsd_mdns_name_to_text(&t, text, sizeof text) && fipsd_mdns_host_is_advertisers(text, npub, strlen(npub)));
    CHECK(!poll_at(r, 2849) && poll_at(r, 2850) == 1);
    CHECK(fipsd_mdns_responder_deadline(r) == UINT64_MAX);
    /* A browse query is answered after the 20-120 ms aggregation delay with
     * the PTR and, as additional records, SRV, TXT and the addresses. */
    uint8_t q[FIPSD_MDNS_MAX_PACKET];
    fipsd_mdns_name svc = N(FIPSD_MDNS_SERVICE_TYPE);
    fipsd_mdns_writer w; fipsd_mdns_writer_init(&w, q, sizeof q, 0, 0);
    CHECK(fipsd_mdns_put_question(&w, &svc, FIPSD_MDNS_TYPE_PTR, false));
    size_t qn = fipsd_mdns_writer_finish(&w);
    static fipsd_mdns_message qm; qm = *decode(q, qn);
    fipsd_mdns_sockaddr from = {.ip = {.width = 4, .bytes = {192, 168, 1, 30}}, .port = 5353};
    fipsd_mdns_responder_on_message(r, &qm, 4, &from, false, 5000);
    CHECK(fipsd_mdns_responder_deadline(r) == 5000 + 20 + 100 % 101);
    CHECK(!poll_at(r, 5119) && poll_at(r, 5120) == 1 && !outs[0].unicast);
    m = decode(outs[0].bytes, outs[0].len);
    CHECK(m->ancount == 1 && m->arcount == 4 && m->records[0].rtype == FIPSD_MDNS_TYPE_PTR);
    /* Rate limit: asked again within a second, nothing is multicast. */
    fipsd_mdns_responder_on_message(r, &qm, 4, &from, false, 5500);
    CHECK(!poll_at(r, 5700));
    /* Known-answer suppression: the PTR listed with over half its TTL. */
    static fipsd_mdns_rr rr;
    fipsd_mdns_name inst; CHECK(fipsd_mdns_name_prepend(label, &svc, &inst));
    fipsd_mdns_writer_init(&w, q, sizeof q, 0, 0);
    CHECK(fipsd_mdns_put_question(&w, &svc, FIPSD_MDNS_TYPE_PTR, false));
    fipsd_mdns_rr_ptr(&rr, &svc, &inst, 3000); CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    qn = fipsd_mdns_writer_finish(&w); qm = *decode(q, qn);
    fipsd_mdns_responder_on_message(r, &qm, 4, &from, false, 9000);
    CHECK(fipsd_mdns_responder_deadline(r) == UINT64_MAX);
    /* ... but not with under half. */
    fipsd_mdns_writer_init(&w, q, sizeof q, 0, 0);
    CHECK(fipsd_mdns_put_question(&w, &svc, FIPSD_MDNS_TYPE_PTR, false));
    fipsd_mdns_rr_ptr(&rr, &svc, &inst, 2000); CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    qn = fipsd_mdns_writer_finish(&w); qm = *decode(q, qn);
    fipsd_mdns_responder_on_message(r, &qm, 4, &from, false, 9000);
    CHECK(poll_at(r, 9120) == 1);
    /* Legacy unicast (source port not 5353): an immediate unicast reply
     * echoing the ID and question, TTLs at most 10 s, no cache-flush. */
    fipsd_mdns_name hn; char hl[sizeof label + sizeof "-2.local."]; snprintf(hl, sizeof hl, "%s.local.", label); hn = N(hl);
    fipsd_mdns_writer_init(&w, q, sizeof q, 0x4242, 0);
    CHECK(fipsd_mdns_put_question(&w, &hn, FIPSD_MDNS_TYPE_A, false));
    qn = fipsd_mdns_writer_finish(&w); qm = *decode(q, qn);
    fipsd_mdns_sockaddr legacy = from; legacy.port = 40000;
    fipsd_mdns_responder_on_message(r, &qm, 4, &legacy, false, 9500);
    CHECK(poll_at(r, 9500) == 1 && outs[0].unicast && outs[0].dst.port == 40000);
    m = decode(outs[0].bytes, outs[0].len);
    CHECK(m->id == 0x4242 && m->qdcount == 1 && m->ancount == 1 && m->records[0].ttl == 10 && !m->records[0].cache_flush);
    /* QU right after the records were multicast: a unicast reply to port 5353. */
    fipsd_mdns_writer_init(&w, q, sizeof q, 0, 0);
    CHECK(fipsd_mdns_put_question(&w, &hn, FIPSD_MDNS_TYPE_A, true));
    qn = fipsd_mdns_writer_finish(&w); qm = *decode(q, qn);
    fipsd_mdns_responder_on_message(r, &qm, 4, &from, false, 9600);
    CHECK(poll_at(r, 9600) == 1 && outs[0].unicast && outs[0].dst.port == 5353);
    /* An address change re-announces, carrying a goodbye for the lost address. */
    fipsd_mdns_iface_addrs moved = ifc; moved.v4[0][3] = 21;
    fipsd_mdns_responder_set_interfaces(r, &moved, 1, 20000);
    CHECK(poll_at(r, 20000) == 1);
    m = decode(outs[0].bytes, outs[0].len);
    bool goodbye = false, fresh = false;
    for (size_t i = 0; i < m->ancount; ++i) if (m->records[i].rtype == FIPSD_MDNS_TYPE_A) {
        if (m->records[i].rdata[3] == 20) goodbye = m->records[i].ttl == 0;
        if (m->records[i].rdata[3] == 21) fresh = m->records[i].ttl == 120;
    }
    CHECK(goodbye && fresh);
    CHECK(poll_at(r, 21000) == 1);
    /* Shutdown: every record with TTL 0. */
    size_t g = fipsd_mdns_responder_goodbye(r, outs, 16);
    CHECK(g == 1);
    m = decode(outs[0].bytes, outs[0].len);
    CHECK(m->ancount == 5);
    for (size_t i = 0; i < m->ancount; ++i) CHECK(m->records[i].ttl == 0);
    fipsd_mdns_responder_destroy(r);

    /* Conflict while probing: another host answers for our host name with
     * an address that is not ours: rename to <label>-2 and probe again. Our
     * own packet looped back is not a conflict. */
    r = fipsd_mdns_responder_create(&cfg);
    fipsd_mdns_responder_set_interfaces(r, &ifc, 1, 0);
    fipsd_mdns_responder_set_port(r, 2121, 0);
    CHECK(poll_at(r, 100) == 1);
    uint8_t ab[FIPSD_MDNS_MAX_PACKET];
    fipsd_mdns_writer_init(&w, ab, sizeof ab, 0, FIPSD_MDNS_FLAG_QR | FIPSD_MDNS_FLAG_AA);
    fipsd_mdns_rr_a(&rr, &hn, (const uint8_t[4]){192, 168, 1, 99}, 120, true);
    CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    size_t an = fipsd_mdns_writer_finish(&w);
    qm = *decode(ab, an);
    fipsd_mdns_responder_on_message(r, &qm, 4, &from, true, 150);
    CHECK(!strcmp(fipsd_mdns_responder_label(r), label));
    fipsd_mdns_responder_on_message(r, &qm, 4, &from, false, 150);
    char renamed[sizeof label + sizeof "-2"]; snprintf(renamed, sizeof renamed, "%s-2", label);
    CHECK(!strcmp(fipsd_mdns_responder_label(r), renamed));
    snprintf(hl, sizeof hl, "%s.local.", renamed);
    CHECK(fipsd_mdns_host_is_advertisers(hl, npub, strlen(npub)));
    CHECK(fipsd_mdns_responder_deadline(r) == 250);
    /* The same address as ours is no conflict. */
    fipsd_mdns_rr_a(&rr, &hn, ifc.v4[0], 120, true);
    /* Simultaneous probe: a probe for our (renamed) host proposing an address
     * that sorts after ours wins; we defer a second. */
    CHECK(poll_at(r, 250) == 1);
    fipsd_mdns_name rh = N(hl);
    fipsd_mdns_writer_init(&w, ab, sizeof ab, 0, 0);
    CHECK(fipsd_mdns_put_question(&w, &rh, FIPSD_MDNS_TYPE_ANY, false));
    fipsd_mdns_rr_a(&rr, &rh, (const uint8_t[4]){192, 168, 1, 250}, 120, true);
    CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_AUTHORITY, &rr, 0, false));
    an = fipsd_mdns_writer_finish(&w); qm = *decode(ab, an);
    fipsd_mdns_responder_on_message(r, &qm, 4, &from, false, 300);
    CHECK(fipsd_mdns_responder_deadline(r) == 1300);
    /* One that sorts before ours loses: our schedule is unchanged. */
    CHECK(poll_at(r, 1300) == 1);
    fipsd_mdns_writer_init(&w, ab, sizeof ab, 0, 0);
    CHECK(fipsd_mdns_put_question(&w, &rh, FIPSD_MDNS_TYPE_ANY, false));
    fipsd_mdns_rr_a(&rr, &rh, (const uint8_t[4]){10, 0, 0, 1}, 120, true);
    CHECK(fipsd_mdns_put_rr(&w, FIPSD_MDNS_AUTHORITY, &rr, 0, false));
    an = fipsd_mdns_writer_finish(&w); qm = *decode(ab, an);
    fipsd_mdns_responder_on_message(r, &qm, 4, &from, false, 1350);
    CHECK(fipsd_mdns_responder_deadline(r) == 1550);
    /* An interface that goes away is forgotten; a new one probes. */
    fipsd_mdns_responder_set_interfaces(r, NULL, 0, 2000);
    CHECK(fipsd_mdns_responder_deadline(r) == UINT64_MAX);
    fipsd_mdns_responder_destroy(r);
    printf("responder: ok\n");
}

/* ---- LAN candidate set (upstream peering/lan.rs) -------------------- */
static fipsd_lan_pair pair_of(const char *npub, uint8_t last, uint16_t port)
{
    fipsd_lan_pair p = {.identity = identity_of(npub), .transport = {1},
        .addr = {.ip = {.width = 4, .bytes = {192, 168, 1, last}}, .port = port}, .ifaces = {1}, .iface_count = 1};
    return p;
}
static fips_onlink_prefix storage[3][4];
static size_t storage_used;
static fips_onlink_prefixes on_one(const char *ip4, uint8_t len, uint32_t index)
{
    fips_onlink_prefix *store = storage[storage_used++];
    fips_onlink_prefixes set; fips_onlink_prefix e = {.net = {.width = 4}, .len = len, .has_index = true, .index = index};
    CHECK(sscanf(ip4, "%hhu.%hhu.%hhu.%hhu", &e.net.bytes[0], &e.net.bytes[1], &e.net.bytes[2], &e.net.bytes[3]) == 4);
    CHECK(fips_onlink_from_indexed(&set, store, 4, &e, 1) == FIPS_OK);
    return set;
}
static bool done_all(void *u, const fipsd_lan_dialed *d) { (void)u; (void)d; return true; }
static void test_lan(void)
{
    static char npubs[80][128];
    for (int i = 0; i < 80; ++i) npub_of_seed((uint8_t)(0x60 + i), npubs[i]);
    fipsd_lan_pending *s = calloc(1, sizeof *s);
    fipsd_lan_nodes none = {0};
    /* a_second_address_for_the_same_npub_does_not_displace_the_first, and
     * a_node_holds_at_most_four_pending_addresses_and_keeps_the_first_seen */
    for (int i = 0; i < 4; ++i) { fipsd_lan_pair p = pair_of(npubs[0], (uint8_t)(10 + i), 2121); CHECK(fipsd_lan_offer_pair(s, &p, none, 0, seeded_rng, NULL, NULL) == FIPSD_LAN_HELD); }
    fipsd_lan_pair p = pair_of(npubs[0], 10, 2121);
    CHECK(fipsd_lan_offer_pair(s, &p, none, 5, seeded_rng, NULL, NULL) == FIPSD_LAN_DUPLICATE);
    p = pair_of(npubs[0], 20, 2121);
    CHECK(fipsd_lan_offer_pair(s, &p, none, 5, seeded_rng, NULL, NULL) == FIPSD_LAN_REFUSED_PER_NODE);
    CHECK(s->count == 4 && s->pairs[0].first_seen_ms == 0);
    /* pending_lan_candidates_expire_after_ten_minutes */
    (void)fipsd_lan_expire(s, FIPSD_LAN_PENDING_MAX_AGE_MS - 1, NULL); CHECK(s->count == 4);
    (void)fipsd_lan_expire(s, FIPSD_LAN_PENDING_MAX_AGE_MS, NULL); CHECK(s->count == 0);
    /* a_configured_lan_candidate_is_never_displaced_by_unconfigured_newcomers
     * and the set-full warning fires once and releases after a minute below. */
    fips_node_addr configured[1] = {identity_of(npubs[1]).node_addr};
    fipsd_lan_nodes conf = {configured, 1};
    p = pair_of(npubs[1], 1, 1);
    CHECK(fipsd_lan_offer_pair(s, &p, conf, 0, seeded_rng, NULL, NULL) == FIPSD_LAN_HELD);
    for (int i = 0; i < 63; ++i) { p = pair_of(npubs[2 + i], (uint8_t)i, 2); CHECK(fipsd_lan_offer_pair(s, &p, conf, 0, seeded_rng, NULL, NULL) == FIPSD_LAN_HELD); }
    bool first = false; fipsd_lan_pair displaced;
    for (int i = 0; i < 10; ++i) {
        p = pair_of(npubs[65 + i], 200, 3);
        CHECK(fipsd_lan_offer_pair(s, &p, conf, 10, seeded_rng, &displaced, &first) == FIPSD_LAN_REPLACED);
        CHECK(first == (i == 0));
        CHECK(memcmp(displaced.identity.node_addr.bytes, configured[0].bytes, 16));
    }
    CHECK(s->count == 64 && s->latched);
    bool still = false;
    for (size_t i = 0; i < s->count; ++i) still |= !memcmp(s->pairs[i].identity.node_addr.bytes, configured[0].bytes, 16);
    CHECK(still);
    uint64_t refused = 0;
    CHECK(!fipsd_lan_expire(s, 20, &refused));
    fipsd_lan_take(s, &s->pairs[5].identity.node_addr, &s->pairs[5].addr);
    CHECK(!fipsd_lan_expire(s, 30, &refused));
    CHECK(!fipsd_lan_expire(s, 30 + FIPSD_LAN_BOUND_LOG_HOLD_MS - 1, &refused));
    CHECK(fipsd_lan_expire(s, 30 + FIPSD_LAN_BOUND_LOG_HOLD_MS, &refused) && refused == 10 && !s->latched);
    /* ordered_lists_at_most_two_unconfigured_nodes_per_address */
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 5; ++i) { p = pair_of(npubs[10 + i], 7, (uint16_t)(1000 + i)); CHECK(fipsd_lan_offer_pair(s, &p, none, 0, seeded_rng, NULL, NULL) == FIPSD_LAN_HELD); }
    size_t order[FIPSD_LAN_MAX_PENDING];
    CHECK(fipsd_lan_ordered(s, none, seeded_rng, order) == 2);
    fipsd_lan_note_dialed(s, &s->pairs[0].identity.node_addr, &s->pairs[0].addr, 0, 0);
    size_t n = fipsd_lan_ordered(s, none, seeded_rng, order);
    CHECK(n == 2);
    bool has_dialed = false;
    for (size_t i = 0; i < n; ++i) has_dialed |= order[i] == 0;
    CHECK(has_dialed);
    CHECK(fipsd_lan_in_flight(s) == 1);
    fipsd_lan_prune_dialed(s, done_all, NULL);
    CHECK(fipsd_lan_in_flight(s) == 0);
    /* A configured pair is listed first, outside the per-address bound. */
    fips_node_addr c4[1] = {s->pairs[4].identity.node_addr};
    n = fipsd_lan_ordered(s, (fipsd_lan_nodes){c4, 1}, seeded_rng, order);
    CHECK(n == 3 && order[0] == 4);
    /* lan_dial_targets_are_judged_against_this_nodes_links,
     * an_advert_naming_another_interfaces_prefix_is_not_on_link,
     * a_held_lan_pair_whose_link_went_away_is_not_dialled */
    fips_onlink_prefixes link = on_one("192.168.1.0", 24, 1), other = on_one("192.168.1.0", 24, 2), far = on_one("10.0.0.0", 8, 1);
    fipsd_mdns_sockaddr in = {.ip = {.width = 4, .bytes = {192, 168, 1, 7}}, .port = 1};
    uint32_t one = 1;
    CHECK(fipsd_lan_target_on_link(&in, &one, 1, &link));
    CHECK(!fipsd_lan_target_on_link(&in, &one, 1, &other));
    CHECK(!fipsd_lan_target_on_link(&in, &one, 1, &far));
    CHECK(!fipsd_lan_target_on_link(&in, NULL, 0, &link));
    fipsd_mdns_sockaddr ll = {.ip = {.width = 16, .bytes = {0xfe, 0x80, [15] = 1}}, .port = 1};
    CHECK(!fipsd_lan_target_on_link(&ll, NULL, 0, &far));
    ll.scope_id = 3;
    CHECK(fipsd_lan_target_on_link(&ll, NULL, 0, &far));
    fips_onlink_prefixes failed = fips_onlink_failed();
    CHECK(fipsd_lan_target_on_link(&ll, NULL, 0, &failed) && !fipsd_lan_target_on_link(&in, &one, 1, &failed));
    CHECK(fipsd_lan_retain_on_link(s, &link) == 0 && s->count == 5);
    CHECK(fipsd_lan_retain_on_link(s, &far) == 5 && s->count == 0);
    /* prune_connected drops every pair of a connected node. */
    for (int i = 0; i < 3; ++i) { p = pair_of(npubs[30], (uint8_t)i, 1); (void)fipsd_lan_offer_pair(s, &p, none, 0, seeded_rng, NULL, NULL); }
    p = pair_of(npubs[31], 1, 1); (void)fipsd_lan_offer_pair(s, &p, none, 0, seeded_rng, NULL, NULL);
    fips_node_addr conn[1] = {identity_of(npubs[30]).node_addr};
    fipsd_lan_prune_connected(s, (fipsd_lan_nodes){conn, 1});
    CHECK(s->count == 1);
    free(s);
    printf("lan: ok\n");
}

int main(void)
{
    CHECK(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    test_records();
    test_upstream();
    test_browser();
    test_responder();
    test_lan();
    fips_identity_destroy_secp(secp);
    return 0;
}
