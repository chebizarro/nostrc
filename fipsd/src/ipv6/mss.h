#ifndef FIPSD_IPV6_MSS_H
#define FIPSD_IPV6_MSS_H
/* The shim's TCP MSS clamp (upstream mss.rs): the node-wide ceiling the TUN
 * fibers share, and the per-destination ceiling read from the shim's copy of
 * the node's path MTU map. */
#include "ipv6/mtu.h"
#include <nsync.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct fipsd_ipv6_path { bool used; uint8_t addr[16]; uint16_t limit; } fipsd_ipv6_path;
/* Per-peer payload limits keyed by the peer's IPv6 address, as the TUN fibers
 * look them up from a packet's address bytes (upstream PathMtuReplica). The
 * node side writes under the mutex; readers hold it for one lookup. Bounded by
 * the host: a full table refuses a new entry and logs it. */
typedef struct fipsd_ipv6_paths {
    nsync_mu mu;
    fipsd_ipv6_path *entries;
    size_t capacity, count;
} fipsd_ipv6_paths;
int fipsd_ipv6_paths_init(fipsd_ipv6_paths *paths, size_t capacity);
void fipsd_ipv6_paths_destroy(fipsd_ipv6_paths *paths);
void fipsd_ipv6_paths_clear(fipsd_ipv6_paths *paths);
bool fipsd_ipv6_paths_set(fipsd_ipv6_paths *paths, const uint8_t addr[16], uint16_t payload_limit);
void fipsd_ipv6_paths_remove(fipsd_ipv6_paths *paths, const uint8_t addr[16]);
bool fipsd_ipv6_paths_get(fipsd_ipv6_paths *paths, const uint8_t addr[16], uint16_t *payload_limit);
size_t fipsd_ipv6_paths_count(fipsd_ipv6_paths *paths);

/* min(global, tcp_mss(limit)), or the conservative min(global, mss_ceiling(1280))
 * = 1143 under a 1500-byte transport when nothing is stored or the stored
 * limit leaves no room for one TCP payload byte. The reader's clamp and the
 * re-clamp of a held SYN both take their ceiling from here. */
uint16_t fipsd_ipv6_flow_ceiling(bool has_limit, uint16_t payload_limit, uint16_t global);
/* The per-flow ceiling for a 16-byte fd00::/8 address; anything else (wrong
 * length, another prefix, no entry) falls back to the conservative ceiling. */
uint16_t fipsd_ipv6_per_flow_max_mss(fipsd_ipv6_paths *paths, const uint8_t *addr, size_t addr_len, uint16_t global);
/* Which address names the far end of a flow: the source of a packet toward
 * the host, the destination of one from it. */
typedef enum fipsd_ipv6_remote { FIPSD_IPV6_REMOTE_SOURCE, FIPSD_IPV6_REMOTE_DESTINATION } fipsd_ipv6_remote;
/* Clamp a SYN's MSS to the smaller of ceiling and the stored path to the far
 * end; a packet too short to hold that address is clamped to ceiling alone.
 * True when clamped, with the ceiling applied in *applied. */
bool fipsd_ipv6_clamp(uint8_t *packet, size_t len, uint16_t ceiling, fipsd_ipv6_paths *paths,
                      fipsd_ipv6_remote remote, uint16_t *applied);
#endif
