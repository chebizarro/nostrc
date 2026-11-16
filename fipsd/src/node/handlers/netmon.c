/* Network change reactions (upstream handlers/netmon.rs): interface presence
 * reaches the path machines; moved peers are heartbeated by their own timers. */
#include "node/handlers/internal.h"

void hs_netmon_event(handlers *h, const fipsd_netmon_event *e)
{
    if (e->kind == FIPSD_NETWORK_READ_FAILED) return;
    hs_path_network_event(h, e);
    if (e->kind == FIPSD_INTERFACE_UP || e->kind == FIPSD_INTERFACE_ADDR_CHANGE) {
        /* Peers that may have moved with the interface get an immediate liveness
         * probe through the peer machine's heartbeat rather than a timer reset. */
        for (size_t i = 0; i < h->cfg.peer_capacity; ++i) if (h->recs[i].used) {
            uint8_t hb = FIPS_LINK_Heartbeat;
            if (hs_link_send(h, &h->recs[i], &hb, 1)) ++h->counters.heartbeats_sent;
        }
    }
    hs_mark_dirty(h);
}
