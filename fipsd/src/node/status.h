#ifndef FIPSD_NODE_STATUS_H
#define FIPSD_NODE_STATUS_H
#include <stdbool.h>
#include <stdint.h>
#include <swap.h>

typedef enum fipsd_node_state {
    FIPSD_NODE_CREATED, FIPSD_NODE_STARTING, FIPSD_NODE_READY,
    FIPSD_NODE_DEGRADED, FIPSD_NODE_FAILED, FIPSD_NODE_DRAINING,
    FIPSD_NODE_STOPPING, FIPSD_NODE_STOPPED
} fipsd_node_state;
/* Immutable, host-internal control seam. Bits name lifecycle child indexes.
 * Ready means substrate startup completed, not protocol/peer convergence.
 * An absent transport remains up, but contributes to degraded. */
typedef struct fipsd_node_status {
    fipsd_node_state state;
    uint64_t up, failed, absent;
    bool ready;
} fipsd_node_status;
/* Slot is borrowed, outlives all loads. Release returned reference once. */
static inline GoRefPtr fipsd_node_status_load(GoSwap *slot) { return go_swap_load(slot); }
#endif
