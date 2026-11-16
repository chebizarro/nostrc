/* Nym transport (nostrc-gfi8h.4), upstream transport/nym: outbound-only
 * streams through nym-socks5-client's SOCKS5 listener, here the loopback
 * fake. Covers configuration, the startup readiness probe (ready, and
 * proceeding after startup_timeout_secs when the client is absent), the
 * no-auth SOCKS5 codec on the wire (greeting 05 01 00, numeric and hostname
 * targets), frames both ways to a TCP listener, every SOCKS5 failure counted
 * as socks5_errors (REP 0x05 included: Nym keeps no connect_refused), the
 * connect deadline as connect_timeouts, and ops refused before the stream
 * owner started. Built without -DFIPSD_ENABLE_NYM it checks that a
 * configured transports.nym is refused. No sleeps. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "transport/nym/nym.h"
#include "test_tmpdir.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef FIPSD_ENABLE_NYM
int main(void)
{
    const char *yaml = "transports: {nym: {socks5_addr: '127.0.0.1:1080'}}";
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    fipsd_transport_registry r = {0}; fipsd_transport *owned[4]; size_t n = 0;
    assert(fipsd_nym_register_config(&r, cfg, owned, &n) == -1 && n == 0 && r.count == 0);
    fipsd_config_free(cfg);
    yaml = "transports: {udp: {}}"; cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    assert(fipsd_nym_register_config(&r, cfg, owned, &n) == 0 && n == 0);
    fipsd_config_free(cfg);
    puts("nym disabled: a configured transports.nym is refused, an absent one is fine: ok");
    return 0;
}
#else
#include "transport/tcp/internal.h"
#include "socks5_fixture.h"
static void *receive(GoChannel *channel)
{
    void *v = NULL; GoSelectCase c = {GO_SELECT_RECEIVE, channel, NULL, &v};
    GoSelectResult r = go_select_timeout(&c, 1, 15000); assert(r.selected_case == 0); return v;
}
static fipsd_transport_event *event(GoChannel *ch, fipsd_transport_event_kind kind)
{
    for (;;) {
        fipsd_transport_event *e = receive(ch); assert(e);
        if (e->kind == kind) return e;
        assert(e->kind == FIPSD_TRANSPORT_BECAME_PRESENT || e->kind == FIPSD_TRANSPORT_CONNECTED);
        free(e);
    }
}
static fipsd_transport_stats_view stats(const fipsd_transport *t) { return fipsd_transport_stats_read(&t->shared->stats); }
static void drained(fipsd_transport *t)
{
    tcp_instance *u = fipsd_nym_stream(t)->user;
    for (;;) {
        nsync_mu_lock(&u->mu); bool empty = !u->connections; nsync_mu_unlock(&u->mu);
        if (empty) return;
        (void)receive(u->changed);
    }
}
static void config(void)
{
    const char *yaml = "node: {limits: {max_connections: 5}}\ntransports: {nym: {mtu: 1300}}";
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    fipsd_transport_registry r = {0}; fipsd_transport *owned[4]; size_t n = 0;
    assert(fipsd_nym_register_config(&r, cfg, owned, &n) == 0 && n == 1);
    fipsd_transport *t = owned[0];
    assert(!strcmp(t->type, "nym") && !strcmp(t->name, "nym") && !t->accept_connections && fipsd_nym_is(t));
    assert(t->ops->mtu(t, NULL) == 1300);
    tcp_instance *u = fipsd_nym_stream(t)->user;
    assert(u->config.connect_timeout_ms == 300000 && u->config.max_outbound_connections == 5 && !u->config.bind_addr);
    fips_transport_addr none; assert(!t->ops->local_addr(t, &none));
    fipsd_nym_destroy(t); fipsd_config_free(cfg);
    yaml = "transports: {nym: {a: {socks5_addr: '127.0.0.1:1081'}, b: {startup_timeout_secs: 5}}}";
    cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg); n = 0; r = (fipsd_transport_registry){0};
    assert(fipsd_nym_register_config(&r, cfg, owned, &n) == 0 && n == 2);
    for (size_t i = 0; i < n; ++i) fipsd_nym_destroy(owned[i]);
    fipsd_config_free(cfg);
    yaml = "transports: {nym: {socks5_addr: 'no-port'}}";
    cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg); n = 0; r = (fipsd_transport_registry){0};
    assert(fipsd_nym_register_config(&r, cfg, owned, &n) == -1 && n == 0);
    fipsd_config_free(cfg);
    puts("config: defaults (300 s connect, 1080, outbound-only), named instances, invalid socks5_addr: ok");
}
typedef struct fixture {
    socks5_fixture proxy;
    fipsd_runtime *rt;
    fipsd_transport *nym, *absent, *tcp;
    GoChannel *en, *ea, *et, *done;
} fixture;
static int start(fipsd_subsystem *sub, fipsd_runtime *rt)
{
    fixture *f = sub->user; f->rt = rt;
    if (f->tcp->ops->start_async(f->tcp, rt, NULL, f->et)) return -1;
    /* Before the deferred start: nothing dials. */
    fips_transport_addr x; assert(fips_transport_addr_from_string("127.0.0.1:9", 11, &x) == FIPS_OK);
    assert(f->nym->ops->connect_async(f->nym, &x) == -1);
    return f->nym->ops->start_async(f->nym, rt, NULL, f->en) || f->absent->ops->start_async(f->absent, rt, NULL, f->ea);
}
static void stop(fipsd_subsystem *sub, fipsd_runtime *rt)
{
    (void)rt; fixture *f = sub->user;
    f->nym->ops->stop(f->nym); f->absent->ops->stop(f->absent); f->tcp->ops->stop(f->tcp);
}
static void scenario(void *arg)
{
    fixture *f = arg;
    /* Readiness: the probe reached the client; the absent one gave up after its timeout. */
    void *v; assert(go_channel_receive(fipsd_nym_ready(f->nym), &v) != 0 || go_channel_is_closed(fipsd_nym_ready(f->nym)));
    assert(fipsd_nym_proxy_ready(f->nym));
    assert(receive(fipsd_nym_bound(f->nym)) && receive(fipsd_tcp_bound(f->tcp)));
    (void)go_channel_receive(fipsd_nym_ready(f->absent), &v);
    assert(!fipsd_nym_proxy_ready(f->absent) && receive(fipsd_nym_bound(f->absent)));
    fips_transport_addr target; assert(fipsd_tcp_address(f->tcp, &target));
    /* Numeric target, no-auth: the greeting offers method 0 only. */
    assert(f->nym->ops->connect_async(f->nym, &target) == 0);
    free(event(f->en, FIPSD_TRANSPORT_CONNECTED));
    s5_request *r = receive(f->proxy.requests);
    assert(r->greeting_len == 3 && r->greeting[0] == 5 && r->greeting[1] == 1 && r->greeting[2] == 0);
    assert(!r->userpass && r->atyp == 1); free(r);
    uint8_t msg[114] = {1, 0, 110, 0};
    assert(f->nym->ops->send(f->nym, &target, msg, sizeof msg) == 0);
    fipsd_transport_event *e = event(f->et, FIPSD_TRANSPORT_PACKET); fips_transport_addr back = e->address; free(e);
    assert(f->tcp->ops->send(f->tcp, &back, msg, sizeof msg) == 0);
    free(event(f->en, FIPSD_TRANSPORT_PACKET));
    /* Hostname target: resolved by the mixnet exit, no dot required. */
    const char *text; size_t n; assert(fips_transport_addr_as_str(&target, &text, &n) == FIPS_OK);
    char named[64]; snprintf(named, sizeof named, "exit-host:%s", strrchr(text, ':') + 1);
    fips_transport_addr host; assert(fips_transport_addr_from_string(named, strlen(named), &host) == FIPS_OK);
    socks5_fixture_route(&f->proxy, "exit-host", &target);
    assert(f->nym->ops->connect_async(f->nym, &host) == 0);
    free(event(f->en, FIPSD_TRANSPORT_CONNECTED));
    r = receive(f->proxy.requests); assert(r->atyp == 3 && !strcmp(r->host, "exit-host")); free(r);
    fipsd_transport_stats_view s = stats(f->nym);
    assert(s.connections_established == 2 && s.packets_sent == 1 && s.packets_recv == 1);
    f->nym->ops->close_connection(f->nym, &target); f->nym->ops->close_connection(f->nym, &host);
    free(event(f->et, FIPSD_TRANSPORT_DISCONNECTED)); free(event(f->et, FIPSD_TRANSPORT_DISCONNECTED));
    drained(f->nym);
    /* Every SOCKS5 failure is a socks5_error, a refusal included. */
    socks5_fixture_behave(&f->proxy, S5_REPLY, 0x05);
    assert(f->nym->ops->connect_async(f->nym, &target) == 0);
    free(event(f->en, FIPSD_TRANSPORT_DISCONNECTED)); free(receive(f->proxy.requests)); drained(f->nym);
    socks5_fixture_behave(&f->proxy, S5_NO_METHOD, 0);
    assert(f->nym->ops->connect_async(f->nym, &target) == 0);
    free(event(f->en, FIPSD_TRANSPORT_DISCONNECTED)); drained(f->nym);
    s = stats(f->nym); assert(s.socks5_errors == 2 && !s.connect_refused);
    socks5_fixture_behave(&f->proxy, S5_STALL, 0);
    assert(f->nym->ops->connect_async(f->nym, &target) == 0);
    free(event(f->en, FIPSD_TRANSPORT_DISCONNECTED)); drained(f->nym);
    assert(stats(f->nym).connect_timeouts == 1);
    assert(go_channel_receive(f->proxy.stalled, &v) == 0);
    /* The absent client: dials fail as SOCKS5 errors until it appears. */
    assert(f->absent->ops->connect_async(f->absent, &target) == 0);
    free(event(f->ea, FIPSD_TRANSPORT_DISCONNECTED));
    assert(stats(f->absent).socks5_errors == 1);
    socks5_fixture_behave(&f->proxy, S5_RELAY, 0);
    puts("nym: readiness probe, no-auth codec on the wire, frames both ways, socks5_errors, connect deadline: ok");
    assert(go_channel_send(f->done, f) == 0);
}
int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    config();
    fixture f = {.en = go_channel_create(64), .ea = go_channel_create(64), .et = go_channel_create(64), .done = go_channel_create(1)};
    socks5_fixture_start(&f.proxy);
    fipsd_nym_config c; fipsd_nym_config_default(&c); c.socks5_addr = f.proxy.addr; c.connect_timeout_ms = 300;
    f.nym = fipsd_nym_create((fips_transport_id){1}, "nym", &c);
    c.socks5_addr = "127.0.0.1:1"; c.startup_timeout_secs = 0;
    f.absent = fipsd_nym_create((fips_transport_id){2}, "absent", &c);
    fipsd_tcp_config tc; fipsd_tcp_config_default(&tc); tc.bind_addr = "127.0.0.1:0";
    f.tcp = fipsd_tcp_create((fips_transport_id){3}, "tcp", &tc);
    assert(f.nym && f.absent && f.tcp);
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    fipsd_subsystem sub = {.name = "nym-test", .start = start, .stop = stop, .user = &f};
    assert(!fipsd_runtime_register(rt, &sub) && !fipsd_runtime_start(rt));
    assert(fipsd_runtime_spawn(rt, scenario, &f) == 0 && receive(f.done));
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt);
    assert(!stats(f.nym).pool_outbound);
    fipsd_nym_destroy(f.nym); fipsd_nym_destroy(f.absent); fipsd_tcp_destroy(f.tcp);
    socks5_fixture_stop(&f.proxy);
    GoChannel *channels[] = {f.en, f.ea, f.et, f.done};
    for (size_t i = 0; i < 4; ++i) { void *v; while (go_channel_try_receive(channels[i], &v) == 0) free(v); go_channel_unref(channels[i]); }
    return 0;
}
#endif
