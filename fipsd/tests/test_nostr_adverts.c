/* The node's own kind-37195 advert (nostrc-r4aeo), end to end through the
 * production pieces: config -> transport registry (three named TCP
 * instances and a directory-mode Tor onion with a control monitor) -> the
 * real Nostr node driver -> the rendezvous actor -> two libsoup NIP-01
 * relays. The fake control port's HS_DESC UPLOADED is the only thing that
 * adds the onion. Every wait is an event: a publish OK, a refetch EOSE
 * (FIFO behind earlier commands) or a control command line; no sleeps. */
#define _POSIX_C_SOURCE 200809L
#include "nostr/runtime.h"
#include "nostr/advert.h"
#include "host_os.h"
#include "nostr_relay_fixture.h"
#include "torctl_fixture.h"
#include "test_tmpdir.h"
#include "transport/tcp/tcp.h"
#include "transport/tor/tor.h"
#include <select.h>
#include <sodium.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID "ccccccccccccccccccccccccccccccccccccccccccccccccccccccq"
#define ONION ID ".onion"
#define NSEC "3333333333333333333333333333333333333333333333333333333333333333"
#define HSDIR " UNKNOWN $0101010101010101010101010101010101010101~hsdir1"
#define BODY(e) "{\"identifier\":\"fips-overlay-v1\",\"version\":1,\"endpoints\":[" e "]}"
#define TCP "{\"transport\":\"tcp\",\"addr\":\"8.8.8.8:4433\"}"
enum { PUBLISHED, FETCHED };
typedef struct { int kind; bool ok; int value; } notice;
typedef struct {
    fipsd_nostr_node_driver driver; /* the driver callbacks' user is this first member */
    GoChannel *notices;
    fipsd_config *config; GoSwap *configs; fipsd_node *node; fipsd_node_handlers *handlers;
    fipsd_netmon_source netmon; fipsd_nostr_runtime *relay;
    fipsd_transport *owned[FIPSD_TRANSPORT_MAX]; size_t tcp_count, tor_count;
    size_t adverts_reported; /* accepted advert publish notices consumed */
} harness;
static void *receive(GoChannel *c)
{
    void *p = NULL; GoSelectCase cs = {.op = GO_SELECT_RECEIVE, .chan = c, .recv_buf = &p};
    GoSelectResult r = go_select_timeout(&cs, 1, 20000); assert(r.selected_case == 0 && p); return p;
}
static void notify(harness *h, int kind, bool ok, int value)
{
    notice *n = malloc(sizeof *n); assert(n); *n = (notice){kind, ok, value};
    assert(!go_channel_try_send(h->notices, n));
}
static void published(void *user, const char *url, int kind, bool ok) { (void)url; notify(user, PUBLISHED, ok, kind); }
static void fetched(void *user, const char *peer, fipsd_nostr_refetch_outcome o) { (void)peer; notify(user, FETCHED, true, (int)o); }
static fipsd_network_snapshot *network(void *arg)
{ (void)arg; fipsd_network_snapshot *s = calloc(1, sizeof *s); assert(s); return s; }
/* The next accepted advert publish; other kinds (the 10050 inbox) pass. */
static void advert_published(harness *h)
{
    for (;;) {
        notice *n = receive(h->notices); int kind = n->kind, value = n->value; bool ok = n->ok; free(n);
        assert(kind == PUBLISHED);
        if (value == NOSTR_KIND_FIPS_OVERLAY_ADVERT) { assert(ok); ++h->adverts_reported; return; }
    }
}
/* A relay can store an advert before its publish reports, so once the relays
 * hold `stored` adverts in all, the reports still owed are taken here; a
 * barrier after this sees only publishes that came later. */
static void adverts_reported(harness *h, size_t stored)
{ while (h->adverts_reported < stored) advert_published(h); }
/* Commands are processed in order and a publish reports before the actor
 * takes the next command, so a refetch's result proves every earlier
 * command finished: no advert publish may come first. */
static void barrier(harness *h)
{
    assert(fipsd_nostr_runtime_refetch(h->relay, "1111111111111111111111111111111111111111111111111111111111111111"));
    for (;;) {
        notice *n = receive(h->notices); int kind = n->kind, value = n->value; free(n);
        if (kind == FETCHED) return;
        assert(value != NOSTR_KIND_FIPS_OVERLAY_ADVERT);
    }
}
static char *latest(fixture *f, size_t relay, const char *author, size_t *count, int64_t *created)
{
    char *json = fixture_latest(f, relay, NOSTR_KIND_FIPS_OVERLAY_ADVERT, author, count);
    if (!json) return NULL;
    NostrEvent *e = NULL; assert(fipsd_nostr_event_parse(json, strlen(json), &e) == FIPSD_NOSTR_OK); free(json);
    char *content = strdup(e->content); assert(content); *created = e->created_at; nostr_event_free(e); return content;
}
/* Both relays' newest advert has `want`; created[i] is its created_at. */
static bool both(fixture *f, const char *author, const char *want, size_t counts[2], int64_t created[2])
{
    bool same = true;
    for (size_t i = 0; i < 2; ++i) {
        char *content = latest(f, i, author, &counts[i], &created[i]);
        same = same && content && !strcmp(content, want); free(content);
    }
    return same;
}
static void expect_command(torctl_fixture *c, const char *prefix)
{
    for (;;) {
        char *line = receive(c->commands); bool hit = !strncmp(line, prefix, strlen(prefix)); free(line);
        if (hit) return;
    }
}
int main(void)
{
    assert(sodium_init() >= 0);
    const char *dir = fipsd_test_tmpdir(NULL);
    char hostname[512]; snprintf(hostname, sizeof hostname, "%s/hostname", dir);
    FILE *file = fopen(hostname, "w"); assert(file && fputs(ONION "\n", file) >= 0); fclose(file);
    fixture *f = fixture_start();
    torctl_fixture ctl = {0}; torctl_fixture_start(&ctl, NULL);
    pthread_mutex_lock(&ctl.mu); strcpy(ctl.password, "pw"); pthread_mutex_unlock(&ctl.mu);
    fips_secp *secp; fips_identity *me;
    assert(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    assert(fips_identity_from_secret_str(secp, fipsd_host_os_entropy(), NSEC, strlen(NSEC), &me) == FIPS_IDENTITY_OK);
    fips_xonly_pubkey pk = fips_identity_pubkey(me); char author[65]; sodium_bin2hex(author, sizeof author, pk.bytes, 32);
    /* pub: external_addr overrides its loopback bind. loop: bound to
     * loopback without one, never publicly usable. quiet: not advertised. */
    char yaml[4096];
    snprintf(yaml, sizeof yaml,
        "node:\n  identity: {nsec: '" NSEC "', persistent: true}\n  control: {enabled: false}\n"
        "  rendezvous:\n    nostr:\n      enabled: true\n      advertise: true\n"
        "      advert_relays: ['%s', '%s']\n      dm_relays: ['%s', '%s']\n"
        "transports:\n  tcp:\n"
        "    pub: {bind_addr: '127.0.0.1:0', advertise_on_nostr: true, external_addr: '8.8.8.8:4433'}\n"
        "    loop: {bind_addr: '127.0.0.1:0', advertise_on_nostr: true}\n"
        "    quiet: {bind_addr: '127.0.0.1:0', external_addr: '9.9.9.9:443'}\n"
        "  tor:\n    mode: directory\n    socks5_addr: '127.0.0.1:9'\n    control_addr: '%s'\n"
        "    control_auth: 'password:pw'\n    advertise_on_nostr: true\n"
        "    directory_service: {hostname_file: '%s', bind_addr: '127.0.0.1:0'}\n",
        fixture_url(f, 0), fixture_url(f, 1), fixture_url(f, 0), fixture_url(f, 1), ctl.addr, hostname);
    harness h = {.notices = go_channel_create(256)};
    h.config = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(h.config);
    h.configs = go_swap_create(h.config, fipsd_config_free);
    fipsd_node_handlers_config hc; fipsd_node_handlers_config_default(&hc); hc.peer_capacity = 4; hc.notice_capacity = 256;
    h.handlers = fipsd_node_handlers_create(&hc); assert(h.handlers);
    h.netmon = (fipsd_netmon_source){.read = network, .injected = true};
    fipsd_node_options options = {.workers = 2, .peer_capacity = 4, .netmon_source = &h.netmon};
    fipsd_node_handlers_install(h.handlers, &options); h.node = fipsd_node_create(h.configs, &options); assert(h.node);
    fipsd_transport_registry *registry = fipsd_node_transports(h.node);
    assert(!fipsd_tcp_register_config(registry, h.config, h.owned, &h.tcp_count) && h.tcp_count == 3);
    assert(!fipsd_tor_register_config(registry, h.config, h.owned + h.tcp_count, &h.tor_count) && h.tor_count == 1);
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false; rc.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    assert(!fipsd_runtime_register(rt, fipsd_node_subsystem(h.node)));
    fipsd_rendezvous_driver d = fipsd_nostr_node_driver_init(&h.driver, fipsd_node_get_context(h.node), h.handlers, registry);
    d.publish_result = published; d.refetch_result = fetched;
    h.relay = fipsd_nostr_runtime_create(h.config, NULL, fipsd_node_get_context(h.node), d); assert(h.relay);
    assert(!fipsd_runtime_register(rt, fipsd_nostr_runtime_subsystem(h.relay)));
    assert(!fipsd_runtime_start(rt));
    /* The monitor follows HS_DESC for this onion; Tor holds no descriptor yet. */
    expect_command(&ctl, "SETEVENTS STATUS_CLIENT NETWORK_LIVENESS BW HS_DESC");
    expect_command(&ctl, "GETINFO hs/service/desc/id/" ID);
    /* First advert: only pub's external TCP endpoint, at both relays. The
     * set may be built before pub's listener binds; then the bind republishes. */
    size_t base[2], now[2]; int64_t first[2], at[2];
    do advert_published(&h); while (!both(f, author, BODY(TCP), base, first));
    adverts_reported(&h, base[0] + base[1]);
    printf("tcp-only advert at both relays after %zu/%zu publishes: %s\n", base[0], base[1], BODY(TCP));
    /* Unchanged inputs: refresh requests (as STUN observation sends) publish nothing. */
    assert(fipsd_nostr_runtime_refresh_advert(h.relay) && fipsd_nostr_runtime_refresh_advert(h.relay));
    barrier(&h); assert(both(f, author, BODY(TCP), now, at) && now[0] == base[0] && now[1] == base[1]);
    /* Tor reports this service's descriptor uploaded: one republish per relay. */
    torctl_fixture_event(&ctl, "HS_DESC UPLOADED " ID HSDIR);
    const char *with_onion = BODY(TCP ",{\"transport\":\"tor\",\"addr\":\"" ONION ":443\"}");
    advert_published(&h); advert_published(&h);
    assert(both(f, author, with_onion, now, at) && now[0] == base[0] + 1 && now[1] == base[1] + 1);
    /* The fixture appends; a NIP-01 relay replaces, keeping the lowest id on a
     * tie. The new set must win there too: a strictly later created_at, even
     * though it follows the first advert within milliseconds. */
    assert(at[0] > first[0] && at[1] > first[1]);
    printf("onion joined after HS_DESC UPLOADED, republished once per relay: %s\n", with_onion);
    /* The other HSDirs' reports and a refresh change nothing: no republish. */
    torctl_fixture_event(&ctl, "HS_DESC UPLOADED " ID " UNKNOWN $0202020202020202020202020202020202020202~hsdir2");
    torctl_fixture_event(&ctl, "STATUS_CLIENT NOTICE CIRCUIT_ESTABLISHED");
    expect_command(&ctl, "GETINFO status/bootstrap-phase"); /* the monitor handled both lines */
    assert(fipsd_nostr_runtime_refresh_advert(h.relay));
    barrier(&h); assert(both(f, author, with_onion, now, at) && now[0] == base[0] + 1 && now[1] == base[1] + 1);
    /* What a peer reads back: signed, tagged, unexpired, both endpoints usable. */
    size_t count; char *json = fixture_latest(f, 0, NOSTR_KIND_FIPS_OVERLAY_ADVERT, author, &count); assert(json);
    NostrEvent *e = NULL; assert(fipsd_nostr_event_parse(json, strlen(json), &e) == FIPSD_NOSTR_OK); free(json);
    fipsd_nostr_advert parsed; uint64_t until, wall_ms = (uint64_t)e->created_at * 1000;
    assert(fipsd_nostr_advert_parse(e, FIPSD_NOSTR_IDENTIFIER, wall_ms, 3600000, &parsed, &until) == FIPSD_NOSTR_OK);
    assert(parsed.endpoint_count == 2 && parsed.endpoints[1].transport == FIPSD_NOSTR_TOR && !parsed.has_relays);
    nostr_event_free(e);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK); fipsd_runtime_destroy(rt);
    fipsd_nostr_runtime_destroy(h.relay); fipsd_node_destroy(h.node); fipsd_node_handlers_destroy(h.handlers);
    for (size_t i = 0; i < h.tcp_count; ++i) fipsd_tcp_destroy(h.owned[i]);
    for (size_t i = 0; i < h.tor_count; ++i) fipsd_tor_destroy(h.owned[h.tcp_count + i]);
    go_swap_free(h.configs); torctl_fixture_stop(&ctl); fixture_destroy(f);
    void *n; while (go_channel_try_receive(h.notices, &n) == 0) free(n); go_channel_unref(h.notices);
    fips_identity_destroy(me); fips_identity_destroy_secp(secp);
    puts("own advert: registry tcp (external_addr, advertise_on_nostr=false, loopback filtered) + onion after HS_DESC UPLOADED; change-only republish: OK");
    return 0;
}
