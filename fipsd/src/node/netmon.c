#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "node/netmon.h"
#include "blocking_executor.h"
#include "select.h"
#include "ticker.h"
#include "log.h"
#include <libgo/fiber.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <net/route.h>
#endif

struct fipsd_netmon {
    fipsd_netmon_source source;
    fipsd_runtime *runtime;
    GoSwap *snapshots;
    GoChannel *events, *wake, *stop;
    Ticker *ticker;
    int fd;
    bool started;
};
static const fipsd_interface *find_interface(const fipsd_network_snapshot *s, uint32_t index)
{
    for (size_t i = 0; i < s->interface_count; ++i)
        if (s->interfaces[i].index == index) return &s->interfaces[i];
    return NULL;
}
static int address_order(const void *aa, const void *bb)
{
    const fipsd_interface_address *a = aa, *b = bb;
    if (a->index != b->index) return a->index < b->index ? -1 : 1;
    if (a->ip.width != b->ip.width) return a->ip.width < b->ip.width ? -1 : 1;
    int c = memcmp(a->ip.bytes, b->ip.bytes, a->ip.width);
    return c ? c : (int)a->prefix - (int)b->prefix;
}
bool fipsd_network_snapshot_prepare(fipsd_network_snapshot *s)
{
    if (s->failed || s->interface_count > FIPSD_NETMON_MAX_INTERFACES ||
        s->address_count > FIPSD_NETMON_MAX_ADDRESSES) goto failed;
    for (size_t i = 0; i < s->address_count; ++i)
        if (s->addresses[i].ip.width != 4 && s->addresses[i].ip.width != 16) goto failed;
    qsort(s->addresses, s->address_count, sizeof(*s->addresses), address_order);
    fips_onlink_prefix *raw = calloc(s->address_count ? s->address_count : 1, sizeof(*raw));
    if (!raw) goto failed;
    size_t n = 0;
    for (size_t i = 0; i < s->address_count; ++i) {
        const fipsd_interface_address *a = &s->addresses[i];
        const fipsd_interface *iface = find_interface(s, a->index);
        if (iface && iface->up && !iface->loopback)
            raw[n++] = (fips_onlink_prefix){.net = a->ip, .len = a->prefix,
                .has_index = true, .index = a->index};
    }
    fips_status rc = fips_onlink_from_indexed(&s->onlink, s->storage,
        FIPSD_NETMON_MAX_ADDRESSES, raw, n);
    free(raw);
    if (rc == FIPS_OK) return true;
failed:
    s->failed = true; s->onlink = fips_onlink_failed(); return false;
}
static uint8_t prefix_length(const uint8_t *mask, size_t n)
{
    unsigned bits = 0;
    bool zero = false;
    for (size_t i = 0; i < n; ++i)
        for (unsigned bit = 0; bit < 8; ++bit) {
            bool one = (mask[i] & (0x80u >> bit)) != 0;
            if (one && zero) return 0; /* malformed mask fails closed in core */
            if (one) ++bits; else zero = true;
        }
    return (uint8_t)bits;
}
fipsd_network_snapshot *fipsd_onlink_read_system(void)
{
    fipsd_network_snapshot *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all) != 0) { s->failed = true; goto done; }
    for (const struct ifaddrs *a = all; a; a = a->ifa_next) {
        if (!a->ifa_name) continue;
        uint32_t index = if_nametoindex(a->ifa_name);
        if (!index) { s->failed = true; break; }
        if (!find_interface(s, index)) {
            if (s->interface_count == FIPSD_NETMON_MAX_INTERFACES ||
                strlen(a->ifa_name) >= FIPSD_INTERFACE_NAME_SIZE) { s->failed = true; break; }
            fipsd_interface *iface = &s->interfaces[s->interface_count++];
            strcpy(iface->name, a->ifa_name); iface->index = index;
            iface->up = (a->ifa_flags & IFF_UP) != 0; /* NOT IFF_RUNNING */
            iface->carrier = (a->ifa_flags & IFF_RUNNING) != 0;
            iface->loopback = (a->ifa_flags & IFF_LOOPBACK) != 0;
        }
        if (!a->ifa_addr || !a->ifa_netmask) continue;
        int family = a->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        if (s->address_count == FIPSD_NETMON_MAX_ADDRESSES) { s->failed = true; break; }
        fipsd_interface_address *out = &s->addresses[s->address_count++];
        out->index = index;
        const void *ip, *mask;
        if (family == AF_INET) {
            out->ip.width = 4;
            ip = &((const struct sockaddr_in *)a->ifa_addr)->sin_addr;
            mask = &((const struct sockaddr_in *)a->ifa_netmask)->sin_addr;
        } else {
            out->ip.width = 16;
            ip = &((const struct sockaddr_in6 *)a->ifa_addr)->sin6_addr;
            mask = &((const struct sockaddr_in6 *)a->ifa_netmask)->sin6_addr;
        }
        memcpy(out->ip.bytes, ip, out->ip.width);
        out->prefix = prefix_length(mask, out->ip.width);
#if defined(__APPLE__) || defined(__FreeBSD__)
        /* BSD sockaddr embeds the link-local zone in bytes 2..3. Scope lives
         * in interface index, not in the address bits used by OnLinkPrefixes. */
        if (family == AF_INET6 && out->ip.bytes[0] == 0xfe && (out->ip.bytes[1] & 0xc0) == 0x80)
            out->ip.bytes[2] = out->ip.bytes[3] = 0;
#endif
    }
    freeifaddrs(all);
done:
    (void)fipsd_network_snapshot_prepare(s);
    return s;
}
int fipsd_network_fd_prepare(int fd)
{
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) return -1;
    return 0;
}
int fipsd_network_socket(bool notifications)
{
    int fd = -1;
#if defined(__linux__)
    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct sockaddr_nl address = {0};
    address.nl_family = AF_NETLINK;
    address.nl_groups = notifications ? RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR : 0;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) { close(fd); return -1; }
    if (!notifications && connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int error = errno; close(fd); errno = error; return -1;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__)
    fd = socket(PF_ROUTE, SOCK_RAW, notifications ? 0 : AF_INET6);
#endif
    if (fd >= 0 && fipsd_network_fd_prepare(fd) < 0) {
        close(fd); fd = -1;
    }
    return fd;
}
static bool stopping(fipsd_netmon *m)
{
    return go_channel_is_closed(m->stop) ||
        go_context_is_canceled(fipsd_runtime_context(m->runtime));
}
__attribute__((noinline)) static int io_error(void)
{ return __atomic_load_n(&errno, __ATOMIC_RELAXED); }
static void kernel_reader(void *arg)
{
    fipsd_netmon *m = arg;
    uint8_t bytes[8192];
    while (!stopping(m)) {
        /* Peek with a bounded cancellable wait: gof_read has no cancellation
         * or deadline API. It then consumes the ready socket, as sole reader.
         * This deadline checks cancellation, NOT a sampling cadence. */
        ssize_t n = gof_recvfrom(m->fd, bytes, 1, MSG_PEEK, NULL, NULL, 250);
        if (n < 0) {
            int error = io_error(); /* evaluate TLS after possible fiber migration */
            if (error == ETIMEDOUT || error == EINTR) continue;
            if (error == ENOBUFS) { (void)go_channel_try_send(m->wake, m); continue; }
            break; /* timer remains live if the kernel stream fails */
        }
        if (!n) break;
        n = gof_read(m->fd, bytes, sizeof(bytes));
        if (n <= 0) break;
        (void)go_channel_try_send(m->wake, m); /* coalesce bursts */
    }
}
static bool emit(fipsd_netmon *m, fipsd_netmon_event_kind kind, uint32_t index,
                 uint64_t generation, bool backstop, const fipsd_interface *iface)
{
    fipsd_netmon_event *e = malloc(sizeof(*e));
    if (!e) return false;
    *e = (fipsd_netmon_event){.kind = kind, .index = index, .generation = generation, .backstop = backstop};
    if (iface) memcpy(e->interface_name, iface->name, sizeof e->interface_name - 1);
    void *cancel = NULL;
    GoSelectCase cases[] = {
        {GO_SELECT_RECEIVE, m->stop, NULL, &cancel},
        {GO_SELECT_RECEIVE, go_context_done(fipsd_runtime_context(m->runtime)), NULL, &cancel},
        {GO_SELECT_SEND, m->events, e, NULL}};
    int selected = go_select(cases, 3);
    if (selected != 2) { free(e); return false; }
    return true;
}
static bool addresses_equal(const fipsd_network_snapshot *a,
                            const fipsd_network_snapshot *b, uint32_t index)
{
    size_t i = 0, j = 0;
    for (;;) {
        while (i < a->address_count && a->addresses[i].index != index) ++i;
        while (j < b->address_count && b->addresses[j].index != index) ++j;
        if (i == a->address_count || j == b->address_count)
            return i == a->address_count && j == b->address_count;
        if (address_order(&a->addresses[i++], &b->addresses[j++])) return false;
    }
}
static void *read_snapshot(void *arg)
{
    fipsd_netmon *m = arg;
    return m->source.read ? m->source.read(m->source.user) : fipsd_onlink_read_system();
}
static bool refresh(fipsd_netmon *m, bool backstop)
{
    fipsd_network_snapshot *s = go_blocking_submit(read_snapshot, m);
    if (!s) return emit(m, FIPSD_NETWORK_READ_FAILED, 0, 0, backstop, NULL);
    GoRefPtr old = go_swap_load(m->snapshots);
    const fipsd_network_snapshot *prev = old.ptr;
    s->generation = prev ? prev->generation + 1 : 1;
    if (!fipsd_network_snapshot_prepare(s)) {
        uint64_t gen = s->generation;
        free(s); go_refptr_release(&old);
        /* Retain last good set but emit explicit failure; never synthesize down. */
        return emit(m, FIPSD_NETWORK_READ_FAILED, 0, gen, backstop, NULL);
    }
    bool ok = go_swap_store(m->snapshots, s);
    if (!ok) { free(s); go_refptr_release(&old); return false; }
    for (size_t i = 0; ok && i < s->interface_count; ++i) {
        const fipsd_interface *cur = &s->interfaces[i];
        const fipsd_interface *was = prev ? find_interface(prev, cur->index) : NULL;
        if ((!was || !was->up) && cur->up)
            ok = emit(m, FIPSD_INTERFACE_UP, cur->index, s->generation, backstop, cur);
        if (was && was->up && !cur->up)
            ok = emit(m, FIPSD_INTERFACE_DOWN, cur->index, s->generation, backstop, cur);
        if (ok && (!prev || !addresses_equal(prev, s, cur->index)))
            ok = emit(m, FIPSD_INTERFACE_ADDR_CHANGE, cur->index, s->generation, backstop, cur);
    }
    if (prev) for (size_t i = 0; ok && i < prev->interface_count; ++i) {
        const fipsd_interface *was = &prev->interfaces[i];
        if (!find_interface(s, was->index)) {
            if (was->up) ok = emit(m, FIPSD_INTERFACE_DOWN, was->index, s->generation, backstop, was);
            if (ok) ok = emit(m, FIPSD_INTERFACE_ADDR_CHANGE, was->index, s->generation, backstop, was);
        }
    }
    if (ok) ok = emit(m, FIPSD_NETWORK_REFRESH, 0, s->generation, backstop, NULL);
    go_refptr_release(&old);
    return ok;
}
static void *stop_timer(void *arg) { stop_ticker(arg); return NULL; }
static void monitor(void *arg)
{
    fipsd_netmon *m = arg;
    bool ok = refresh(m, false);
    while (ok && !stopping(m)) {
        void *value = NULL;
        GoSelectCase cases[] = {
            {GO_SELECT_RECEIVE, m->stop, NULL, &value},
            {GO_SELECT_RECEIVE, go_context_done(fipsd_runtime_context(m->runtime)), NULL, &value},
            {GO_SELECT_RECEIVE, m->wake, NULL, &value},
            {GO_SELECT_RECEIVE, m->ticker ? m->ticker->c : m->source.ticks, NULL, &value}};
        int selected = go_select(cases, 4);
        if (selected < 2) break;
        if (selected == 2 && go_channel_is_closed(m->wake)) break;
        ok = refresh(m, selected == 3);
    }
    if (!ok && !stopping(m)) fipsd_runtime_shutdown(m->runtime);
    if (m->ticker) { (void)go_blocking_submit(stop_timer, m->ticker); m->ticker = NULL; }
    go_channel_close(m->events);
}
fipsd_netmon *fipsd_netmon_create(const fipsd_netmon_source *source)
{
    fipsd_netmon *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->fd = -1;
    if (source) {
        m->source = *source;
        if (source->ticks && !source->injected) { free(m); return NULL; }
    }
    m->events = go_channel_create(64); m->stop = go_channel_create(1);
    m->wake = source && source->wake ? go_channel_ref(source->wake) : go_channel_create(1);
    m->snapshots = go_swap_create(NULL, free);
    if (!m->events || !m->stop || !m->wake || !m->snapshots) {
        fipsd_netmon_destroy(m); return NULL;
    }
    return m;
}
int fipsd_netmon_start(fipsd_netmon *m, fipsd_runtime *rt)
{
    if (m->started) return -1;
    m->runtime = rt;
    if (!m->source.ticks) {
        m->ticker = create_ticker(1000);
        if (!m->ticker) return -1;
    }
    if (!m->source.injected) m->fd = fipsd_network_socket(true);
    if (m->fd < 0 && !m->source.injected)
        FIPSD_WARN("netmon", "kernel notification socket unavailable; using 1 s backstop");
    if (fipsd_runtime_spawn(rt, monitor, m) != 0) {
        stop_ticker(m->ticker); m->ticker = NULL;
        if (m->fd >= 0) { close(m->fd); m->fd = -1; }
        return -1;
    }
    m->started = true;
    if (m->fd >= 0 && fipsd_runtime_spawn(rt, kernel_reader, m) != 0)
        FIPSD_WARN("netmon", "kernel reader spawn failed; using 1 s backstop");
    return 0;
}
void fipsd_netmon_stop(fipsd_netmon *m) { if (m && m->stop) go_channel_close(m->stop); }
void fipsd_netmon_destroy(fipsd_netmon *m)
{
    if (!m) return;
    if (m->fd >= 0) close(m->fd);
    if (m->ticker) stop_ticker(m->ticker);
    if (m->events) {
        void *e;
        while (go_channel_try_receive(m->events, &e) == 0) free(e);
        go_channel_unref(m->events);
    }
    if (m->stop) go_channel_unref(m->stop);
    if (m->wake) go_channel_unref(m->wake);
    go_swap_free(m->snapshots); free(m);
}
GoChannel *fipsd_netmon_events(fipsd_netmon *m) { return m->events; }
GoRefPtr fipsd_netmon_snapshot(fipsd_netmon *m) { return go_swap_load(m->snapshots); }
int fipsd_netmon_fd(const fipsd_netmon *m) { return m->fd; }
