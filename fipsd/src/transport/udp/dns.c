#define _POSIX_C_SOURCE 200809L
#include "transport/udp/dns.h"
#include "transport/presence.h"
#include "blocking_executor.h"
#include <netdb.h>
#include <string.h>

typedef struct lookup { char text[257]; int family; bool ok; fipsd_udp_endpoint endpoint; } lookup;
static void *resolve(void *arg)
{
    lookup *q = arg;
    char *host = q->text, *port;
    if (*host == '[') {
        char *end = strchr(host, ']'); if (!end || end[1] != ':') return NULL;
        *end = 0; host++; port = end + 2;
    } else { port = strrchr(host, ':'); if (!port) return NULL; *port++ = 0; }
    if (!*host || !*port) return NULL;
    struct addrinfo hints = {.ai_family = q->family, .ai_socktype = SOCK_DGRAM, .ai_flags = AI_NUMERICSERV};
    struct addrinfo *answer = NULL;
    if (getaddrinfo(host, port, &hints, &answer)) return NULL;
    for (struct addrinfo *a = answer; a; a = a->ai_next) {
        if (a->ai_addrlen > sizeof q->endpoint.address) continue;
        memcpy(&q->endpoint.address, a->ai_addr, a->ai_addrlen); q->endpoint.len = (socklen_t)a->ai_addrlen;
        q->ok = true; break;
    }
    freeaddrinfo(answer); return NULL;
}
void fipsd_udp_dns_init(fipsd_udp_dns *d) { nsync_mu_init(&d->mu); }
bool fipsd_udp_resolve(fipsd_udp_dns *d, const fips_transport_addr *addr, int family, fipsd_udp_endpoint *out)
{
    if (!addr || !addr->len || addr->len > 256 || memchr(addr->bytes, 0, addr->len)) return false;
    lookup q = {.family = family}; memcpy(q.text, addr->bytes, addr->len);
    if (fipsd_udp_endpoint_numeric(q.text, out)) return out->address.ss_family == family;
    uint64_t now = fipsd_transport_now_ms();
    nsync_mu_lock(&d->mu);
    for (size_t i = 0; i < FIPSD_UDP_DNS_ENTRIES; ++i) {
        if (d->entries[i].expires > now && !strcmp(d->entries[i].name, q.text) &&
            d->entries[i].endpoint.address.ss_family == family) {
            *out = d->entries[i].endpoint; nsync_mu_unlock(&d->mu); return true;
        }
    }
    nsync_mu_unlock(&d->mu);
    go_blocking_submit(resolve, &q);
    if (!q.ok) return false;
    nsync_mu_lock(&d->mu);
    fipsd_udp_dns_entry *e = &d->entries[d->next++ % FIPSD_UDP_DNS_ENTRIES];
    memcpy(e->name, addr->bytes, addr->len); e->name[addr->len] = 0;
    e->endpoint = q.endpoint; e->expires = fipsd_transport_now_ms() + 60000;
    nsync_mu_unlock(&d->mu); *out = q.endpoint; return true;
}
