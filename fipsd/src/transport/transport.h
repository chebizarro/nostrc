#ifndef FIPSD_TRANSPORT_SHARED_H
#define FIPSD_TRANSPORT_SHARED_H
#include "node/transport.h"
#include "transport/presence.h"
#include "transport/stats_common.h"
#define FIPSD_TRANSPORT_LINKS 64u
typedef struct fipsd_transport_link {
    bool used;
    fips_link_id id;
    fips_transport_addr address;
    fips_link_direction direction;
    uint16_t mtu;
    fips_link_stats stats;
} fipsd_transport_link;
/* Embed in a concrete transport. Object, metadata and event sink outlive
 * runtime join. Counters persist across detach; descriptor ownership does not
 * live here. The bounded link table is optional for datagram drivers. */
typedef struct fipsd_transport_shared {
    _Atomic int state;
    _Atomic bool started;
    fipsd_presence presence;
    fipsd_transport_stats stats;
    fipsd_transport_diagnostics diagnostics;
    nsync_mu links_mu;
    fipsd_transport_link links[FIPSD_TRANSPORT_LINKS];
} fipsd_transport_shared;
void fipsd_transport_shared_init(fipsd_transport *, fipsd_transport_shared *, bool optional);
fipsd_transport_state fipsd_transport_shared_state(const fipsd_transport *);
fips_path_medium fipsd_transport_medium(const fipsd_transport *);
/* Copies bytes; only success transfers the allocated event. stop is optional.
 * Selecting cancellation never frees or closes the borrowed destination. */
int fipsd_transport_emit(GoChannel *events, GoContext *root, GoChannel *stop,
    fipsd_transport_event_kind, fips_transport_id, const fips_transport_addr *, const uint8_t *, size_t);
/* The same send for an event the caller built in a malloc block (the node
 * frees it): a receive path that fills events in place hands them over
 * without a copy (nostrc-u19cz.4). Only success transfers e. */
int fipsd_transport_emit_event(GoChannel *events, GoContext *root, GoChannel *stop, fipsd_transport_event *);
/* Replacement gets a new process-wide id. Old receive/writer exits can only
 * remove their own id, never the replacement (stream.rs remove_own). */
int fipsd_transport_link_add(fipsd_transport_shared *, const fips_transport_addr *,
    fips_link_direction, uint16_t mtu, fips_link_id *);
bool fipsd_transport_link_remove(fipsd_transport_shared *, const fips_transport_addr *, fips_link_id);
bool fipsd_transport_link_get(fipsd_transport_shared *, const fips_transport_addr *, fipsd_transport_link *);
void fipsd_transport_links_clear(fipsd_transport_shared *);
bool fipsd_transport_link_record(fipsd_transport_shared *, const fips_transport_addr *,
    fips_link_id, size_t bytes, bool sent, uint64_t now_ms);
#endif
