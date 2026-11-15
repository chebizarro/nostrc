#ifndef FIPSD_ACTIVE_PEER_H
#define FIPSD_ACTIVE_PEER_H
#include "peer/data.h"
#include "peer/machine.h"
#include "refptr.h"
#include <fips/proto/mmp/state.h>

typedef struct fipsd_active_peer fipsd_active_peer;
typedef struct fipsd_peer_info {
    fips_peer_identity identity;
    fips_transport_id transport;
    fips_transport_addr addr;
    fips_link_id link;
    fips_link_direction direction;
    fipsd_peer_state state;
    fipsd_peer_data data;
    bool has_mmp;
    fips_mmp_metrics metrics;
} fipsd_peer_info;
/* Opaque, immovable owner. Dispatcher only, except snapshot_load which is
 * thread-safe. A snapshot contains public metadata, never keys or slots. */
fipsd_active_peer *fipsd_active_peer_create(const fips_peer_identity *identity,
    fips_transport_id transport, const fips_transport_addr *addr, fips_link_id link,
    fips_link_direction direction, uint64_t wall_ms, fips_instant now,
    fipsd_peer_timing timing);
/* Refuses live rekey state: close it and execute its outputs before destroy. */
bool fipsd_active_peer_destroy(fipsd_active_peer *peer);
const fipsd_peer_info *fipsd_active_peer_info(const fipsd_active_peer *peer);
fipsd_peer_data *fipsd_active_peer_data(fipsd_active_peer *peer);
void fipsd_active_peer_step(fipsd_active_peer *peer, fips_instant now,
    const fipsd_peer_event *event, fipsd_peer_actions *out);
fips_instant fipsd_active_peer_next_wake(const fipsd_active_peer *peer);
bool fipsd_active_peer_store_msg1(fipsd_active_peer *peer, const uint8_t *wire, size_t len);
/* MMP mutable access is dispatcher-only; snapshots copy only pointer-free metrics. */
bool fipsd_active_peer_mmp_init(fipsd_active_peer *peer, fips_mmp_mode mode);
fips_mmp_peer_state *fipsd_active_peer_mmp(fipsd_active_peer *peer);
bool fipsd_active_peer_publish(fipsd_active_peer *peer);
GoRefPtr fipsd_active_peer_snapshot_load(fipsd_active_peer *peer);
/* Rebinding a table member MUST go through table_rebind (both index and cap).
 * The table uses this setter only after its transactional preflight. */
void fipsd_active_peer_set_endpoint(fipsd_active_peer *peer, fips_transport_id transport,
    const fips_transport_addr *addr);
#endif
