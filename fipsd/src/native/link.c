#include "native/link.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

const char *fipsd_native_drop_reason_text(fipsd_native_drop_reason reason)
{
    switch (reason) {
    case FIPSD_NATIVE_REASON_NO_PORT: return "no listener or flow on that port";
    case FIPSD_NATIVE_REASON_BACKLOG_FULL: return "listener backlog full";
    case FIPSD_NATIVE_REASON_TOO_MANY_FLOWS: return "node flow ceiling reached";
    case FIPSD_NATIVE_REASON_PEER_SHARE: return "peer flow limit reached";
    case FIPSD_NATIVE_REASON_PENDING_QUEUE_FULL: case FIPSD_NATIVE_REASON_FLOW_QUEUE_FULL: return "queue full";
    case FIPSD_NATIVE_REASON_ARRIVAL_QUEUE_FULL: return "arrival queue full";
    case FIPSD_NATIVE_REASON_LISTENER_NOT_READING: return "listener not reading arrivals";
    case FIPSD_NATIVE_REASON_LISTENER_GONE: default: return "listener closed";
    }
}
fipsd_metric fipsd_native_drop_reason_metric(fipsd_native_drop_reason reason)
{
    switch (reason) {
    case FIPSD_NATIVE_REASON_NO_PORT: return FIPSD_METRIC_native_drop_no_port;
    case FIPSD_NATIVE_REASON_BACKLOG_FULL: return FIPSD_METRIC_native_drop_backlog_full;
    case FIPSD_NATIVE_REASON_TOO_MANY_FLOWS: return FIPSD_METRIC_native_drop_too_many_flows;
    case FIPSD_NATIVE_REASON_PEER_SHARE: return FIPSD_METRIC_native_drop_peer_share;
    case FIPSD_NATIVE_REASON_PENDING_QUEUE_FULL: return FIPSD_METRIC_native_drop_pending_queue_full;
    case FIPSD_NATIVE_REASON_FLOW_QUEUE_FULL: return FIPSD_METRIC_native_drop_flow_queue_full;
    case FIPSD_NATIVE_REASON_ARRIVAL_QUEUE_FULL: return FIPSD_METRIC_native_drop_arrival_queue_full;
    case FIPSD_NATIVE_REASON_LISTENER_NOT_READING: return FIPSD_METRIC_native_drop_listener_not_reading;
    case FIPSD_NATIVE_REASON_LISTENER_GONE: default: return FIPSD_METRIC_native_drop_listener_gone;
    }
}
fipsd_native_drop_reason fipsd_native_drop_reason_of(fipsd_native_drop_cause cause)
{
    switch (cause) {
    case FIPSD_NATIVE_DROP_NO_PORT: return FIPSD_NATIVE_REASON_NO_PORT;
    case FIPSD_NATIVE_DROP_BACKLOG_FULL: return FIPSD_NATIVE_REASON_BACKLOG_FULL;
    case FIPSD_NATIVE_DROP_TOO_MANY_FLOWS: return FIPSD_NATIVE_REASON_TOO_MANY_FLOWS;
    case FIPSD_NATIVE_DROP_QUEUE_FULL: return FIPSD_NATIVE_REASON_PENDING_QUEUE_FULL;
    case FIPSD_NATIVE_DROP_PEER_SHARE: default: return FIPSD_NATIVE_REASON_PEER_SHARE;
    }
}
fipsd_native_drop_reason fipsd_native_drop_reason_why(int error)
{
    return error == EPIPE || error == ECONNRESET || error == EDESTADDRREQ
        ? FIPSD_NATIVE_REASON_LISTENER_GONE : FIPSD_NATIVE_REASON_LISTENER_NOT_READING;
}
static fipsd_native_outcome dropped(fipsd_native_drop_reason reason)
{
    return (fipsd_native_outcome){.kind = FIPSD_NATIVE_OUTCOME_DROPPED, .reason = reason};
}
fipsd_native_outcome fipsd_native_link_deliver(fipsd_native_registry *r, const fips_node_addr *peer,
                                               const fips_xonly_pubkey *pubkey, uint16_t src, uint16_t dst,
                                               fipsd_native_datagram *dg, uint64_t now)
{
    fipsd_native_delivery d = fipsd_native_registry_deliver(r, peer, pubkey, src, dst, now);
    switch (d.kind) {
    case FIPSD_NATIVE_DELIVER_FLOW:
        /* Never block the receive path on a slow client: a full queue costs
         * that client a datagram, not the node its tick. */
        if (go_channel_try_send(d.sink, dg) == 0) return (fipsd_native_outcome){.kind = FIPSD_NATIVE_OUTCOME_DELIVERED};
        fipsd_native_datagram_free(dg);
        /* The flow is named so its owner can react to the overflow. */
        return (fipsd_native_outcome){.kind = FIPSD_NATIVE_OUTCOME_DROPPED, .reason = FIPSD_NATIVE_REASON_FLOW_QUEUE_FULL, .flow = d.flow};
    case FIPSD_NATIVE_DELIVER_ARRIVED: {
        fipsd_native_arrival *a = malloc(sizeof *a);
        if (!a || go_channel_try_send(d.arrivals, a) != 0) {
            /* The client is not reading its own announcements: undo the
             * registration rather than leave a pending flow nobody is told of. */
            free(a); fipsd_native_datagram_free(dg);
            (void)fipsd_native_registry_reject(r, d.flow, NULL);
            return dropped(FIPSD_NATIVE_REASON_ARRIVAL_QUEUE_FULL);
        }
        *a = d.arrival;
        if (fipsd_native_registry_hold(r, d.flow, dg))
            return (fipsd_native_outcome){.kind = FIPSD_NATIVE_OUTCOME_ANNOUNCED, .flow = d.flow};
        fipsd_native_datagram_free(dg);
        return dropped(FIPSD_NATIVE_REASON_PENDING_QUEUE_FULL);
    }
    case FIPSD_NATIVE_DELIVER_PENDING:
        if (fipsd_native_registry_hold(r, d.flow, dg))
            return (fipsd_native_outcome){.kind = FIPSD_NATIVE_OUTCOME_HELD, .flow = d.flow};
        fipsd_native_datagram_free(dg);
        return dropped(FIPSD_NATIVE_REASON_PENDING_QUEUE_FULL);
    case FIPSD_NATIVE_DELIVER_DROP: default:
        fipsd_native_datagram_free(dg);
        return dropped(fipsd_native_drop_reason_of(d.cause));
    }
}
void fipsd_native_link_count(fipsd_metrics *m, const fipsd_native_outcome *outcome, size_t bytes)
{
    if (outcome->kind == FIPSD_NATIVE_OUTCOME_DROPPED) {
        fipsd_metrics_add(m, fipsd_native_drop_reason_metric(outcome->reason), 1);
        return;
    }
    fipsd_metrics_add(m, FIPSD_METRIC_native_received_datagrams, 1);
    fipsd_metrics_add(m, FIPSD_METRIC_native_received_bytes, bytes);
}
void fipsd_native_outcome_text(const fipsd_native_outcome *outcome, char *out, size_t cap)
{
    switch (outcome->kind) {
    case FIPSD_NATIVE_OUTCOME_DELIVERED: snprintf(out, cap, "delivered"); break;
    case FIPSD_NATIVE_OUTCOME_ANNOUNCED: snprintf(out, cap, "announced"); break;
    case FIPSD_NATIVE_OUTCOME_HELD: snprintf(out, cap, "held"); break;
    case FIPSD_NATIVE_OUTCOME_DROPPED: default:
        snprintf(out, cap, "dropped: %s", fipsd_native_drop_reason_text(outcome->reason)); break;
    }
}
