#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "transport/presence.h"
#include <net/if.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <net/if_dl.h>
#elif defined(__linux__)
#include <netpacket/packet.h>
#endif
uint64_t fipsd_transport_now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
void fipsd_presence_init(fipsd_presence *p, bool optional, uint64_t now)
{ nsync_mu_init(&p->mu); p->view = (fipsd_presence_view){.optional = optional, .since_ms = now, .interface = {.medium = FIPS_PATH_Unknown}}; }
fipsd_presence_view fipsd_presence_snapshot(fipsd_presence *p)
{ nsync_mu_lock(&p->mu); fipsd_presence_view v = p->view; nsync_mu_unlock(&p->mu); return v; }
static void transition(fipsd_presence_view *v, fipsd_presence_phase phase, uint64_t now)
{
    if ((v->phase == FIPSD_PRESENT) != (phase == FIPSD_PRESENT)) v->since_ms = now;
    v->phase = phase;
}
void fipsd_presence_transition(fipsd_presence *p, fipsd_presence_phase phase, uint64_t now)
{ nsync_mu_lock(&p->mu); transition(&p->view, phase, now); nsync_mu_unlock(&p->mu); }
void fipsd_presence_observe(fipsd_presence *p, fipsd_presence_facts f)
{ nsync_mu_lock(&p->mu); p->view.interface = f; nsync_mu_unlock(&p->mu); }
void fipsd_presence_bound(fipsd_presence *p, fipsd_presence_facts f, uint64_t now)
{
    nsync_mu_lock(&p->mu); p->view.interface = f; p->view.binds++; p->view.attempts = 0;
    transition(&p->view, FIPSD_PRESENT, now); nsync_mu_unlock(&p->mu);
}
uint32_t fipsd_presence_attempt(fipsd_presence *p)
{ nsync_mu_lock(&p->mu); if (p->view.attempts < UINT32_MAX) p->view.attempts++; uint32_t n = p->view.attempts; nsync_mu_unlock(&p->mu); return n; }
uint64_t fipsd_bind_backoff_ms(uint32_t n)
{ unsigned shift = n ? n - 1 : 0; if (shift > 5) shift = 5; uint64_t s = UINT64_C(1) << shift; return (s > 30 ? 30 : s) * 1000; }
const char *fipsd_presence_name(fipsd_presence_phase p)
{ return p == FIPSD_PRESENT ? "present" : p == FIPSD_BINDING ? "binding" : "absent"; }
fipsd_presence_facts fipsd_presence_from_snapshot(const fipsd_network_snapshot *s, const char *name)
{
    fipsd_presence_facts f = {.medium = FIPS_PATH_Unknown};
    if (!s || s->failed || !name) return f;
    f.valid = true;
    for (size_t i = 0; i < s->interface_count; ++i) if (!strcmp(s->interfaces[i].name, name)) {
        const fipsd_interface *in = &s->interfaces[i];
        f.exists = true; f.up = in->up; f.carrier = in->carrier; f.index = in->index; break;
    }
    return f;
}
fipsd_presence_facts fipsd_presence_from_ifaddrs(const struct ifaddrs *ifs, const char *name,
                                               unsigned (*index_of)(const char *))
{
    fipsd_presence_facts f = {.valid = true, .medium = FIPS_PATH_Unknown};
    for (const struct ifaddrs *i = ifs; i; i = i->ifa_next) if (i->ifa_name && !strcmp(i->ifa_name, name)) {
        f.exists = true; f.up = (i->ifa_flags & IFF_UP) != 0;
        f.carrier = (i->ifa_flags & IFF_RUNNING) != 0;
        f.index = index_of(name);
        if (!i->ifa_addr) continue;
#ifdef __APPLE__
        if (i->ifa_addr->sa_family == AF_LINK) {
            const struct sockaddr_dl *a = (const void *)i->ifa_addr;
            if (a->sdl_alen == 6) { memcpy(f.mac, LLADDR(a), 6); f.has_mac = true; }
        }
#elif defined(__linux__)
        if (i->ifa_addr->sa_family == AF_PACKET) {
            const struct sockaddr_ll *a = (const void *)i->ifa_addr;
            if (a->sll_halen == 6) { memcpy(f.mac, a->sll_addr, 6); f.has_mac = true; }
        }
#endif
    }
    return f;
}
fipsd_presence_facts fipsd_presence_read(const char *name)
{
    struct ifaddrs *ifs = NULL;
    if (!name || getifaddrs(&ifs) < 0) return (fipsd_presence_facts){0};
    fipsd_presence_facts f = fipsd_presence_from_ifaddrs(ifs, name, if_nametoindex);
    freeifaddrs(ifs);
    if (f.exists) f.medium = fipsd_medium_read(name);
    return f;
}
