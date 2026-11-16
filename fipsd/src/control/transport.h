#ifndef FIPSD_CONTROL_TRANSPORT_H
#define FIPSD_CONTROL_TRANSPORT_H
#include "control/snapshot.h"
#include "transport/transport.h"
/* Caller-owned scratch; row borrows stable transport metadata and this view.
 * The provider allocates it in the retained snapshot, not on its stack. */
typedef struct fipsd_control_transport_view {
    fipsd_control_transport row;
    fipsd_control_interface interface;
    fipsd_control_counter counters[FIPSD_TRANSPORT_STATS_COUNT];
    char local_addr[300], onion_address[128];
    json_t *tor_monitoring; /* owned by the view: adopt into the snapshot or decref */
} fipsd_control_transport_view;
void fipsd_control_transport_read(const fipsd_transport *, uint64_t now_ms,
                                 fipsd_control_transport_view *);
#endif
