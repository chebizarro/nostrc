#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "node/netmon.h"
#include <arpa/inet.h>
#include <assert.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static fipsd_network_snapshot *sample(bool up, uint8_t ip)
{
    fipsd_network_snapshot *s = calloc(1, sizeof(*s)); assert(s);
    s->interface_count = 1; s->address_count = 1;
    s->interfaces[0] = (fipsd_interface){.name = "test0", .index = 42, .up = up};
    s->addresses[0] = (fipsd_interface_address){.index = 42, .ip = {.bytes = {10, 1, 1, ip}, .width = 4}, .prefix = 24};
    return s;
}
static fipsd_network_snapshot *injected_read(void *arg)
{ void *value; assert(go_channel_receive(arg, &value) == 0); return value; }
static unsigned round_events(fipsd_netmon *m, uint64_t generation, bool backstop)
{
    unsigned kinds = 0;
    for (;;) {
        void *value; assert(go_channel_receive(fipsd_netmon_events(m), &value) == 0);
        fipsd_netmon_event *e = value;
        assert(e->generation == generation && e->backstop == backstop);
        kinds |= 1u << e->kind; bool done = e->kind == FIPSD_NETWORK_REFRESH || e->kind == FIPSD_NETWORK_READ_FAILED;
        free(e); if (done) return kinds;
    }
}
static void injected(void)
{
    GoChannel *wake = go_channel_create(1), *reads = go_channel_create(8), *ticks = go_channel_create(1); assert(wake && reads && ticks);
    assert(go_channel_send(reads, sample(true, 1)) == 0);
    fipsd_netmon_source source = {.injected = true, .wake = wake, .read = injected_read, .user = reads, .ticks = ticks};
    fipsd_netmon *m = fipsd_netmon_create(&source); assert(m);
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.workers = 2; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); assert(rt && fipsd_runtime_start(rt) == 0);
    assert(fipsd_netmon_start(m, rt) == 0 && fipsd_netmon_fd(m) == -1);
    assert(round_events(m, 1, false) == ((1u << FIPSD_INTERFACE_UP) | (1u << FIPSD_INTERFACE_ADDR_CHANGE) | (1u << FIPSD_NETWORK_REFRESH)));
    GoRefPtr retained = fipsd_netmon_snapshot(m); assert(retained.ptr);
    assert(((fipsd_network_snapshot *)retained.ptr)->onlink.count == 1);
    assert(go_channel_send(reads, sample(true, 2)) == 0); assert(go_channel_send(wake, m) == 0);
    assert(round_events(m, 2, false) == ((1u << FIPSD_INTERFACE_ADDR_CHANGE) | (1u << FIPSD_NETWORK_REFRESH)));
    assert(go_channel_send(reads, sample(false, 2)) == 0); assert(go_channel_send(wake, m) == 0);
    assert(round_events(m, 3, false) == ((1u << FIPSD_INTERFACE_DOWN) | (1u << FIPSD_NETWORK_REFRESH)));
    GoRefPtr now = fipsd_netmon_snapshot(m); assert(((fipsd_network_snapshot *)now.ptr)->onlink.count == 0);
    assert(((fipsd_network_snapshot *)retained.ptr)->onlink.count == 1); go_refptr_release(&now);
    fipsd_network_snapshot *bad = sample(true, 9); bad->failed = true;
    assert(go_channel_send(reads, bad) == 0); assert(go_channel_send(wake, m) == 0);
    assert(round_events(m, 4, false) == (1u << FIPSD_NETWORK_READ_FAILED));
    now = fipsd_netmon_snapshot(m); assert(((fipsd_network_snapshot *)now.ptr)->generation == 3);
    go_refptr_release(&now); go_refptr_release(&retained);
    /* Deterministic injected backstop; native smoke separately waits for the real Ticker. */
    assert(go_channel_send(reads, sample(true, 2)) == 0);
    assert(go_channel_send(ticks, m) == 0);
    assert(round_events(m, 4, true) == ((1u << FIPSD_INTERFACE_UP) | (1u << FIPSD_NETWORK_REFRESH)));
    fipsd_netmon_stop(m); fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_netmon_destroy(m); fipsd_runtime_destroy(rt); go_channel_unref(wake); go_channel_unref(reads); go_channel_unref(ticks);
}
#if defined(__APPLE__) || defined(__FreeBSD__)
static void ifconfig_check(const fipsd_network_snapshot *s)
{
    /* Independent native command cross-check: each reported IP and mask must
     * occur in read_system, including loopback. No interface modification. */
    FILE *f = popen("/sbin/ifconfig -a", "r"); assert(f);
    char line[1024], name[64] = ""; size_t matched = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '\t' && line[0] != ' ') {
            char *colon = strchr(line, ':');
            if (colon && (size_t)(colon - line) < sizeof(name)) { memcpy(name, line, (size_t)(colon - line)); name[colon - line] = 0; }
        }
        char ip[256]; unsigned prefix = 0; int family = 0;
        unsigned mask;
        if (sscanf(line, " inet %255s netmask 0x%x", ip, &mask) == 2) {
            family = AF_INET; prefix = (unsigned)__builtin_popcount(mask);
        } else if (sscanf(line, " inet6 %255s prefixlen %u", ip, &prefix) == 2) family = AF_INET6;
        if (!family) continue;
        char *scope = strchr(ip, '%'); if (scope) *scope = 0;
        uint8_t bytes[16] = {0}; assert(inet_pton(family, ip, bytes) == 1);
        uint32_t index = if_nametoindex(name); bool found = false;
        for (size_t i = 0; i < s->address_count; ++i) {
            const fipsd_interface_address *a = &s->addresses[i];
            if (a->index == index && a->ip.width == (family == AF_INET ? 4 : 16) &&
                a->prefix == prefix && !memcmp(a->ip.bytes, bytes, a->ip.width)) found = true;
        }
        if (!found) fprintf(stderr, "ifconfig mismatch: %s %s/%u\n", name, ip, prefix);
        assert(found); ++matched;
    }
    assert(pclose(f) == 0 && matched > 0);
    printf("ifconfig cross-check: %zu address/prefix matches\n", matched);
}
#endif
static void native(void)
{
    fipsd_network_snapshot *s = fipsd_onlink_read_system(); assert(s && !s->failed && !fips_onlink_read_failed(&s->onlink));
    bool loopback = false;
    for (size_t i = 0; i < s->interface_count; ++i) {
        const fipsd_interface *iface = &s->interfaces[i];
        printf("interface %s index=%u up=%d\n", iface->name, iface->index, iface->up);
        if (iface->loopback) loopback = true;
        for (size_t j = 0; j < s->address_count; ++j) if (s->addresses[j].index == iface->index) {
            char ip[INET6_ADDRSTRLEN]; const fipsd_interface_address *a = &s->addresses[j];
            assert(inet_ntop(a->ip.width == 4 ? AF_INET : AF_INET6, a->ip.bytes, ip, sizeof(ip)));
            printf("  %s/%u\n", ip, a->prefix);
        }
    }
    assert(loopback && s->address_count > 0);
#if defined(__APPLE__) || defined(__FreeBSD__)
    assert(if_nametoindex("lo0") && if_nametoindex("en0"));
    ifconfig_check(s);
#endif
    free(s);
    fipsd_netmon *m = fipsd_netmon_create(NULL); assert(m);
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.workers = 2; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); assert(rt && fipsd_runtime_start(rt) == 0);
    assert(fipsd_netmon_start(m, rt) == 0);
    assert(fipsd_netmon_fd(m) >= 0); /* this smoke MUST exercise native backend */
    bool tick = false;
    while (!tick) {
        void *value; assert(go_channel_receive(fipsd_netmon_events(m), &value) == 0);
        fipsd_netmon_event *e = value;
        tick = e->kind == FIPSD_NETWORK_REFRESH && e->backstop; free(e);
    }
    fipsd_netmon_stop(m); fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_netmon_destroy(m); fipsd_runtime_destroy(rt);
    puts("native kernel socket + 1 s backstop: ok");
}
int main(int argc, char **argv)
{ (void)argv; if (argc > 1) native(); else injected(); return 0; }
