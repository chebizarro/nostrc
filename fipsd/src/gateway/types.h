#ifndef FIPSD_GATEWAY_TYPES_H
#define FIPSD_GATEWAY_TYPES_H
/* Shared vocabulary of the fips-gateway port (nostrc-iq1n9.3): the clock the
 * pool is driven with, the events the pool emits for the NAT table and the
 * network setup, and the IPv6 helpers every module needs. Pure: no I/O. */
#include <fips/identity/mod.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CLOCK_MONOTONIC nanoseconds, the port of std::time::Instant. */
typedef uint64_t fipsd_gw_instant;
#define FIPSD_GW_SEC UINT64_C(1000000000)
fipsd_gw_instant fipsd_gw_now(void);
/* now plus secs seconds, saturating (upstream `later`). */
fipsd_gw_instant fipsd_gw_later(fipsd_gw_instant now, uint64_t secs);
/* a - b, or 0 when b is later (saturating_duration_since). */
static inline uint64_t fipsd_gw_since(fipsd_gw_instant a, fipsd_gw_instant b) { return a > b ? a - b : 0; }

/* Whether the gateway's socket calls may be on a fiber. libgo's gof_in_fiber
 * reads a thread key its scheduler creates in gof_init, and before then the
 * key aliases whatever another library made first, so it is asked only once
 * the gateway's runtime runs; until then every call is a plain syscall. */
void fipsd_gw_fibers_started(void);
bool fipsd_gw_on_fiber(void);

/* Events the pool emits on state transitions. */
typedef enum fipsd_gw_event_kind {
    FIPSD_GW_EVENT_MAPPING_CREATED, /* NAT rules should be created */
    FIPSD_GW_EVENT_MAPPING_REMOVED  /* NAT rules should be removed */
} fipsd_gw_event_kind;
typedef struct fipsd_gw_pool_event {
    fipsd_gw_event_kind kind;
    struct in6_addr virtual_ip, mesh_addr;
} fipsd_gw_pool_event;
typedef struct fipsd_gw_events { fipsd_gw_pool_event *items; size_t len, cap; } fipsd_gw_events;
int fipsd_gw_events_push(fipsd_gw_events *events, const fipsd_gw_pool_event *event);
void fipsd_gw_events_clear(fipsd_gw_events *events);

/* ---- IPv6 helpers ------------------------------------------------------ */
bool fipsd_gw_ip6_parse(const char *text, struct in6_addr *out);
/* Compressed lowercase text, as Rust's Ipv6Addr Display; buf needs 46 bytes. */
const char *fipsd_gw_ip6_text(const struct in6_addr *addr, char *buf, size_t cap);
static inline bool fipsd_gw_ip6_eq(const struct in6_addr *a, const struct in6_addr *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < 16; ++i) if (x[i] != y[i]) return false;
    return true;
}
int fipsd_gw_ip6_cmp(const struct in6_addr *a, const struct in6_addr *b);
/* "addr/len": the address and the prefix length as written (any u32). */
bool fipsd_gw_cidr_parse(const char *cidr, struct in6_addr *addr, uint32_t *prefix_len);
/* The mask of a prefix length (clamped to 128), the network address of addr
 * under it, and whether addr lies in network/prefix_len. */
void fipsd_gw_ip6_mask(uint32_t prefix_len, struct in6_addr *mask);
void fipsd_gw_ip6_network(const struct in6_addr *addr, uint32_t prefix_len, struct in6_addr *out);
bool fipsd_gw_ip6_in_prefix(const struct in6_addr *addr, const struct in6_addr *network, uint32_t prefix_len);
/* base + offset, as 128-bit arithmetic. */
void fipsd_gw_ip6_add(const struct in6_addr *base, uint32_t offset, struct in6_addr *out);
/* addr - base when it is in 0..=UINT32_MAX (true and *offset), else false. */
bool fipsd_gw_ip6_offset(const struct in6_addr *addr, const struct in6_addr *base, uint32_t *offset);
/* An address in fd00::/8. */
static inline bool fipsd_gw_ip6_is_mesh(const struct in6_addr *addr) { return ((const uint8_t *)addr)[0] == FIPS_IPV6_ADDRESS_PREFIX; }
#endif
