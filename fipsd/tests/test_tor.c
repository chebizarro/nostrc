/* Tor transport (nostrc-gfi8h.4) against loopback fakes: the SOCKS5 proxy
 * stands in for Tor's SocksPort (routing each .onion CONNECT to a local
 * listener, as Tor's rendezvous delivers it to the HiddenServicePort target),
 * the control fixture for its ControlPort. Covers upstream tor/mod.rs and
 * tor/tests/mtu.rs: configuration and modes, stream-isolated SOCKS5 dials to
 * numeric and onion targets, the directory-mode onion listener, links kept
 * up between ends configured with different mtu values (Tor<->Tor both ways,
 * Tor->TCP, TCP->Tor), connect_refused / socks5_errors / connect_timeouts
 * accounting, the inbound cap and first-frame deadline, the show_transports
 * row, and the control-port monitor (cookie and password auth, GETINFO
 * snapshot and per-key fallback, SETEVENTS bootstrap / circuit / liveness /
 * bandwidth events, reconnect with backoff, never ADD_ONION). No sleeps. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "transport/tor/internal.h"
#include "transport/tcp/internal.h"
#include "control/transport.h"
#include "socks5_fixture.h"
#include "torctl_fixture.h"
#include "test_tmpdir.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define ID_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2"
#define ID_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb2"
#define ONION_A ID_A ".onion"
#define ONION_B ID_B ".onion"
#define HSDIR " UNKNOWN $0101010101010101010101010101010101010101~hsdir"
static const char *dir;
static void *receive(GoChannel *channel)
{
    void *v = NULL; GoSelectCase c = {GO_SELECT_RECEIVE, channel, NULL, &v};
    GoSelectResult r = go_select_timeout(&c, 1, 15000); assert(r.selected_case == 0); return v;
}
static void frame(uint8_t *p, size_t n)
{
    memset(p, 0, n); assert(n >= FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE);
    size_t payload = n - FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE;
    p[2] = (uint8_t)payload; p[3] = (uint8_t)(payload >> 8);
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
/* After DISCONNECTED: the pool removal that admits a reconnect to the same
 * endpoint is announced on the owner's change channel, not timed. */
static void drained(fipsd_transport *t)
{
    tcp_instance *u = (fipsd_tor_is(t) ? fipsd_tor_stream(t) : t)->user;
    for (;;) {
        nsync_mu_lock(&u->mu); bool empty = !u->connections; nsync_mu_unlock(&u->mu);
        if (empty) return;
        (void)receive(u->changed);
    }
}
static fips_transport_addr addr_of(const char *s)
{ fips_transport_addr a; assert(fips_transport_addr_from_string(s, strlen(s), &a) == FIPS_OK); return a; }
static fipsd_transport_stats_view stats(const fipsd_transport *t) { return fipsd_transport_stats_read(&t->shared->stats); }
static const char *write_file(const char *name, const void *bytes, size_t n)
{
    static char paths[8][512]; static unsigned next;
    char *path = paths[next++ % 8]; snprintf(path, 512, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb"); assert(f && fwrite(bytes, 1, n, f) == n); fclose(f); return path;
}
static fipsd_tor_config base(const char *mode, const char *proxy, uint16_t mtu)
{
    fipsd_tor_config c; fipsd_tor_config_default(&c);
    c.mode = mode; c.socks5_addr = proxy; c.mtu = mtu; c.control_backoff_ms = 20;
    return c;
}
static fipsd_transport *directory(uint32_t id, const char *name, const char *proxy, uint16_t mtu, const char *onion)
{
    fipsd_tor_config c = base("directory", proxy, mtu);
    char text[128]; int n = snprintf(text, sizeof text, "%s\n", onion);
    char file[64]; snprintf(file, sizeof file, "%s.hostname", name);
    c.hostname_file = write_file(file, text, (size_t)n); c.bind_addr = "127.0.0.1:0";
    fipsd_transport *t = fipsd_tor_create((fips_transport_id){id}, name, &c); assert(t); return t;
}
static void config(void)
{
    const char *yaml = "transports: {tor: {mode: socks5}}";
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    fipsd_transport_registry r = {0}; fipsd_transport *owned[32]; size_t n = 0;
    assert(fipsd_tor_register_config(&r, cfg, owned, &n) == 0 && n == 1);
    tor_instance *x = owned[0]->user;
    assert(!strcmp(owned[0]->type, "tor") && !strcmp(owned[0]->name, "tor") && !owned[0]->accept_connections);
    assert(x->mode == TOR_MODE_SOCKS5 && !strcmp(x->socks5_addr, "127.0.0.1:9050") && x->config.mtu == 1400);
    assert(x->config.connect_timeout_ms == 120000 && !x->has_control && fipsd_tor_is(owned[0]));
    assert(owned[0]->ops->mtu(owned[0], NULL) == 1400 && !strcmp(fipsd_tor_mode(owned[0]), "socks5"));
    fips_transport_addr none; assert(!owned[0]->ops->local_addr(owned[0], &none));
    assert(!fipsd_tor_advertised_onion(owned[0]) && fipsd_tor_advertised_port(owned[0]) == 443);
    fipsd_tor_destroy(owned[0]); fipsd_config_free(cfg);
    yaml = "node: {limits: {max_connections: 9}}\ntransports:\n  tor:\n"
        "    out: {socks5_addr: '127.0.0.1:19050', mtu: 1500, control_addr: '127.0.0.1:9051'}\n"
        "    mon: {mode: control_port}\n"
        "    onion: {mode: directory, control_addr: './ctl.sock', control_auth: 'password:pw', max_inbound_connections: 3,\n"
        "            directory_service: {hostname_file: '/nonexistent/hostname', bind_addr: '127.0.0.1:0'}}\n";
    cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg); n = 0; r = (fipsd_transport_registry){0};
    assert(fipsd_tor_register_config(&r, cfg, owned, &n) == 0 && n == 3);
    for (size_t i = 0; i < n; ++i) {
        x = owned[i]->user;
        assert(x->config.max_outbound_connections == 9);
        if (!strcmp(owned[i]->name, "out")) {
            assert(x->mode == TOR_MODE_SOCKS5 && x->config.mtu == 1500 && !x->has_control); /* socks5 ignores control_addr */
        } else if (!strcmp(owned[i]->name, "mon")) {
            assert(x->mode == TOR_MODE_CONTROL_PORT && x->has_control && !strcmp(x->control.addr, "/run/tor/control"));
            assert(x->control.auth == TOR_CONTROL_COOKIE && !strcmp(x->control.cookie_path, "/var/run/tor/control.authcookie"));
            assert(!owned[i]->accept_connections);
        } else {
            assert(x->mode == TOR_MODE_DIRECTORY && owned[i]->accept_connections && x->has_control);
            assert(x->control.auth == TOR_CONTROL_PASSWORD && !strcmp(x->control.password, "pw"));
            assert(!strcmp(x->hostname_file, "/nonexistent/hostname") && x->config.max_inbound_connections == 3);
            assert(((tcp_instance *)x->tcp->user)->config.max_inbound_connections == 3);
            assert(!fipsd_tor_onion_address(owned[i])); /* read at start, not at create */
        }
        fipsd_tor_destroy(owned[i]);
    }
    fipsd_config_free(cfg);
    const char *invalid[] = {"transports: {tor: {mode: bogus}}",
        "transports: {tor: {mode: socks5, directory_service: {bind_addr: '127.0.0.1:1'}}}",
        "transports: {tor: {mode: socks5, socks5_addr: 'no-port'}}",
        "transports: {tor: {mode: control_port, control_auth: 'kerberos'}}",
        "transports: {tor: {mode: control_port, control_addr: 'not a host'}}"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        cfg = fipsd_config_parse(invalid[i], strlen(invalid[i]), NULL); assert(cfg);
        n = 0; r = (fipsd_transport_registry){0};
        assert(fipsd_tor_register_config(&r, cfg, owned, &n) == -1 && n == 0);
        fipsd_config_free(cfg);
    }
    /* tor/stats.rs TorStatsSnapshot and nym/stats.rs NymStatsSnapshot. */
    static const char *const tor_fields[] = {"packets_sent", "bytes_sent", "packets_recv", "bytes_recv", "send_errors",
        "recv_errors", "mtu_exceeded", "connections_established", "connect_timeouts", "connect_refused", "socks5_errors",
        "connections_accepted", "connections_rejected", "control_errors", "pool_inbound", "pool_outbound"};
    const char *const *fields = fipsd_transport_stats_fields("tor", &n);
    assert(n == 16); for (size_t i = 0; i < n; ++i) assert(!strcmp(fields[i], tor_fields[i]));
    fields = fipsd_transport_stats_fields("nym", &n);
    assert(n == 10 && !strcmp(fields[7], "connections_established") && !strcmp(fields[9], "socks5_errors"));
    puts("config: modes, defaults, named instances, control_port default address, rejected forms, stats schema: ok");
}
typedef struct fixture {
    socks5_fixture proxy;
    fipsd_runtime *rt;
    fipsd_transport *t[8];
    GoChannel *e[8], *done;
    size_t count;
    unsigned mode;
    torctl_fixture *ctl;
} fixture;
static int start(fipsd_subsystem *sub, fipsd_runtime *rt)
{
    fixture *f = sub->user; f->rt = rt;
    for (size_t i = 0; i < f->count; ++i) if (f->t[i]->ops->start_async(f->t[i], rt, NULL, f->e[i])) return -1;
    return 0;
}
static void stop(fipsd_subsystem *sub, fipsd_runtime *rt)
{ (void)rt; fixture *f = sub->user; for (size_t i = 0; i < f->count; ++i) f->t[i]->ops->stop(f->t[i]); }
static bool is_tor(fipsd_transport *t) { return fipsd_tor_is(t); }
static GoChannel *bound(fipsd_transport *t) { return is_tor(t) ? fipsd_tor_bound(t) : fipsd_tcp_bound(t); }
static fips_transport_addr listener(fipsd_transport *t)
{
    fips_transport_addr a;
    assert(is_tor(t) ? fipsd_tor_listener_address(t, &a) : fipsd_tcp_address(t, &a)); return a;
}
static s5_request *request(fixture *f) { return receive(f->proxy.requests); }
/* `from` sends a `len`-byte frame to `to`; `at` receives it on events `e`. */
static fips_transport_addr deliver(fipsd_transport *from, const fips_transport_addr *to, GoChannel *e, size_t len)
{
    uint8_t bytes[1600]; frame(bytes, len);
    assert(from->ops->send(from, to, bytes, len) == 0);
    fipsd_transport_event *p = event(e, FIPSD_TRANSPORT_PACKET);
    assert(p->size == len && !memcmp(p->bytes, bytes, len));
    fips_transport_addr remote = p->address; free(p); return remote;
}
static void dial_and_mtu(fixture *f)
{
    /* A: directory, mtu 1500. B: directory, mtu 1400. C: TCP 1500. D: TCP 1400. */
    fipsd_transport *A = f->t[0], *B = f->t[1], *C = f->t[2], *D = f->t[3];
    fips_transport_addr a_listen = listener(A), b_listen = listener(B), c_addr = listener(C), d_addr = listener(D);
    socks5_fixture_route(&f->proxy, ONION_A, &a_listen); socks5_fixture_route(&f->proxy, ONION_B, &b_listen);
    assert(!strcmp(fipsd_tor_onion_address(A), ONION_A) && !strcmp(fipsd_tor_onion_address(B), ONION_B));
    /* No control monitor: the hostname read at start is the advert input (upstream). */
    void *ready = NULL;
    assert(!strcmp(fipsd_tor_advertised_onion(A), ONION_A) && fipsd_tor_advertised_port(A) == 443);
    assert(go_channel_try_receive(fipsd_tor_advert_updates(A), &ready) == 0 && ready == A->user);
    assert(go_channel_try_receive(fipsd_tor_advert_updates(A), &ready) != 0);
    assert(A->accept_connections && !strcmp(fipsd_tor_mode(A), "directory"));
    /* Numeric target: SOCKS5 ATYP IPv4, isolation credentials fips / peer. */
    assert(A->ops->connect_async(A, &d_addr) == 0);
    free(event(f->e[0], FIPSD_TRANSPORT_CONNECTED));
    s5_request *r = request(f);
    const char *text; size_t n; assert(fips_transport_addr_as_str(&d_addr, &text, &n) == FIPS_OK);
    assert(r->userpass && !strcmp(r->username, "fips") && strlen(r->password) == n && !memcmp(r->password, text, n));
    assert(r->atyp == 1 && !strcmp(r->host, "127.0.0.1")); free(r);
    /* Control half of the mixed link: TCP 1400 listener takes a 1500 frame from a Tor 1500 dialer. */
    fips_transport_addr from_a = deliver(A, &d_addr, f->e[3], 1400);
    (void)deliver(A, &d_addr, f->e[3], 1500);
    (void)deliver(D, &from_a, f->e[0], 114);
    assert(!stats(D).recv_errors);
    /* Onion target: ATYP domain, the HiddenServicePort virtual port. */
    char onion_b[96]; snprintf(onion_b, sizeof onion_b, "%s:443", ONION_B);
    fips_transport_addr to_b = addr_of(onion_b);
    assert(A->ops->connect_async(A, &to_b) == 0);
    free(event(f->e[0], FIPSD_TRANSPORT_CONNECTED));
    r = request(f);
    assert(r->atyp == 3 && !strcmp(r->host, ONION_B) && r->port == 443 && !strcmp(r->password, onion_b)); free(r);
    /* Tor 1500 -> Tor 1400 onion listener: delivered, no receive error, connection kept. */
    fips_transport_addr a_at_b = deliver(A, &to_b, f->e[1], 1400), again = deliver(A, &to_b, f->e[1], 1500);
    assert(fips_transport_addr_equal(&a_at_b, &again));
    (void)deliver(A, &to_b, f->e[1], 114); /* the same connection carries on */
    assert(!stats(B).recv_errors && fipsd_tor_connected_count(B) == 1);
    assert(stats(B).connections_accepted == 1 && stats(B).pool_inbound == 1);
    /* Tor 1400 dialler <- Tor 1500 onion listener's answer on the accepted connection. */
    char onion_a[96]; snprintf(onion_a, sizeof onion_a, "%s:443", ONION_A);
    fips_transport_addr to_a = addr_of(onion_a);
    assert(B->ops->connect_async(B, &to_a) == 0);
    free(event(f->e[1], FIPSD_TRANSPORT_CONNECTED)); free(request(f));
    fips_transport_addr b_at_a = deliver(B, &to_a, f->e[0], 1400);
    (void)deliver(A, &b_at_a, f->e[1], 1400);
    (void)deliver(A, &b_at_a, f->e[1], 1500);
    (void)deliver(A, &b_at_a, f->e[1], 114);
    assert(!stats(B).recv_errors);
    /* Tor 1400 dialler <- TCP 1500 listener's answer. */
    assert(B->ops->connect_async(B, &c_addr) == 0);
    free(event(f->e[1], FIPSD_TRANSPORT_CONNECTED)); free(request(f));
    fips_transport_addr b_at_c = deliver(B, &c_addr, f->e[2], 1400);
    (void)deliver(C, &b_at_c, f->e[1], 1500);
    (void)deliver(C, &b_at_c, f->e[1], 114);
    assert(!stats(B).recv_errors && !stats(A).recv_errors);
    /* check_mtu: a frame above the local mtu is refused before queueing. */
    uint8_t big[1600]; frame(big, 1501);
    assert(A->ops->send(A, &to_b, big, 1501) == -1 && stats(A).mtu_exceeded == 1);
    frame(big, 1401); assert(B->ops->send(B, &to_a, big, 1401) == -1 && stats(B).mtu_exceeded == 1);
    fipsd_transport_stats_view sa = stats(A);
    assert(sa.connections_established == 2 && sa.pool_outbound == 2 && sa.connections_accepted == 1 && sa.pool_inbound == 1);
    assert(!sa.connect_refused && !sa.socks5_errors && !sa.connect_timeouts && !sa.control_errors);
    /* show_transports: upstream's Tor row fields, no local_addr, the 16 TorStatsSnapshot counters. */
    fipsd_control_transport_view view; fipsd_control_transport_read(A, 0, &view);
    assert(!strcmp(view.row.type, "tor") && !strcmp(view.row.tor_mode, "directory") && !strcmp(view.row.onion_address, ONION_A));
    assert(!view.row.local_addr && !view.row.tor_monitoring && !strcmp(view.row.state, "up"));
    puts("dial: isolated SOCKS5 to numeric and onion targets, onion listener, mixed mtu both ways and with TCP, check_mtu, row: ok");
    A->ops->close_connection(A, &to_b);
    free(event(f->e[1], FIPSD_TRANSPORT_DISCONNECTED));
}
static void errors(fixture *f)
{
    /* E and F keep the 120 s default so a slow handshake is never a timeout;
     * only S, the stall case, has a short connect deadline. */
    fipsd_transport *E = f->t[0], *F = f->t[1], *T = f->t[2], *S = f->t[3];
    fips_transport_addr target = listener(T), lazy = addr_of("localhost:80");
    assert(E->ops->connect_async(E, &lazy) == -1); /* tor requires a fully qualified hostname */
    uint8_t msg[114] = {1, 0, 110, 0};
    assert(E->ops->send(E, &target, msg, sizeof msg) == -1 && !stats(E).send_errors); /* NotConnected */
    socks5_fixture_behave(&f->proxy, S5_REPLY, 0x05);
    assert(E->ops->connect_async(E, &target) == 0);
    free(event(f->e[0], FIPSD_TRANSPORT_DISCONNECTED)); free(request(f)); drained(E);
    assert(stats(E).connect_refused == 1 && !stats(E).socks5_errors);
    socks5_fixture_behave(&f->proxy, S5_REPLY, 0x01);
    assert(E->ops->connect_async(E, &target) == 0);
    free(event(f->e[0], FIPSD_TRANSPORT_DISCONNECTED)); free(request(f)); drained(E);
    assert(stats(E).socks5_errors == 1 && stats(E).connect_refused == 1);
    socks5_fixture_behave(&f->proxy, S5_AUTH_FAIL, 0);
    assert(E->ops->connect_async(E, &target) == 0);
    free(event(f->e[0], FIPSD_TRANSPORT_DISCONNECTED)); drained(E);
    assert(stats(E).socks5_errors == 2);
    socks5_fixture_behave(&f->proxy, S5_STALL, 0);
    assert(S->ops->connect_async(S, &target) == 0);
    free(event(f->e[3], FIPSD_TRANSPORT_DISCONNECTED)); drained(S);
    assert(stats(S).connect_timeouts == 1 && !stats(S).socks5_errors && !stats(S).connect_refused);
    void *v; assert(go_channel_receive(f->proxy.stalled, &v) == 0);
    assert(!stats(E).connect_timeouts && stats(E).socks5_errors == 2);
    /* An unreachable proxy is a SOCKS5 error, never the peer refusing. */
    assert(F->ops->connect_async(F, &target) == 0);
    free(event(f->e[1], FIPSD_TRANSPORT_DISCONNECTED));
    assert(stats(F).socks5_errors == 1 && !stats(F).connect_refused && !stats(F).connect_timeouts);
    fipsd_transport_stats_view se = stats(E);
    assert(!se.connections_established && !se.pool_outbound && !se.send_errors);
    socks5_fixture_behave(&f->proxy, S5_RELAY, 0);
    puts("errors: REP 0x05 refused, general failure, auth failure, stalled proxy deadline, proxy down, NotConnected: ok");
}
static int raw_connect(const fips_transport_addr *to)
{
    tcp_endpoint *items = NULL; size_t n = 0;
    assert(tcp_resolve(to, true, &items, &n));
    int fd = socket(items[0].address.ss_family, SOCK_STREAM, IPPROTO_TCP); assert(fd >= 0);
    assert(!fcntl(fd, F_SETFL, O_NONBLOCK) && gof_io_reset_fd(fd) == 0);
    assert(gof_connect(fd, (struct sockaddr *)&items[0].address, items[0].len, 5000) == 0);
    free(items); return fd;
}
static void inbound(fixture *f)
{
    /* G: max_inbound_connections 1 with the default 30 s first-frame
     * deadline, so the held slot cannot expire under the cap check. */
    fipsd_transport *G = f->t[0], *G2 = f->t[1];
    fips_transport_addr g = listener(G), g2 = listener(G2);
    int held = raw_connect(&g), rejected = raw_connect(&g); uint8_t byte;
    assert(gof_read(rejected, &byte, 1) <= 0); tcp_fd_close(rejected);
    assert(stats(G).connections_rejected == 1 && stats(G).pool_inbound == 1);
    tcp_fd_close(held);
    free(event(f->e[0], FIPSD_TRANSPORT_DISCONNECTED)); drained(G);
    int next = raw_connect(&g);
    uint8_t msg[114] = {1, 0, 110, 0};
    assert(gof_write(next, msg, sizeof msg) == sizeof msg);
    free(event(f->e[0], FIPSD_TRANSPORT_PACKET));
    fipsd_transport_stats_view s = stats(G);
    assert(s.connections_rejected == 1 && s.connections_accepted == 2 && s.pool_inbound == 1 && !s.recv_errors);
    tcp_fd_close(next);
    free(event(f->e[0], FIPSD_TRANSPORT_DISCONNECTED));
    /* G2: a silent onion connection loses its slot at the first-frame deadline. */
    int silent = raw_connect(&g2);
    free(event(f->e[1], FIPSD_TRANSPORT_DISCONNECTED));
    assert(gof_read(silent, &byte, 1) <= 0); tcp_fd_close(silent); drained(G2);
    assert(atomic_load(&G2->shared->diagnostics.deadlines) == 1 && !stats(G2).recv_errors && !stats(G2).pool_inbound);
    /* Directory mode needs Tor's hostname file, holding an onion name. */
    fipsd_tor_config c = base("directory", f->proxy.addr, 1400);
    c.hostname_file = "/nonexistent/fips/hostname"; c.bind_addr = "127.0.0.1:0";
    fipsd_transport *missing = fipsd_tor_create((fips_transport_id){10}, "missing", &c); assert(missing);
    c.hostname_file = write_file("bad.hostname", "not-an-onion\n", 13);
    fipsd_transport *bad = fipsd_tor_create((fips_transport_id){11}, "bad", &c); assert(bad);
    assert(missing->ops->start_async(missing, f->rt, NULL, f->e[4]) == -1);
    assert(bad->ops->start_async(bad, f->rt, NULL, f->e[5]) == -1);
    f->t[4] = missing; f->t[5] = bad; /* destroyed after the runtime joins */
    puts("onion listener: inbound cap, first-frame deadline releases the slot, hostname file required and checked: ok");
}
/* Command lines in order until one starts with `prefix`; never ADD_ONION. */
static char *expect_line(torctl_fixture *c, const char *prefix)
{
    for (;;) {
        char *line = receive(c->commands); assert(line);
        assert(strncmp(line, "ADD_ONION", 9) && strncmp(line, "DEL_ONION", 9));
        if (!strncmp(line, prefix, strlen(prefix))) return line;
        free(line);
    }
}
static void expect_command(torctl_fixture *c, const char *prefix) { free(expect_line(c, prefix)); }
static fipsd_tor_monitoring until(fipsd_transport *t, bool (*ok)(const fipsd_tor_monitoring *))
{
    fipsd_tor_monitoring m;
    while (!fipsd_tor_monitoring_snapshot(t, &m) || !ok(&m)) (void)receive(fipsd_tor_monitoring_updates(t));
    return m;
}
static bool half(const fipsd_tor_monitoring *m) { return m->bootstrap == 50; }
static bool done(const fipsd_tor_monitoring *m) { return m->bootstrap == 100; }
static bool circuit(const fipsd_tor_monitoring *m) { return m->circuit_established; }
static bool down(const fipsd_tor_monitoring *m) { return !strcmp(m->network_liveness, "down"); }
static bool bw(const fipsd_tor_monitoring *m) { return m->traffic_read == 1048576 + 100; }
static bool any(const fipsd_tor_monitoring *m) { return m->version[0] != 0; }
static void control(fixture *f)
{
    torctl_fixture *ctl = calloc(5, sizeof *ctl); assert(ctl);
    torctl_fixture_start(&ctl[0], NULL);
    fipsd_tor_config c = base("directory", f->proxy.addr, 1400);
    char text[80]; int n = snprintf(text, sizeof text, "%s\n", ONION_A);
    c.hostname_file = write_file("mon.hostname", text, (size_t)n); c.bind_addr = "127.0.0.1:0";
    c.control_addr = ctl[0].addr; c.control_auth = "password:test\"pa\\ss";
    fipsd_transport *J = fipsd_tor_create((fips_transport_id){20}, "mon", &c); assert(J);
    GoChannel *ej = f->e[2], *ek = f->e[3], *el = f->e[4], *em = f->e[5];
    assert(J->ops->start_async(J, f->rt, NULL, ej) == 0);
    expect_command(&ctl[0], "AUTHENTICATE \"test\\\"pa\\\\ss\"");
    expect_command(&ctl[0], "GETINFO status/bootstrap-phase status/circuit-established traffic/read traffic/written network-liveness version dormant");
    char *line = expect_line(&ctl[0], "SETEVENTS"); /* an onion is served: HS_DESC too */
    assert(!strcmp(line, "SETEVENTS STATUS_CLIENT NETWORK_LIVENESS BW HS_DESC")); free(line);
    expect_command(&ctl[0], "GETINFO hs/service/desc/id/" ID_A); /* answered 552: nothing published yet */
    fipsd_tor_monitoring m = until(J, any);
    assert(m.bootstrap == 10 && !m.circuit_established && !strcmp(m.version, "0.4.8.10") && !strcmp(m.network_liveness, "up"));
    /* The advert onion waits for Tor's own upload of this service's descriptor:
     * other onions, other actions and longer addresses are not it. */
    GoChannel *ready = fipsd_tor_advert_updates(J); void *v = NULL;
    assert(!fipsd_tor_advertised_onion(J) && !strcmp(fipsd_tor_onion_address(J), ONION_A));
    torctl_fixture_event(&ctl[0], "HS_DESC UPLOADED " ID_B HSDIR);
    torctl_fixture_event(&ctl[0], "HS_DESC RECEIVED " ID_A " NO_AUTH $0101010101010101010101010101010101010101~hsdir");
    torctl_fixture_event(&ctl[0], "HS_DESC UPLOAD " ID_A HSDIR " ABCDEF HSDIR_INDEX=00");
    torctl_fixture_event(&ctl[0], "HS_DESC UPLOADED " ID_A "x" HSDIR);
    torctl_fixture_event(&ctl[0], "STATUS_CLIENT NOTICE BOOTSTRAP PROGRESS=50 TAG=loading_descriptors SUMMARY=\"Loading relay descriptors\"");
    (void)until(J, half); /* every earlier line was handled */
    assert(!fipsd_tor_advertised_onion(J) && go_channel_try_receive(ready, &v) != 0);
    torctl_fixture_event(&ctl[0], "HS_DESC UPLOADED " ID_A HSDIR);
    assert(receive(ready) == J->user && !strcmp(fipsd_tor_advertised_onion(J), ONION_A));
    torctl_fixture_event(&ctl[0], "HS_DESC UPLOADED " ID_A " UNKNOWN $0202020202020202020202020202020202020202~other");
    torctl_fixture_event(&ctl[0], "STATUS_CLIENT NOTICE BOOTSTRAP PROGRESS=100 TAG=done SUMMARY=\"Done\"");
    (void)until(J, done);
    assert(go_channel_try_receive(ready, &v) != 0); /* one notification for the latch, not one per HSDir */
    torctl_fixture_event(&ctl[0], "STATUS_CLIENT NOTICE CIRCUIT_ESTABLISHED");
    (void)until(J, circuit);
    torctl_fixture_event(&ctl[0], "NETWORK_LIVENESS DOWN");
    (void)until(J, down);
    torctl_fixture_event(&ctl[0], "BW 100 200");
    m = until(J, bw); assert(m.traffic_written == 524288 + 200 && m.bootstrap == 100);
    fipsd_control_transport_view view; fipsd_control_transport_read(J, 0, &view);
    assert(view.tor_monitoring && view.row.tor_monitoring == view.tor_monitoring);
    json_t *tm = view.tor_monitoring;
    assert(json_object_size(tm) == 7 && json_integer_value(json_object_get(tm, "bootstrap")) == 100);
    assert(json_is_true(json_object_get(tm, "circuit_established")) && !strcmp(json_string_value(json_object_get(tm, "network_liveness")), "down"));
    assert(json_integer_value(json_object_get(tm, "traffic_read")) == 1048676 && json_is_false(json_object_get(tm, "dormant")));
    json_decref(view.tor_monitoring);
    /* A lost control connection is a control error and reconnects with backoff. */
    torctl_fixture_drop(&ctl[0]);
    expect_command(&ctl[0], "AUTHENTICATE");
    expect_command(&ctl[0], "SETEVENTS");
    assert(stats(J).control_errors == 1);
    puts("monitor: password auth, GETINFO snapshot, SETEVENTS bootstrap/circuit/liveness/bandwidth events, row, reconnect: ok");
    /* control_port mode: cookie auth over the Unix socket, fatal at start. */
    uint8_t cookie[32]; for (size_t i = 0; i < 32; ++i) cookie[i] = (uint8_t)(0xa0 + i);
    char sock[512]; snprintf(sock, sizeof sock, "%s/control.sock", dir);
    ctl[1].use_cookie = true; memcpy(ctl[1].cookie, cookie, 32);
    torctl_fixture_start(&ctl[1], sock);
    fipsd_tor_config k = base("control_port", f->proxy.addr, 1400);
    char cookie_auth[600]; snprintf(cookie_auth, sizeof cookie_auth, "cookie:%s", write_file("cookie", cookie, 32));
    k.control_addr = ctl[1].addr; k.control_auth = cookie_auth;
    fipsd_transport *K = fipsd_tor_create((fips_transport_id){21}, "ctl", &k); assert(K);
    assert(K->ops->start_async(K, f->rt, NULL, ek) == 0);
    assert(fipsd_tor_monitoring_snapshot(K, &m) && m.bootstrap == 10); /* taken before start returned */
    line = expect_line(&ctl[1], "SETEVENTS"); assert(!strstr(line, "HS_DESC")); free(line); /* no onion here */
    pthread_mutex_lock(&ctl[1].mu); unsigned authenticated = ctl[1].authenticated; pthread_mutex_unlock(&ctl[1].mu);
    assert(!K->accept_connections && !fipsd_tor_onion_address(K) && !fipsd_tor_advertised_onion(K) && authenticated == 1);
    /* Refused authentication fails a control_port start. */
    ctl[2].reject_auth = true; torctl_fixture_start(&ctl[2], NULL);
    k.control_addr = ctl[2].addr; k.control_auth = "password:wrong";
    fipsd_transport *L = fipsd_tor_create((fips_transport_id){22}, "refused", &k); assert(L);
    assert(L->ops->start_async(L, f->rt, NULL, el) == -1 && stats(L).control_errors == 1);
    /* A daemon refusing the batch: one GETINFO per key, unanswered keys keep defaults. */
    ctl[3].refuse_batch = true;
    char late[512]; snprintf(late, sizeof late, "%s/late.sock", dir);
    c.control_addr = late; c.control_auth = "password:test\"pa\\ss";
    c.hostname_file = write_file("late.hostname", text, (size_t)n);
    fipsd_transport *M = fipsd_tor_create((fips_transport_id){23}, "late", &c); assert(M);
    /* The control socket does not exist yet: the optional monitor retries
     * with backoff and connects once Tor's socket appears. */
    assert(M->ops->start_async(M, f->rt, NULL, em) == 0);
    /* The first attempt must really find no socket: it counts control_errors
     * and, with no session to confirm the descriptor, latches the hostname-file
     * advert, in that order. Starting the fixture before that event let a slow
     * monitor fiber connect first and never fail. */
    assert(receive(fipsd_tor_advert_updates(M)) == M->user && stats(M).control_errors >= 1);
    torctl_fixture_start(&ctl[3], late);
    expect_command(&ctl[3], "GETINFO status/bootstrap-phase status/circuit-established");
    expect_command(&ctl[3], "GETINFO version");
    expect_command(&ctl[3], "SETEVENTS");
    m = until(M, any);
    assert(!strcmp(m.version, "0.4.8.10") && m.bootstrap == 10 && !m.dormant && stats(M).control_errors >= 1);
    puts("monitor: control_port cookie auth on a Unix socket, refused auth fatal, late socket backoff, per-key GETINFO fallback: ok");
    /* A monitor (re)subscribing to a Tor that already holds the service's
     * current descriptor (fipsd restarted under a running tor) need not wait
     * for the next upload. */
    ctl[4].descriptor = true; torctl_fixture_start(&ctl[4], NULL);
    char text_b[80]; int nb = snprintf(text_b, sizeof text_b, "%s\n", ONION_B);
    c.control_addr = ctl[4].addr; c.hostname_file = write_file("current.hostname", text_b, (size_t)nb);
    fipsd_transport *P = fipsd_tor_create((fips_transport_id){24}, "current", &c); assert(P);
    assert(P->ops->start_async(P, f->rt, NULL, f->e[6]) == 0);
    expect_command(&ctl[4], "GETINFO hs/service/desc/id/" ID_B);
    assert(receive(fipsd_tor_advert_updates(P)) == P->user && !strcmp(fipsd_tor_advertised_onion(P), ONION_B));
    /* A configured monitor that never gets a first session cannot confirm
     * anything: the hostname file suffices, as without a monitor (upstream). */
    char absent[512]; snprintf(absent, sizeof absent, "%s/absent.sock", dir);
    c.control_addr = absent; c.hostname_file = write_file("absent.hostname", text, (size_t)n);
    fipsd_transport *N = fipsd_tor_create((fips_transport_id){25}, "absent", &c); assert(N);
    assert(N->ops->start_async(N, f->rt, NULL, f->e[7]) == 0);
    assert(receive(fipsd_tor_advert_updates(N)) == N->user && !strcmp(fipsd_tor_advertised_onion(N), ONION_A));
    assert(stats(N).control_errors >= 1);
    puts("advert onion: HS_DESC UPLOADED for this service only, latched once; current descriptor at subscribe; unmonitored fallback: ok");
    J->ops->stop(J); K->ops->stop(K); L->ops->stop(L); M->ops->stop(M); P->ops->stop(P); N->ops->stop(N);
    /* Destroyed after the runtime joins; the fixtures outlive the monitors. */
    f->t[2] = J; f->t[3] = K; f->t[4] = L; f->t[5] = M; f->t[6] = P; f->t[7] = N; f->ctl = ctl;
}
static void run(void *arg)
{
    fixture *f = arg;
    for (size_t i = 0; i < f->count; ++i) assert(receive(bound(f->t[i])));
    switch (f->mode) {
    case 0: dial_and_mtu(f); break;
    case 1: errors(f); break;
    case 2: inbound(f); break;
    default: control(f); break;
    }
    assert(go_channel_send(f->done, f) == 0);
}
static void lifecycle(unsigned mode)
{
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    fixture f = {.mode = mode, .done = go_channel_create(1)};
    socks5_fixture_start(&f.proxy);
    for (size_t i = 0; i < 8; ++i) { f.e[i] = go_channel_create(64); assert(f.e[i]); }
    fipsd_tcp_config tc; fipsd_tcp_config_default(&tc); tc.bind_addr = "127.0.0.1:0";
    if (mode == 0) {
        f.t[0] = directory(1, "a", f.proxy.addr, 1500, ONION_A);
        f.t[1] = directory(2, "b", f.proxy.addr, 1400, ONION_B);
        tc.mtu = 1500; f.t[2] = fipsd_tcp_create((fips_transport_id){3}, "c", &tc);
        tc.mtu = 1400; f.t[3] = fipsd_tcp_create((fips_transport_id){4}, "d", &tc);
        f.count = 4;
    } else if (mode == 1) {
        fipsd_tor_config c = base("socks5", f.proxy.addr, 1400);
        f.t[0] = fipsd_tor_create((fips_transport_id){1}, "e", &c);
        c.connect_timeout_ms = 300; f.t[3] = fipsd_tor_create((fips_transport_id){4}, "s", &c);
        c.connect_timeout_ms = 120000; c.socks5_addr = "127.0.0.1:1"; f.t[1] = fipsd_tor_create((fips_transport_id){2}, "f", &c);
        f.t[2] = fipsd_tcp_create((fips_transport_id){3}, "t", &tc);
        f.count = 4;
    } else if (mode == 2) {
        fipsd_tor_config c = base("directory", f.proxy.addr, 1400);
        c.hostname_file = write_file("g.hostname", ONION_A "\n", strlen(ONION_A) + 1); c.bind_addr = "127.0.0.1:0";
        c.max_inbound_connections = 1;
        f.t[0] = fipsd_tor_create((fips_transport_id){1}, "g", &c);
        c.max_inbound_connections = 64; c.first_frame_ms = 200;
        c.hostname_file = write_file("g2.hostname", ONION_B "\n", strlen(ONION_B) + 1);
        f.t[1] = fipsd_tor_create((fips_transport_id){2}, "g2", &c);
        f.count = 2;
    } else {
        fipsd_tor_config c = base("socks5", f.proxy.addr, 1400);
        f.t[0] = fipsd_tor_create((fips_transport_id){1}, "p", &c);
        f.t[1] = fipsd_tcp_create((fips_transport_id){2}, "q", &tc);
        f.count = 2;
    }
    for (size_t i = 0; i < f.count; ++i) assert(f.t[i]);
    fipsd_subsystem sub = {.name = "tor-test", .start = start, .stop = stop, .user = &f};
    assert(!fipsd_runtime_register(rt, &sub) && !fipsd_runtime_start(rt));
    assert(fipsd_runtime_spawn(rt, run, &f) == 0);
    void *result = receive(f.done); assert(result);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt);
    size_t total = mode <= 1 ? 4 : mode == 2 ? 6 : 8;
    for (size_t i = 0; i < total; ++i) {
        fipsd_transport *t = f.t[i]; if (!t) continue;
        assert(!atomic_load(&t->shared->stats.pool_outbound) && !atomic_load(&t->shared->stats.pool_inbound));
        if (is_tor(t)) fipsd_tor_destroy(t); else fipsd_tcp_destroy(t);
    }
    if (f.ctl) { for (size_t i = 0; i < 5; ++i) torctl_fixture_stop(&f.ctl[i]); free(f.ctl); }
    socks5_fixture_stop(&f.proxy);
    for (size_t i = 0; i < 8; ++i) { void *v; while (go_channel_try_receive(f.e[i], &v) == 0) free(v); go_channel_unref(f.e[i]); }
    go_channel_unref(f.done);
}
int main(void)
{
    dir = fipsd_test_tmpdir("/control.sock");
    config(); lifecycle(0); lifecycle(1); lifecycle(2); lifecycle(3);
    return 0;
}
