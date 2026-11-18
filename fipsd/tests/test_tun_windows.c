/* The Windows TUN backend (tun_windows.c, nostrc-iq1n9.6.2) against a fake
 * Wintun injected through fipsd_tun_ops.wintun: no driver, no elevation.
 * Proves the bring-up order and rows (load, name probes, adapter "FIPS",
 * 2 MiB session, IPv6 wait, /128, MTU, fd00::/8), packets through the real
 * receive thread, GoChannel and doorbell in order (more than the channel
 * holds, so the thread blocks and resumes), writes and a full ring, a parked
 * read ended by gof_io_cancel_fd as the shim stops it, the session ending
 * under a reader, every failure unwinding what it made, and the unchanged
 * IPv6 shim answering an unknown fd00::/8 destination through the device.
 * Waits are channel receives and Win32 events with bounds; no sleeps. */
#include "tun/internal.h"
#include "tun/lifecycle.h"
#include "tun/status.h"
#include "tun/wintun.h"
#include "ipv6/shim.h"
#include "ipv6/icmp.h"
#include "node/context.h"
#include "node/handlers/handlers.h"
#include "transport/loopback.h"
#include "config/config.h"
#include "host_os.h"
#include <fips/proto/framing.h>
#include <libgo/fiber.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>

#define LUID 0x0600000001000000ull
#define FAKE_RX 1024u
#define FAKE_TX 64u
#define WAIT_MS 30000u
typedef struct fake_packet { uint32_t len; uint8_t bytes[2048]; } fake_packet;
typedef struct fake {
    SRWLOCK lock;
    HANDLE read_event, tx_event;
    fake_packet rx[FAKE_RX], tx[FAKE_TX];
    size_t rx_head, rx_tail, tx_count;
    uint32_t pending_tx, rx_end;
    long outstanding;
    bool tx_full, alias_taken, wintun_taken, route_exists;
    uint32_t fail_load, fail_create, fail_session, fail_wait, fail_address;
    char trace[64]; size_t at;
    int adapters, sessions, opened;
    uint16_t name[128], type[128];
    fipsd_winnet_address address; bool has_address;
    fipsd_winnet_route route; bool has_route;
    uint32_t mtu;
} fake;
static void lock(fake *f) { AcquireSRWLockExclusive(&f->lock); }
static void unlock(fake *f) { ReleaseSRWLockExclusive(&f->lock); }
static void trace(fake *f, char c) { lock(f); assert(f->at + 1 < sizeof f->trace); f->trace[f->at++] = c; f->trace[f->at] = 0; unlock(f); }
static uint32_t fk_load(void *u) { fake *f = u; trace(f, 'L'); return f->fail_load; }
static uint32_t fk_open(void *u, const uint16_t *name, void **out)
{
    (void)name; fake *f = u; trace(f, 'O');
    if (!f->wintun_taken) { *out = NULL; return ERROR_NOT_FOUND; }
    ++f->opened; *out = &f->opened; return 0;
}
static size_t wlen(const uint16_t *w) { size_t n = 0; while (w[n]) ++n; return n; }
static uint32_t fk_create(void *u, const uint16_t *name, const uint16_t *type, void **out)
{
    fake *f = u; trace(f, 'C');
    if (f->fail_create) { *out = NULL; return f->fail_create; }
    memcpy(f->name, name, (wlen(name) + 1) * 2); memcpy(f->type, type, (wlen(type) + 1) * 2);
    ++f->adapters; *out = f; return 0;
}
static void fk_close(void *u, void *a) { fake *f = u; trace(f, 'X'); if (a == &f->opened) --f->opened; else { assert(a == f); --f->adapters; } }
static uint64_t fk_luid(void *u, void *a) { assert(a == u); return LUID; }
static uint32_t fk_version(void *u) { (void)u; return 14; }
static uint32_t fk_start(void *u, void *a, uint32_t capacity, void **out)
{
    fake *f = u; trace(f, 'S'); assert(a == f && capacity == FIPSD_WINTUN_RING_CAPACITY);
    if (f->fail_session) { *out = NULL; return f->fail_session; }
    ++f->sessions; *out = &f->sessions; return 0;
}
static void fk_end(void *u, void *s) { fake *f = u; trace(f, 'E'); assert(s == &f->sessions); --f->sessions; }
static void *fk_event(void *u, void *s) { fake *f = u; assert(s == &f->sessions); return f->read_event; }
static uint32_t fk_receive(void *u, void *s, uint8_t **p, uint32_t *size)
{
    fake *f = u; assert(s == &f->sessions);
    lock(f);
    uint32_t code = 0;
    if (f->rx_head < f->rx_tail) { *p = f->rx[f->rx_head].bytes; *size = f->rx[f->rx_head].len; ++f->rx_head; ++f->outstanding; }
    else code = f->rx_end ? f->rx_end : ERROR_NO_MORE_ITEMS;
    unlock(f);
    return code;
}
static void fk_release(void *u, void *s, const uint8_t *p) { (void)s; (void)p; fake *f = u; lock(f); --f->outstanding; unlock(f); }
static uint32_t fk_allocate(void *u, void *s, uint32_t size, uint8_t **p)
{
    fake *f = u; (void)s;
    lock(f);
    uint32_t code = 0;
    if (f->tx_full || f->tx_count == FAKE_TX) code = ERROR_BUFFER_OVERFLOW;
    else { assert(size <= sizeof f->tx[0].bytes); f->pending_tx = size; *p = f->tx[f->tx_count].bytes; }
    unlock(f);
    return code;
}
static void fk_send(void *u, void *s, const uint8_t *p)
{
    fake *f = u; (void)s;
    lock(f); assert(p == f->tx[f->tx_count].bytes); f->tx[f->tx_count++].len = f->pending_tx; unlock(f);
    SetEvent(f->tx_event);
}
static uint32_t fk_alias(void *u, const uint16_t *alias, bool *in_use)
{ (void)alias; fake *f = u; trace(f, 'P'); *in_use = f->alias_taken; return 0; }
static uint32_t fk_wait(void *u, uint64_t luid, uint32_t ms)
{ fake *f = u; trace(f, 'W'); assert(luid == LUID && ms == FIPSD_WINNET_IPV6_WAIT_MS); return f->fail_wait; }
static uint32_t fk_address(void *u, const fipsd_winnet_address *row, bool add)
{
    fake *f = u; trace(f, add ? 'A' : 'a');
    if (add) { if (f->fail_address) return f->fail_address; f->address = *row; f->has_address = true; return 0; }
    assert(!memcmp(row, &f->address, sizeof *row));
    bool had = f->has_address; f->has_address = false; return had ? 0 : ERROR_NOT_FOUND;
}
static uint32_t fk_mtu(void *u, uint64_t luid, uint32_t mtu) { fake *f = u; trace(f, 'M'); assert(luid == LUID); f->mtu = mtu; return 0; }
static uint32_t fk_route(void *u, const fipsd_winnet_route *row, bool add)
{
    fake *f = u; trace(f, add ? 'R' : 'r');
    if (add) { f->route = *row; if (f->route_exists) return ERROR_OBJECT_ALREADY_EXISTS; f->has_route = true; return 0; }
    bool had = f->has_route; f->has_route = false; return had ? 0 : ERROR_NOT_FOUND;
}
static fake *fake_new(void)
{
    fake *f = calloc(1, sizeof *f); assert(f);
    InitializeSRWLock(&f->lock);
    f->read_event = CreateEventW(NULL, FALSE, FALSE, NULL); f->tx_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    assert(f->read_event && f->tx_event);
    return f;
}
static void fake_free(fake *f) { CloseHandle(f->read_event); CloseHandle(f->tx_event); free(f); }
static void push(fake *f, const uint8_t *p, uint32_t len)
{
    lock(f); assert(f->rx_tail < FAKE_RX && len <= sizeof f->rx[0].bytes);
    memcpy(f->rx[f->rx_tail].bytes, p, len); f->rx[f->rx_tail++].len = len; unlock(f);
    SetEvent(f->read_event);
}
static uint32_t pattern(unsigned i, uint8_t *p)
{
    uint32_t len = 60 + i % 200;
    p[0] = 0x60; memcpy(p + 1, &i, sizeof i);
    for (uint32_t j = 5; j < len; ++j) p[j] = (uint8_t)(i * 7 + j);
    return len;
}
static unsigned index_of(void *u, const char *name) { (void)u; return strcmp(name, "fipsW0") ? 0 : 9; }
typedef struct device { fake *f; fipsd_tun_wintun_ops w; fipsd_tun_ops ops; fipsd_tun *t; } device;
static void device_open(device *d, uint16_t mtu, uint32_t transport_mtu)
{
    d->f = fake_new();
    d->w = (fipsd_tun_wintun_ops){.user = d->f, .load = fk_load, .open_adapter = fk_open, .create_adapter = fk_create,
        .close_adapter = fk_close, .adapter_luid = fk_luid, .driver_version = fk_version, .start_session = fk_start,
        .end_session = fk_end, .read_event = fk_event, .receive = fk_receive, .release = fk_release,
        .allocate = fk_allocate, .send = fk_send, .alias_in_use = fk_alias, .ipv6_interface = fk_wait,
        .address = fk_address, .mtu = fk_mtu, .route = fk_route};
    d->ops = *fipsd_tun_system_ops(); /* real Winsock doorbell, gof_read and closesocket */
    d->ops.ifindex = index_of; d->ops.wintun = &d->w;
    fipsd_tun_config cfg = {.name = "fipsW0", .mtu = mtu, .transport_mtu = transport_mtu};
    d->t = fipsd_tun_create(&cfg, &d->ops, NULL); assert(d->t);
}
static void device_close(device *d) { fipsd_tun_destroy(d->t); fake_free(d->f); }
static fips_identity *identity(fips_secp **secp)
{
    uint8_t key[32] = {0}; key[31] = 1;
    assert(fips_identity_init_secp(fipsd_host_os_entropy(), secp) == FIPS_IDENTITY_OK);
    fips_identity *id = NULL;
    assert(fips_identity_from_secret_bytes(*secp, fipsd_host_os_entropy(), key, &id) == FIPS_IDENTITY_OK);
    return id;
}
static gof_socket_t sock(int fd) { return (gof_socket_t)(uintptr_t)(unsigned)fd; }
__attribute__((noinline)) static int fiber_errno(void) { return errno; }

/* ---- fibers ---- */
typedef struct result { ssize_t n; int error; uint8_t bytes[2048]; } result;
typedef struct job { fipsd_tun *t; unsigned count; GoChannel *out; const uint8_t *data; size_t len; } job;
static void reader(void *arg)
{
    job *j = arg;
    for (unsigned i = 0; i < j->count; ++i) {
        result *x = calloc(1, sizeof *x); assert(x);
        x->n = fipsd_tun_read(j->t, x->bytes, sizeof x->bytes);
        x->error = x->n < 0 ? fiber_errno() : 0;
        assert(go_channel_send(j->out, x) == 0);
        if (x->n < 0) break;
    }
}
static void writer(void *arg)
{
    job *j = arg;
    result *x = calloc(1, sizeof *x); assert(x);
    x->n = fipsd_tun_write(j->t, j->data, j->len);
    x->error = x->n < 0 ? fiber_errno() : 0;
    assert(go_channel_send(j->out, x) == 0);
}
static result *next(GoChannel *c) { void *x = NULL; assert(go_channel_receive(c, &x) == 0 && x); return x; }
static result *run(fipsd_runtime *rt, void (*fn)(void *), job *j) { assert(fipsd_runtime_spawn(rt, fn, j) == 0); return next(j->out); }

static void lifecycle_and_frames(fipsd_runtime *rt)
{
    fips_secp *secp = NULL; fips_identity *id = identity(&secp);
    device d; device_open(&d, 1400, 1500);
    assert(fipsd_tun_start(d.t, id) == 0);
    assert(!strcmp(d.f->trace, "LPOCSWAMR")); /* load, alias probe, Wintun probe, create, session, IPv6, /128, MTU, route */
    uint16_t want[16];
    assert(fipsd_winnet_wide("fipsW0", want, 16) == 6 && !memcmp(d.f->name, want, 14));
    assert(fipsd_winnet_wide("FIPS", want, 16) == 4 && !memcmp(d.f->type, want, 10));
    assert(d.f->adapters == 1 && d.f->sessions == 1 && d.f->opened == 0);
    fips_ipv6_address our = fips_identity_address(id);
    assert(d.f->has_address && d.f->address.luid == LUID && !memcmp(d.f->address.address, our.bytes, 16) &&
           d.f->address.prefix_len == 128 && d.f->address.prefix_origin == 1 && d.f->address.suffix_origin == 1);
    static const uint8_t mesh[16] = {0xfd}, any[16] = {0};
    assert(d.f->has_route && d.f->route.luid == LUID && !memcmp(d.f->route.prefix, mesh, 16) && d.f->route.prefix_len == 8 &&
           !memcmp(d.f->route.next_hop, any, 16) && d.f->route.metric == 256 && d.f->route.protocol == 3);
    assert(d.f->mtu == 1400 && fipsd_tun_effective_mtu(d.t) == 1400 && fipsd_tun_wintun_luid(d.t) == LUID);
    fipsd_tun_status s = fipsd_tun_status_read(d.t);
    assert(s.state == FIPSD_TUN_UP && !strcmp(s.name, "fipsW0") && s.index == 9 && s.mtu == 1400 && s.attempts == 1);
    assert(!strcmp(fipsd_tun_name(d.t), "fipsW0"));
    int fd = fipsd_tun_fd(d.t); assert(fd >= 0);
    assert(gof_io_reset_fd(sock(fd)) == 0); /* as the shim does at start */

    /* 1. 300 packets queued before anyone reads: the thread fills the
     *    256-packet channel, blocks, and resumes as the reader drains it. */
    uint8_t p[2048];
    for (unsigned i = 0; i < 300; ++i) push(d.f, p, pattern(i, p));
    job r = {.t = d.t, .count = 300, .out = go_channel_create(512)};
    assert(fipsd_runtime_spawn(rt, reader, &r) == 0);
    for (unsigned i = 0; i < 300; ++i) {
        result *x = next(r.out); uint32_t len = pattern(i, p);
        assert(x->n == (ssize_t)len && !memcmp(x->bytes, p, len)); free(x);
    }
    /* 2. A reader parked on the doorbell before the packets arrive. Each
     *    fiber gets its own job: a finished reader still reads its count. */
    job r2 = {.t = d.t, .count = 3, .out = r.out};
    assert(fipsd_runtime_spawn(rt, reader, &r2) == 0);
    for (unsigned i = 300; i < 303; ++i) push(d.f, p, pattern(i, p));
    for (unsigned i = 300; i < 303; ++i) {
        result *x = next(r.out); uint32_t len = pattern(i, p);
        assert(x->n == (ssize_t)len && !memcmp(x->bytes, p, len)); free(x);
    }
    fipsd_tun_wintun_stats st = fipsd_tun_wintun_stats_read(d.t);
    assert(st.received == 303 && st.dropped == 0 && st.end_code == 0);
    lock(d.f); assert(d.f->outstanding == 0); unlock(d.f); /* every ring slot released */

    /* 3. Writes go out through the session; a full ring is ENOBUFS; over the MTU EMSGSIZE. */
    job w = {.t = d.t, .out = r.out, .data = p};
    for (unsigned i = 0; i < 3; ++i) {
        w.len = pattern(500 + i, p);
        result *x = run(rt, writer, &w); assert(x->n == (ssize_t)w.len); free(x);
        assert(d.f->tx_count == i + 1 && d.f->tx[i].len == w.len && !memcmp(d.f->tx[i].bytes, p, w.len));
    }
    d.f->tx_full = true; w.len = 100;
    result *x = run(rt, writer, &w); assert(x->n == -1 && x->error == ENOBUFS); free(x);
    d.f->tx_full = false; w.len = 1401;
    x = run(rt, writer, &w); assert(x->n == -1 && x->error == EMSGSIZE); free(x);

    /* 4. The shim's stop: gof_io_cancel_fd ends a read parked with no packet. */
    job r3 = {.t = d.t, .count = 1, .out = r.out}, r4 = r3;
    assert(fipsd_runtime_spawn(rt, reader, &r3) == 0);
    assert(gof_io_cancel_fd(sock(fd)) == 0);
    x = next(r.out); assert(x->n == -1 && x->error == ECANCELED); free(x);
    assert(gof_io_reset_fd(sock(fd)) == 0); /* no user left on the doorbell */
    /* ... and after the reset the device reads again. */
    push(d.f, p, pattern(900, p));
    x = run(rt, reader, &r4); assert(x->n == (ssize_t)pattern(900, p) && !memcmp(x->bytes, p, (size_t)x->n)); free(x);

    /* 5. Packets nobody reads are freed at stop; teardown reverses the setup. */
    for (unsigned i = 0; i < 5; ++i) push(d.f, p, pattern(i, p));
    assert(fipsd_tun_stop(d.t) == 0);
    assert(!strcmp(d.f->trace, "LPOCSWAMRraEX"));
    assert(d.f->adapters == 0 && d.f->sessions == 0 && !d.f->has_route && !d.f->has_address && d.f->outstanding == 0);
    s = fipsd_tun_status_read(d.t);
    assert(s.state == FIPSD_TUN_STOPPED && s.error == 0 && fipsd_tun_fd(d.t) == -1);
    /* A stopped device starts again on a new adapter. */
    assert(fipsd_tun_start(d.t, id) == 0 && fipsd_tun_status_read(d.t).state == FIPSD_TUN_UP);
    assert(fipsd_tun_stop(d.t) == 0 && d.f->adapters == 0 && d.f->sessions == 0);
    go_channel_unref(r.out);
    device_close(&d);
    fips_identity_destroy(id); fips_identity_destroy_secp(secp);
}
static void session_end(fipsd_runtime *rt)
{
    fips_secp *secp = NULL; fips_identity *id = identity(&secp);
    device d; device_open(&d, 1280, 1357);
    assert(fipsd_tun_start(d.t, id) == 0);
    job r = {.t = d.t, .count = 2, .out = go_channel_create(4)};
    uint8_t p[2048];
    push(d.f, p, pattern(1, p));
    assert(fipsd_runtime_spawn(rt, reader, &r) == 0);
    result *x = next(r.out); assert(x->n == (ssize_t)pattern(1, p)); free(x);
    /* The adapter goes away under the reader: the ring answers ERROR_HANDLE_EOF. */
    lock(d.f); d.f->rx_end = ERROR_HANDLE_EOF; unlock(d.f); SetEvent(d.f->read_event);
    x = next(r.out); assert(x->n == -1 && x->error == ENODEV); free(x);
    assert(fipsd_tun_wintun_stats_read(d.t).end_code == ERROR_HANDLE_EOF);
    assert(fipsd_tun_stop(d.t) == 0 && d.f->sessions == 0 && d.f->adapters == 0);
    go_channel_unref(r.out);
    device_close(&d);
    fips_identity_destroy(id); fips_identity_destroy_secp(secp);
}
/* Each failure: errno, attempts, the calls made and nothing left behind. */
static void expect_failure(void (*arrange)(fake *), int error, unsigned attempts, const char *calls)
{
    fips_secp *secp = NULL; fips_identity *id = identity(&secp);
    device d; device_open(&d, 1280, 1357);
    arrange(d.f);
    errno = 0;
    assert(fipsd_tun_start(d.t, id) == -1 && errno == error);
    fipsd_tun_status s = fipsd_tun_status_read(d.t);
    if (s.state != FIPSD_TUN_FAILED || s.error != error || s.attempts != attempts || strcmp(d.f->trace, calls)) {
        fprintf(stderr, "state %d error %d attempts %u calls %s (want %d %u %s)\n", s.state, s.error, s.attempts,
            d.f->trace, error, attempts, calls);
        abort();
    }
    assert(d.f->adapters == 0 && d.f->sessions == 0 && d.f->opened == 0 && fipsd_tun_fd(d.t) == -1);
    device_close(&d);
    fips_identity_destroy(id); fips_identity_destroy_secp(secp);
}
static void no_dll(fake *f) { f->fail_load = ERROR_MOD_NOT_FOUND; }
static void alias_taken(fake *f) { f->alias_taken = true; }
static void wintun_taken(fake *f) { f->wintun_taken = true; }
static void denied(fake *f) { f->fail_create = ERROR_ACCESS_DENIED; }
static void no_session(fake *f) { f->fail_session = ERROR_NOT_ENOUGH_MEMORY; }
static void no_ipv6(fake *f) { f->fail_wait = ERROR_TIMEOUT; }
static void bad_address(fake *f) { f->fail_address = ERROR_INVALID_PARAMETER; }
static void failures(void)
{
    expect_failure(no_dll, ENOENT, 3, "LLL");            /* ENOENT: the bounded retry, then visible */
    expect_failure(alias_taken, EEXIST, 1, "LP");        /* never created beside another interface's name */
    expect_failure(wintun_taken, EEXIST, 1, "LPOX");     /* the opened adapter is closed, not removed */
    expect_failure(denied, EPERM, 1, "LPOC");
    expect_failure(no_session, ENOMEM, 1, "LPOCSX");
    expect_failure(no_ipv6, ETIMEDOUT, 1, "LPOCSWEX");
    expect_failure(bad_address, EINVAL, 1, "LPOCSWAEX"); /* the unwind ends the session and removes the adapter */
    /* A route already through this adapter is used, and left at stop. */
    fips_secp *secp = NULL; fips_identity *id = identity(&secp);
    device d; device_open(&d, 1280, 1357); d.f->route_exists = true;
    assert(fipsd_tun_start(d.t, id) == 0 && !d.t->routed);
    assert(fipsd_tun_stop(d.t) == 0 && !strcmp(d.f->trace, "LPOCSWAMRaEX"));
    device_close(&d);
    fips_identity_destroy(id); fips_identity_destroy_secp(secp);
}

/* ---- the IPv6 shim, unchanged, on the Windows device ---- */
static fipsd_network_snapshot *network(void *arg) { (void)arg; return calloc(1, sizeof(fipsd_network_snapshot)); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }
static void shim_on_device(void)
{
    const char *yaml = "node: {identity: {persistent: false}, control: {enabled: false}}\ntun: {enabled: true, name: fipsW0}\n";
    fipsd_config *config = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(config);
    GoSwap *configs = go_swap_create(config, fipsd_config_free); assert(configs);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    fipsd_node_handlers *h = fipsd_node_handlers_create(&hcfg); assert(h);
    fipsd_netmon_source source = {.injected = true, .read = network};
    fipsd_node_options options = {.netmon_source = &source, .workers = 2};
    fipsd_node_handlers_install(h, &options);
    fipsd_node *node = fipsd_node_create(configs, &options); assert(node);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fipsd_transport *transport = fipsd_loopback_transport_create(net, (fips_transport_id){7}, "loop", "loop:a", 1357); assert(transport);
    assert(!fipsd_transport_register(fipsd_node_transports(node), transport));
    device d; device_open(&d, 1280, 1357);
    fipsd_tun_service service; fipsd_tun_service_init(&service, d.t, node);
    fipsd_ipv6_shim_config scfg = {.tun = d.t, .handlers = h, .payload_limit = fips_framing_payload_limit(1357), .firewall_scope_check = true};
    fipsd_ipv6_shim *shim = fipsd_ipv6_shim_create(&scfg); assert(shim);
    fipsd_tun_service_set_shim(&service, shim);
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    assert(!fipsd_runtime_register(rt, fipsd_node_subsystem(node)) && !fipsd_runtime_register(rt, &service.subsystem));
    assert(!fipsd_runtime_start(rt));
    assert(fipsd_tun_status_read(d.t).state == FIPSD_TUN_UP);
    /* The host pings an fd00::/8 address no key names. */
    fips_ipv6_address our = fipsd_tun_address(d.t);
    uint8_t unknown[16] = {0xfd, [12] = 0xde, 0xad, 0xbe, 0xef}, frame[104];
    memset(frame, 0, sizeof frame); frame[0] = 0x60; put16(frame + 4, 64); frame[6] = 58; frame[7] = 64;
    memcpy(frame + 8, our.bytes, 16); memcpy(frame + 24, unknown, 16);
    frame[40] = 128; put16(frame + 44, 0x1234); put16(frame + 46, 1);
    for (size_t i = 48; i < sizeof frame; ++i) frame[i] = (uint8_t)i;
    put16(frame + 42, fipsd_ipv6_icmpv6_checksum(frame + 40, 64, our.bytes, unknown));
    push(d.f, frame, sizeof frame);
    assert(WaitForSingleObject(d.f->tx_event, WAIT_MS) == WAIT_OBJECT_0);
    /* Destination Unreachable (no route) from our own address, quoting the ping. */
    lock(d.f); fake_packet got = d.f->tx[0]; unlock(d.f);
    assert(got.len == 48 + sizeof frame && got.bytes[6] == 58 && got.bytes[40] == 1 && got.bytes[41] == 0);
    assert(!memcmp(got.bytes + 8, our.bytes, 16) && !memcmp(got.bytes + 24, our.bytes, 16) && !memcmp(got.bytes + 48, frame, sizeof frame));
    assert(be16(got.bytes + 42) == fipsd_ipv6_icmpv6_checksum(got.bytes + 40, got.len - 40, our.bytes, our.bytes));
    /* The runtime's stop cancels the shim's parked reader, joins it, then
     * tears the device down; the counters are final once its fibers are joined
     * (the writer counts a write after the session has taken it). */
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_ipv6_shim_counters c = fipsd_ipv6_shim_counters_read(shim);
    assert(c.read == 1 && c.unreachable == 1 && c.written == 1 && c.write_errors == 0);
    assert(!strcmp(d.f->trace + strlen(d.f->trace) - 4, "raEX") && d.f->adapters == 0 && d.f->sessions == 0);
    fipsd_runtime_destroy(rt); fipsd_ipv6_shim_destroy(shim); device_close(&d); fipsd_node_destroy(node);
    fipsd_node_handlers_destroy(h); fipsd_loopback_transport_destroy(transport); fipsd_loopback_network_destroy(net);
    go_swap_free(configs);
}
int main(void)
{
    failures();
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    assert(!fipsd_runtime_start(rt));
    lifecycle_and_frames(rt);
    session_end(rt);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK); fipsd_runtime_destroy(rt);
    shim_on_device();
    puts("Wintun backend on a fake session: bring-up rows, 303 packets in order through the receive thread "
         "(channel backpressure), writes/full ring, cancelled park, session end, failure unwinds, shim unreachable: ok");
    return 0;
}
