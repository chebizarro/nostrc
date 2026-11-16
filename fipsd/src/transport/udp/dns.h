#ifndef FIPSD_UDP_DNS_H
#define FIPSD_UDP_DNS_H
#include "transport/udp/io.h"
#include <nsync.h>
#define FIPSD_UDP_DNS_ENTRIES 256u
typedef struct fipsd_udp_dns_entry {
    char name[257];
    fipsd_udp_endpoint endpoint;
    uint64_t expires;
} fipsd_udp_dns_entry;
typedef struct fipsd_udp_dns {
    nsync_mu mu;
    fipsd_udp_dns_entry entries[FIPSD_UDP_DNS_ENTRIES];
    size_t next;
} fipsd_udp_dns;
void fipsd_udp_dns_init(fipsd_udp_dns *);
/* Numeric fast path; bounded 60 s hostname cache. getaddrinfo always executes
 * on the blocking executor. This is peer DNS, not the Tier B4 .fips listener. */
bool fipsd_udp_resolve(fipsd_udp_dns *, const fips_transport_addr *, int family, fipsd_udp_endpoint *);
#endif
