/* End to end: a node, its handlers and the dns subsystem on a loopback UDP
 * port; a real AAAA query over a socket; the derived address in the answer;
 * the npub in show_identity_cache. Modes: (none) IPv6 loopback, `v4` IPv4
 * loopback, `mesh` loopback treated as the mesh interface (the query is
 * dropped unanswered, observed through the service's event channel). The
 * identity-cache check synchronises on the dispatcher: a live provider
 * command queued after the query returns only after the learn command it
 * follows was applied and published, so no sleep or polling is needed. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "node_test.h"
#include "test_tmpdir.h"
#include "dns/service.h"
#include "host_os.h"
#include "log.h"
#include <fips/identity/local.h>
#include <fips/identity/address.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); abort(); } } while (0)
#define WAIT_MS 20000

static fips_secp *secp;
typedef struct identity { char npub[FIPS_NIP19_CAPACITY]; fips_ipv6_address address; char ipv6[48]; } identity;
static identity generate(void)
{
    identity out; fips_identity *id;
    CHECK(fips_identity_generate(secp, fipsd_host_os_entropy(), &id) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_npub(id, out.npub, sizeof out.npub) == FIPS_IDENTITY_OK);
    out.address = fips_identity_address(id);
    CHECK(fips_ipv6_address_format(&out.address, out.ipv6, sizeof out.ipv6) == FIPS_IDENTITY_OK);
    fips_identity_destroy(id);
    return out;
}
static uint16_t word(const uint8_t *b, size_t off) { return (uint16_t)((b[off] << 8) | b[off + 1]); }
static size_t build_query(uint16_t id, const char *name, uint16_t qtype, uint8_t *out)
{
    out[0] = (uint8_t)(id >> 8); out[1] = (uint8_t)id; out[2] = 0x01; out[3] = 0; out[4] = 0; out[5] = 1; memset(out + 6, 0, 6);
    size_t at = 12;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.'); size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len) { out[at++] = (uint8_t)len; memcpy(out + at, p, len); at += len; }
        if (!dot) break;
        p = dot + 1;
    }
    out[at++] = 0; out[at++] = (uint8_t)(qtype >> 8); out[at++] = (uint8_t)qtype; out[at++] = 0; out[at++] = 1;
    return at;
}
static const uint8_t *first_rdata(const uint8_t *r, size_t len, size_t *rdlen)
{
    size_t off = 12; while (off < len && r[off]) off += 1 + r[off]; off += 5;
    CHECK(off + 12 <= len && r[off] == 0xC0); *rdlen = word(r, off + 10); return r + off + 12;
}
static int client; static struct sockaddr_storage server; static socklen_t server_len;
static void send_query(const uint8_t *q, size_t n) { CHECK(sendto(client, q, n, 0, (struct sockaddr *)&server, server_len) == (ssize_t)n); }
/* The next reply, failing the test if none arrives within WAIT_MS. */
static size_t next_reply(uint8_t *buf, size_t cap, const char *what)
{
    struct pollfd p = {.fd = client, .events = POLLIN};
    if (poll(&p, 1, WAIT_MS) <= 0) { fprintf(stderr, "no reply within %d ms: expected %s\n", WAIT_MS, what); abort(); }
    ssize_t n = recv(client, buf, cap, 0); CHECK(n > 0); return (size_t)n;
}
static void write_text(const char *path, const char *text)
{ FILE *f = fopen(path, "w"); CHECK(f); CHECK(fputs(text, f) >= 0); CHECK(fclose(f) == 0); }
static void set_mtime(const char *path, time_t secs)
{ struct timespec times[2] = {{0, UTIME_OMIT}, {secs, 0}}; CHECK(utimensat(AT_FDCWD, path, times, 0) == 0); }
/* A live provider command: the dispatcher barrier. */
static json_t *live(test_node *n, const char *name, json_t *params)
{
    fipsd_control_provider p = fipsd_node_handlers_provider(n->h);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fipsd_control_request_context rc = {.deadline_ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec + 5000000000u};
    json_t *out = p.command(p.user, name, params, &rc);
    json_decref(params);
    return out;
}
static bool cache_holds(test_node *n, const char *npub, const char *ipv6)
{
    json_t *v = nt_query(n, "show_identity_cache"); CHECK(v);
    json_t *entries = json_object_get(json_object_get(v, "data"), "entries"); CHECK(json_is_array(entries));
    bool found = false;
    for (size_t i = 0; i < json_array_size(entries); ++i) {
        json_t *e = json_array_get(entries, i);
        const char *got = json_string_value(json_object_get(e, "npub")), *addr = json_string_value(json_object_get(e, "ipv6_addr"));
        if (got && !strcmp(got, npub)) { found = true; CHECK(addr && !strcmp(addr, ipv6)); }
    }
    json_decref(v);
    return found;
}
static void (*const unused_helpers[])(void) = {(void (*)(void))nt_await, (void (*)(void))nt_identity, (void (*)(void))nt_print_trace, (void (*)(void))nt_drain};
static void debug_logging(void)
{
    fipsd_log_level level; const char *env = getenv("FIPSD_LOG_LEVEL");
    if (!env || !fipsd_log_level_parse(env, &level)) return;
    fipsd_log *log = fipsd_log_create(level, NULL); if (log) fipsd_log_set_default(log);
}

int main(int argc, char **argv)
{
    (void)unused_helpers;
    debug_logging();
    const char *mode = argc > 1 ? argv[1] : "v6";
    bool v4 = !strcmp(mode, "v4"), mesh = !strcmp(mode, "mesh");
    const char *tmp = fipsd_test_tmpdir(NULL);
    char hosts_path[PATH_MAX]; snprintf(hosts_path, sizeof hosts_path, "%s/hosts", tmp);
    CHECK(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    identity alias = generate(), core = generate(), target = generate(), later = generate();
    char line[160]; snprintf(line, sizeof line, "gateway   %s\n", alias.npub); write_text(hosts_path, line);
    time_t base_mtime = time(NULL) - 120; set_mtime(hosts_path, base_mtime);

    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); CHECK(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); CHECK(net);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64; hcfg.notice_capacity = 256; hcfg.tick_ms = 200;
    char yaml[1024];
    snprintf(yaml, sizeof yaml, "node: {control: {enabled: false}}\ndns: {enabled: true, bind_addr: '%s', port: 0, ttl: 300}\npeers: [{npub: '%s', alias: 'core', addresses: [{transport: udp, addr: '127.0.0.1:9999'}]}]",
             v4 ? "127.0.0.1" : "::1", core.npub);
    test_node a; CHECK(nt_init(&a, net, "A", "loop:a", yaml, &hcfg, 1));
    fipsd_dns_service *dns = fipsd_dns_service_create(a.configs, a.h, NULL); CHECK(dns);
    CHECK(fipsd_dns_service_set_hosts_path(dns, hosts_path) == 0);
    GoChannel *events = go_channel_create(64); CHECK(events);
    fipsd_dns_service_observe(dns, events);
    uint32_t lo = fipsd_dns_lookup_ifindex("lo"); if (!lo) lo = fipsd_dns_lookup_ifindex("lo0"); CHECK(lo);
    if (mesh) { fipsd_dns_mesh_side side = {.has_ifindex = true, .ifindex = lo}; fipsd_dns_service_set_mesh_side(dns, &side); }
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_dns_service_subsystem(dns)) == 0);
    CHECK(fipsd_runtime_start(rt) == 0);
    CHECK(fipsd_dns_service_local_addr(dns, &server, &server_len) == 0);
    CHECK(server.ss_family == (v4 ? AF_INET : AF_INET6));
    uint16_t port = v4 ? ntohs(((struct sockaddr_in *)&server)->sin_port) : ntohs(((struct sockaddr_in6 *)&server)->sin6_port);
    CHECK(port != 0);
    client = socket(server.ss_family, SOCK_DGRAM, IPPROTO_UDP); CHECK(client >= 0);
    CHECK(fcntl(client, F_SETFL, O_NONBLOCK) == 0);

    uint8_t q[512], r[1100]; size_t n, rn, rdlen; char name[128];
    snprintf(name, sizeof name, "%s.fips", target.npub);
    if (mesh) {
        /* Loopback is the "mesh" interface: the query from ::1 is dropped
         * before the screen reads it, and no reply is sent. */
        n = build_query(0x0001, name, FIPSD_DNS_TYPE_AAAA, q); send_query(q, n);
        void *value = NULL; GoSelectCase cases[] = {{GO_SELECT_RECEIVE, events, NULL, &value}};
        GoSelectResult sel = go_select_timeout(cases, 1, WAIT_MS); CHECK(sel.selected_case == 0 && value);
        fipsd_dns_event *e = value; CHECK(e->kind == FIPSD_DNS_EVENT_MESH); free(e);
        CHECK(recv(client, r, sizeof r, MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        json_t *v = live(&a, "disconnect", json_pack("{s:s}", "npub", target.npub)); CHECK(v); json_decref(v);
        CHECK(!cache_holds(&a, target.npub, target.ipv6));
    } else {
        /* 1. AAAA for <npub>.fips: the derived address, AA, exactly 28 bytes longer. */
        n = build_query(0x0001, name, FIPSD_DNS_TYPE_AAAA, q); send_query(q, n);
        rn = next_reply(r, sizeof r, "the AAAA answer");
        CHECK(rn == n + 28 && word(r, 0) == 0x0001 && (r[3] & 0x0F) == 0 && (word(r, 2) & 0x0400) && word(r, 6) == 1);
        CHECK(!memcmp(first_rdata(r, rn, &rdlen), target.address.bytes, 16) && rdlen == 16 && !memcmp(r + 12, q + 12, n - 12));
        /* 2. The identity was handed to the dispatcher before the reply: a
         * live command queued now runs after it, and show_identity_cache
         * holds the npub with its address. */
        json_t *v = live(&a, "disconnect", json_pack("{s:s}", "npub", target.npub)); CHECK(v); json_decref(v);
        CHECK(cache_holds(&a, target.npub, target.ipv6));
        /* 3. Aliases: the hosts file and the configured peer alias; any case. */
        n = build_query(0x0002, "gateway.fips", FIPSD_DNS_TYPE_AAAA, q); send_query(q, n);
        rn = next_reply(r, sizeof r, "the hosts-file alias answer");
        CHECK(word(r, 0) == 0x0002 && word(r, 6) == 1 && !memcmp(first_rdata(r, rn, &rdlen), alias.address.bytes, 16));
        n = build_query(0x0003, "CORE.FIPS", FIPSD_DNS_TYPE_AAAA, q); send_query(q, n);
        rn = next_reply(r, sizeof r, "the peer alias answer");
        CHECK(word(r, 0) == 0x0003 && word(r, 6) == 1 && !memcmp(first_rdata(r, rn, &rdlen), core.address.bytes, 16));
        v = live(&a, "disconnect", json_pack("{s:s}", "npub", core.npub)); CHECK(v); json_decref(v);
        CHECK(cache_holds(&a, alias.npub, alias.ipv6) && cache_holds(&a, core.npub, core.ipv6));
        /* 4. Negative answers and the screen through the socket. */
        n = build_query(0x0004, "unknown.fips", FIPSD_DNS_TYPE_AAAA, q); send_query(q, n);
        rn = next_reply(r, sizeof r, "NXDOMAIN"); CHECK(word(r, 0) == 0x0004 && (r[3] & 0x0F) == FIPSD_DNS_RCODE_NXDOMAIN && word(r, 6) == 0);
        n = build_query(0x0005, name, 1, q); send_query(q, n);
        rn = next_reply(r, sizeof r, "NODATA for A"); CHECK(word(r, 0) == 0x0005 && (r[3] & 0x0F) == 0 && word(r, 6) == 0);
        n = build_query(0x0006, name, 12, q); send_query(q, n);
        rn = next_reply(r, sizeof r, "NODATA for PTR"); CHECK(word(r, 0) == 0x0006 && (r[3] & 0x0F) == 0 && word(r, 6) == 0);
        uint8_t empty[12] = {0x00, 0x07, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; send_query(empty, sizeof empty);
        rn = next_reply(r, sizeof r, "FORMERR"); CHECK(rn == 12 && word(r, 0) == 0x0007 && (r[3] & 0x0F) == FIPSD_DNS_RCODE_FORMERR);
        /* a response fed back gets nothing; the next query is answered */
        uint8_t fed[1100]; memcpy(fed, r, rn); fed[0] = 0; fed[1] = 8; fed[2] |= 0x80; send_query(fed, rn);
        n = build_query(0x0009, "unknown.fips", FIPSD_DNS_TYPE_AAAA, q); send_query(q, n);
        rn = next_reply(r, sizeof r, "the query after the fed-back response"); CHECK(word(r, 0) == 0x0009);
        /* 5. Hot reload: the hosts file changes; the next query sees it. */
        snprintf(line, sizeof line, "gateway   %s\nserver2   %s\n", alias.npub, later.npub); write_text(hosts_path, line); set_mtime(hosts_path, base_mtime + 60);
        n = build_query(0x000A, "server2.fips", FIPSD_DNS_TYPE_AAAA, q); send_query(q, n);
        rn = next_reply(r, sizeof r, "the reloaded alias"); CHECK(word(r, 0) == 0x000A && word(r, 6) == 1 && !memcmp(first_rdata(r, rn, &rdlen), later.address.bytes, 16));
        /* Events were posted for every datagram, in order, with the outcomes above. */
        static const fipsd_dns_event_kind kinds[] = {FIPSD_DNS_EVENT_ANSWERED, FIPSD_DNS_EVENT_ANSWERED, FIPSD_DNS_EVENT_ANSWERED, FIPSD_DNS_EVENT_ANSWERED,
            FIPSD_DNS_EVENT_ANSWERED, FIPSD_DNS_EVENT_ANSWERED, FIPSD_DNS_EVENT_REFUSED, FIPSD_DNS_EVENT_DROPPED, FIPSD_DNS_EVENT_ANSWERED, FIPSD_DNS_EVENT_ANSWERED};
        for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; ++i) {
            void *value = NULL; GoSelectCase cases[] = {{GO_SELECT_RECEIVE, events, NULL, &value}};
            GoSelectResult sel = go_select_timeout(cases, 1, WAIT_MS); CHECK(sel.selected_case == 0 && value);
            fipsd_dns_event *e = value;
            if (e->kind != kinds[i]) { fprintf(stderr, "event %zu: kind %d, want %d\n", i, e->kind, kinds[i]); abort(); }
            if (e->kind == FIPSD_DNS_EVENT_DROPPED) CHECK(e->reason == FIPSD_DNS_DROP_RESPONSE);
            if (e->kind == FIPSD_DNS_EVENT_ANSWERED) CHECK(e->sent > 0 && e->path == FIPSD_DNS_REPLY_ROUTED);
            if (i == 0 || i == 1 || i == 2 || i == 9) CHECK(e->resolved); else CHECK(!e->resolved);
            free(e);
        }
    }
    /* Shutdown: the responder's pending receive is cancelled and the socket
     * closed within the grace period. */
    fipsd_runtime_shutdown(rt);
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt);
    fipsd_dns_service_destroy(dns);
    close(client);
    void *left; while (go_channel_try_receive(events, &left) == 0) free(left);
    go_channel_free(events);
    nt_drain(&a); nt_destroy(&a);
    fipsd_loopback_network_destroy(net);
    fips_identity_destroy_secp(secp);
    (void)unlink(hosts_path);
    printf("dns end to end (%s): bind, AAAA, identity cache, aliases, negatives, screen, reload, shutdown: ok\n", mode);
    return 0;
}
