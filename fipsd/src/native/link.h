#ifndef FIPSD_NATIVE_LINK_H
#define FIPSD_NATIVE_LINK_H
/* What a client fiber asks the registry to do, and what became of an inbound
 * datagram (upstream src/native/link.rs). The registry decides; this performs
 * the sends the decision names onto bounded mailboxes, never parking, so the
 * receive path is never held by a slow client. */
#include "native/registry.h"
#include "node/metrics.h"
#include <channel.h>

/* Why a datagram was not delivered, across every path that can refuse one. */
typedef enum fipsd_native_drop_reason {
    FIPSD_NATIVE_REASON_NO_PORT, FIPSD_NATIVE_REASON_BACKLOG_FULL, FIPSD_NATIVE_REASON_TOO_MANY_FLOWS,
    FIPSD_NATIVE_REASON_PEER_SHARE, FIPSD_NATIVE_REASON_PENDING_QUEUE_FULL, FIPSD_NATIVE_REASON_FLOW_QUEUE_FULL,
    FIPSD_NATIVE_REASON_ARRIVAL_QUEUE_FULL, FIPSD_NATIVE_REASON_LISTENER_NOT_READING, FIPSD_NATIVE_REASON_LISTENER_GONE
} fipsd_native_drop_reason;
/* The client-facing text (DropReason::as_str). */
const char *fipsd_native_drop_reason_text(fipsd_native_drop_reason reason);
/* The `native` counter one drop increments. */
fipsd_metric fipsd_native_drop_reason_metric(fipsd_native_drop_reason reason);
fipsd_native_drop_reason fipsd_native_drop_reason_of(fipsd_native_drop_cause cause);
/* What a failed hand-off write says about the client: a gone listener for
 * EPIPE, ECONNRESET and EDESTADDRREQ, a listener not reading otherwise. */
fipsd_native_drop_reason fipsd_native_drop_reason_why(int error);
typedef enum fipsd_native_outcome_kind {
    FIPSD_NATIVE_OUTCOME_DELIVERED, FIPSD_NATIVE_OUTCOME_ANNOUNCED, FIPSD_NATIVE_OUTCOME_HELD, FIPSD_NATIVE_OUTCOME_DROPPED
} fipsd_native_outcome_kind;
typedef struct fipsd_native_outcome {
    fipsd_native_outcome_kind kind;
    uint64_t flow; /* announced / held, and a drop at an established flow's full queue */
    fipsd_native_drop_reason reason;
} fipsd_native_outcome;
/* Deliver one inbound datagram to whatever owns its destination port. The
 * registry's flow sinks and listener mailboxes are GoChannels carrying owned
 * fipsd_native_datagram and fipsd_native_arrival pointers; `dg` is consumed. */
fipsd_native_outcome fipsd_native_link_deliver(fipsd_native_registry *r, const fips_node_addr *peer,
                                               const fips_xonly_pubkey *pubkey, uint16_t src, uint16_t dst,
                                               fipsd_native_datagram *dg, uint64_t now);
/* The same decision without the send to an established flow, for an owner
 * that writes or queues the datagram itself (api.c, the node's receive path
 * and `arrive`): true when an established flow owns the destination, with
 * `*sink` its registered sink and `outcome->flow` its identifier, `data`
 * untouched; otherwise false with the decision performed on a copy of `data`
 * (announced, held or dropped) in `*outcome`. */
bool fipsd_native_link_route(fipsd_native_registry *r, const fips_node_addr *peer, const fips_xonly_pubkey *pubkey,
                             uint16_t src, uint16_t dst, const uint8_t *data, size_t len, uint64_t now,
                             void **sink, fipsd_native_outcome *outcome);
/* Count one dispatched inbound datagram the way upstream's rx_loop does. */
void fipsd_native_link_count(fipsd_metrics *m, const fipsd_native_outcome *outcome, size_t bytes);
/* The `outcome` text the debug `arrive` command reports. */
void fipsd_native_outcome_text(const fipsd_native_outcome *outcome, char *out, size_t cap);
#endif
