#include "ipv6/mss.h"
#include "ipv6/tcp_mss.h"
#include "log.h"
#include <fips/identity/mod.h>
#include <stdlib.h>
#include <string.h>
#define MOD "ipv6/mss"
int fipsd_ipv6_paths_init(fipsd_ipv6_paths *p, size_t capacity)
{
    *p = (fipsd_ipv6_paths){.capacity = capacity ? capacity : 256};
    p->entries = calloc(p->capacity, sizeof *p->entries);
    return p->entries ? 0 : -1;
}
void fipsd_ipv6_paths_destroy(fipsd_ipv6_paths *p) { free(p->entries); p->entries = NULL; p->count = 0; p->capacity = 0; }
static fipsd_ipv6_path *find(fipsd_ipv6_paths *p, const uint8_t addr[16])
{
    for (size_t i = 0; i < p->capacity; ++i)
        if (p->entries[i].used && !memcmp(p->entries[i].addr, addr, 16)) return &p->entries[i];
    return NULL;
}
void fipsd_ipv6_paths_clear(fipsd_ipv6_paths *p)
{
    nsync_mu_lock(&p->mu);
    for (size_t i = 0; i < p->capacity; ++i) p->entries[i].used = false;
    p->count = 0;
    nsync_mu_unlock(&p->mu);
}
bool fipsd_ipv6_paths_set(fipsd_ipv6_paths *p, const uint8_t addr[16], uint16_t limit)
{
    nsync_mu_lock(&p->mu);
    fipsd_ipv6_path *e = find(p, addr);
    for (size_t i = 0; !e && i < p->capacity; ++i) if (!p->entries[i].used) {
        e = &p->entries[i]; e->used = true; memcpy(e->addr, addr, 16); ++p->count;
    }
    if (e) e->limit = limit;
    nsync_mu_unlock(&p->mu);
    if (!e) FIPSD_WARN(MOD, "path MTU table full (%zu entries); a per-destination limit was not stored", p->capacity);
    return e != NULL;
}
void fipsd_ipv6_paths_remove(fipsd_ipv6_paths *p, const uint8_t addr[16])
{
    nsync_mu_lock(&p->mu);
    fipsd_ipv6_path *e = find(p, addr);
    if (e) { e->used = false; --p->count; }
    nsync_mu_unlock(&p->mu);
}
bool fipsd_ipv6_paths_get(fipsd_ipv6_paths *p, const uint8_t addr[16], uint16_t *limit)
{
    if (!p || !p->entries) return false;
    nsync_mu_lock(&p->mu);
    fipsd_ipv6_path *e = find(p, addr);
    if (e && limit) *limit = e->limit;
    nsync_mu_unlock(&p->mu);
    return e != NULL;
}
size_t fipsd_ipv6_paths_count(fipsd_ipv6_paths *p)
{
    nsync_mu_lock(&p->mu); size_t n = p->count; nsync_mu_unlock(&p->mu); return n;
}
uint16_t fipsd_ipv6_flow_ceiling(bool has_limit, uint16_t limit, uint16_t global)
{
    /* RFC 8200 IPv6-minimum MTU (1280) -> effective FIPS-encapsulated payload
     * (1203) -> TCP segment after IPv6+TCP headers (1143). */
    uint16_t empty = fipsd_ipv6_mss_ceiling(FIPSD_IPV6_MIN_MTU);
    if (global < empty) empty = global;
    if (!has_limit) return empty;
    /* The actionable floor deliberately does not apply here: a small stored
     * value is one the node derived from its own outgoing link and is exact.
     * What no provenance rescues is the arithmetic degenerating: at a payload
     * limit of 27 or less not one payload byte fits, and the clamp refuses a
     * ceiling of zero, which would leave the SYN carrying the kernel's MSS. */
    uint16_t path = fipsd_ipv6_tcp_mss(limit);
    if (!path) return empty;
    return global < path ? global : path;
}
uint16_t fipsd_ipv6_per_flow_max_mss(fipsd_ipv6_paths *paths, const uint8_t *addr, size_t addr_len, uint16_t global)
{
    uint16_t empty = fipsd_ipv6_flow_ceiling(false, 0, global);
    /* trace, not warn: this runs on every packet, not only on SYNs. */
    if (addr_len != 16) {
        FIPSD_TRACE(MOD, "per_flow_max_mss: addr_bytes wrong length, fall back to conservative ceiling %u", empty);
        return empty;
    }
    if (addr[0] != FIPS_IPV6_ADDRESS_PREFIX) {
        FIPSD_TRACE(MOD, "per_flow_max_mss: non-fd::/8 prefix, fall back to conservative ceiling %u", empty);
        return empty;
    }
    uint16_t limit;
    if (!fipsd_ipv6_paths_get(paths, addr, &limit)) {
        FIPSD_TRACE(MOD, "per_flow_max_mss: no path_mtu_lookup entry for destination, fall back to conservative ceiling %u", empty);
        return empty;
    }
    uint16_t result = fipsd_ipv6_flow_ceiling(true, limit, global);
    if (!fipsd_ipv6_tcp_mss(limit))
        FIPSD_TRACE(MOD, "per_flow_max_mss: stored path_mtu (payload limit %u) leaves no room for a TCP payload byte, using conservative ceiling", limit);
    else
        FIPSD_TRACE(MOD, "per_flow_max_mss: per-destination clamp applied (payload limit %u, result %u)", limit, result);
    return result;
}
bool fipsd_ipv6_clamp(uint8_t *packet, size_t len, uint16_t ceiling, fipsd_ipv6_paths *paths,
                      fipsd_ipv6_remote remote, uint16_t *applied)
{
    size_t at = remote == FIPSD_IPV6_REMOTE_SOURCE ? 8 : 24;
    uint16_t limit = len >= at + 16 ? fipsd_ipv6_per_flow_max_mss(paths, packet + at, 16, ceiling) : ceiling;
    if (!fipsd_ipv6_clamp_tcp_mss(packet, len, limit)) return false;
    if (applied) *applied = limit;
    return true;
}
