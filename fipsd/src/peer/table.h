#ifndef FIPSD_PEER_TABLE_H
#define FIPSD_PEER_TABLE_H
#include "peer/active.h"
#include "peer/budget.h"

typedef struct fipsd_peer_table fipsd_peer_table;
/* Sorted node index, bounded linear endpoint index (worst-case O(capacity),
 * no attacker-selected hash collision growth). Stable peer allocations. */
fipsd_peer_table *fipsd_peer_table_create(size_t capacity, fipsd_peer_caps total,
    const fipsd_transport_caps *transports, size_t transport_count);
/* Requires closed links and no concurrent/future snapshot loads. Previously
 * loaded references remain valid. Refuses live links without changing table. */
bool fipsd_peer_table_destroy(fipsd_peer_table *table);
/* Reserve before outbound transport connect; pending connects and handshakes
 * share the same connection cap and at most table-capacity reservations. Tickets are linear; release on EVERY refusal.
 * Reserved insertion promotes then transfers the ticket only on success. */
fipsd_peer_admission fipsd_peer_table_reserve(fipsd_peer_table *table,
    fips_transport_id transport, fipsd_peer_ticket *ticket);
fipsd_peer_admission fipsd_peer_table_insert_reserved(fipsd_peer_table *table,
    fipsd_active_peer *peer, fipsd_peer_ticket *ticket);
fipsd_peer_admission fipsd_peer_table_insert(fipsd_peer_table *table, fipsd_active_peer *peer);
fipsd_active_peer *fipsd_peer_table_find(fipsd_peer_table *table, const fips_node_addr *addr);
fipsd_active_peer *fipsd_peer_table_endpoint(fipsd_peer_table *table,
    fips_transport_id transport, const fips_transport_addr *addr);
fipsd_peer_admission fipsd_peer_table_remove(fipsd_peer_table *table, const fips_node_addr *addr);
/* Dispatcher-only, including attached links: transfer the admission ticket and
 * active send pair atomically on success; another peer's pair or exhausted
 * transport budget refuses without changing either. Link identity, keys and
 * index leases keep their original ownership/provenance. The caller must
 * invalidate/reseed dataplane state and publish before subsequent sends.
 * Readers use owned snapshots, never this mutable table or peer allocation. */
fipsd_peer_admission fipsd_peer_table_rebind(fipsd_peer_table *table, const fips_node_addr *peer,
    fips_transport_id transport, const fips_transport_addr *addr);
size_t fipsd_peer_table_count(const fipsd_peer_table *table);
fipsd_active_peer *fipsd_peer_table_at(fipsd_peer_table *table, size_t index);
typedef struct fipsd_peer_table_snapshot {
    uint64_t generation;
    size_t count;
    fipsd_peer_info peers[];
} fipsd_peer_table_snapshot;
/* Explicit publication failure, never silent success with a stale snapshot. */
bool fipsd_peer_table_publish(fipsd_peer_table *table);
GoRefPtr fipsd_peer_table_snapshot_load(fipsd_peer_table *table);
#endif
