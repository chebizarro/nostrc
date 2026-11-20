#include "peer/machine.h"
#include <math.h>
#include <string.h>

static bool due(fips_instant at, fips_instant now)
{ return at.ops && at.ops->compare(now, at) >= 0; }
static void emit(fipsd_peer_actions *a, fipsd_peer_action_kind kind, fips_fmp_bytes bytes)
{ a->items[a->count++] = (fipsd_peer_action){kind, bytes}; }
static void action(fipsd_peer_actions *a, fipsd_peer_action_kind kind)
{ emit(a, kind, (fips_fmp_bytes){0}); }
static fips_instant after(fips_instant now, uint64_t ms)
{ return fips_deadline(now, fips_duration_from_millis(ms)); }

fipsd_peer_timing fipsd_peer_timing_default(void)
{ return (fipsd_peer_timing){1000, 30000, 10000, 5000, 5, 2.0}; }
bool fipsd_peer_timing_valid(fipsd_peer_timing p)
{
    return p.retransmit_ms && p.handshake_timeout_ms && p.heartbeat_ms &&
        isfinite(p.resend_backoff) && p.resend_backoff >= 1.0;
}
void fipsd_peer_machine_init(fipsd_peer_machine *m, fips_link_id link,
    fips_link_direction direction, const fips_peer_identity *expected,
    fips_fmp_leg_stamp at, fipsd_peer_timing timing)
{
    memset(m, 0, sizeof(*m));
    m->timing = timing;
    if (direction == FIPS_LINK_OUTBOUND)
        fips_fmp_connection_outbound(&m->conn, link, *expected, at);
    else
        fips_fmp_connection_inbound(&m->conn, link, at);
}
bool fipsd_peer_machine_established(const fipsd_peer_machine *m)
{
    return m->state == FIPSD_PEER_ESTABLISHED || m->state == FIPSD_PEER_ACTIVE ||
        m->state == FIPSD_PEER_MAINTAINING;
}
static void cancel(fipsd_peer_machine *m)
{
    m->timeout_at = m->resend_at = m->liveness_at = (fips_instant){0};
    m->resend_pending = false;
}
static void close_peer(fipsd_peer_machine *m, fips_instant now)
{
    cancel(m);
    m->state = FIPSD_PEER_CLOSED;
    m->closed_until = after(now, m->timing.cooldown_ms);
}
static void start(fipsd_peer_machine *m, fips_instant now, fipsd_peer_actions *a)
{
    m->state = FIPSD_PEER_HANDSHAKING;
    m->timeout_at = after(now, m->timing.handshake_timeout_ms);
    m->resend_at = after(now, m->timing.retransmit_ms);
    action(a, FIPSD_PEER_START_HANDSHAKE);
}
void fipsd_peer_machine_step(fipsd_peer_machine *m, fips_instant now,
    const fipsd_peer_event *e, fipsd_peer_actions *a)
{
    *a = (fipsd_peer_actions){0};
    switch (e->kind) {
    case FIPSD_PEER_DIAL:
        if (m->state != FIPSD_PEER_DISCOVERED) break;
        if (e->connection_oriented) {
            m->state = FIPSD_PEER_CONNECTING;
            m->timeout_at = after(now, m->timing.handshake_timeout_ms);
            action(a, FIPSD_PEER_OPEN_TRANSPORT);
        } else start(m, now, a);
        break;
    case FIPSD_PEER_CONNECTED:
        if (m->state == FIPSD_PEER_CONNECTING) start(m, now, a);
        break;
    case FIPSD_PEER_CONNECT_FAILED:
        if (m->state != FIPSD_PEER_CONNECTING) break;
        close_peer(m, now);
        action(a, FIPSD_PEER_TEARDOWN);
        action(a, FIPSD_PEER_LOST_HANDSHAKE);
        break;
    case FIPSD_PEER_SEND_FAILED:
        if (m->state == FIPSD_PEER_HANDSHAKING) m->send_failed = true;
        break;
    case FIPSD_PEER_RESEND_RESULT:
        if (!m->resend_pending) break;
        m->resend_pending = false;
        if (e->success) fips_fmp_connection_record_resend(&m->conn);
        /* The successful deadline is the one computed by the FMP core. */
        else m->resend_at = after(now, m->timing.retransmit_ms);
        break;
    case FIPSD_PEER_INBOUND_AUTHENTICATED: {
        if (m->state == FIPSD_PEER_CLOSED || m->state == FIPSD_PEER_FAILED) break;
        fips_fmp_inbound_decision d = fips_fmp_establish_inbound(&e->inbound, &e->wire);
        if (d.kind == FIPS_FMP_IN_Reject) {
            if (!fipsd_peer_machine_established(m)) {
                cancel(m);
                m->state = FIPSD_PEER_FAILED;
                action(a, FIPSD_PEER_TEARDOWN);
            }
            break;
        }
        if (d.kind == FIPS_FMP_IN_Rekey) { action(a, FIPSD_PEER_REKEY_INPUT); break; }
        if (d.kind == FIPS_FMP_IN_RestartThenPromote || d.kind == FIPS_FMP_IN_ReplaceThenPromote) {
            action(a, FIPSD_PEER_INVALIDATE);
            action(a, FIPSD_PEER_LOST_LINK);
        }
        m->conn.expected_identity = e->wire.peer_identity;
        m->conn.has_expected_identity = true;
        m->conn.remote_epoch = e->wire.remote_epoch;
        m->conn.their_index = e->wire.their_index;
        m->conn.has_their_index = true;
        if (fips_fmp_connection_set_handshake_msg2(&m->conn,
                e->wire.msg2_payload.data, e->wire.msg2_payload.len) != FIPS_OK) {
            cancel(m);
            m->state = FIPSD_PEER_FAILED;
            action(a, FIPSD_PEER_TEARDOWN);
            break;
        }
        m->state = FIPSD_PEER_AUTHORIZING;
        m->timeout_at = after(now, m->timing.handshake_timeout_ms);
        break;
    }
    case FIPSD_PEER_MSG2_AUTHENTICATED: {
        if (m->state != FIPSD_PEER_HANDSHAKING || m->conn.direction != FIPS_LINK_OUTBOUND) break;
        m->conn.remote_epoch = e->wire.remote_epoch;
        m->conn.their_index = e->wire.their_index;
        m->conn.has_their_index = true;
        fips_fmp_outbound_decision d = fips_fmp_establish_outbound(&e->outbound, e->wire.remote_epoch);
        cancel(m);
        if (d == FIPS_FMP_OUT_CrossConnectionKeep || d == FIPS_FMP_OUT_CrossConnectionSwap) {
            action(a, d == FIPS_FMP_OUT_CrossConnectionKeep ? FIPSD_PEER_CROSS_KEEP : FIPSD_PEER_CROSS_SWAP);
            /* The surviving peer is separate; this connection is terminal. */
            close_peer(m, now);
            break;
        }
        if (d == FIPS_FMP_OUT_RestartThenPromote) {
            action(a, FIPSD_PEER_INVALIDATE);
            action(a, FIPSD_PEER_LOST_LINK);
        }
        action(a, FIPSD_PEER_PROMOTE);
        break;
    }
    case FIPSD_PEER_AUTHORIZED:
        if (m->state != FIPSD_PEER_AUTHORIZING) break;
        emit(a, FIPSD_PEER_SEND_HANDSHAKE, (fips_fmp_bytes){m->conn.handshake_msg2, m->conn.handshake_msg2_len});
        action(a, FIPSD_PEER_PROMOTE);
        break;
    case FIPSD_PEER_REJECTED:
        if (m->state != FIPSD_PEER_HANDSHAKING && m->state != FIPSD_PEER_AUTHORIZING) break;
        cancel(m); m->state = FIPSD_PEER_FAILED;
        action(a, FIPSD_PEER_TEARDOWN);
        break;
    case FIPSD_PEER_PROMOTED:
        if (m->state != FIPSD_PEER_HANDSHAKING && m->state != FIPSD_PEER_AUTHORIZING) break;
        cancel(m);
        if (e->promotion.kind == FIPS_FMP_CrossConnectionLost) {
            m->state = FIPSD_PEER_FAILED;
            action(a, FIPSD_PEER_TEARDOWN);
        } else {
            m->state = FIPSD_PEER_ESTABLISHED;
            m->liveness_at = after(now, m->timing.heartbeat_ms);
        }
        break;
    case FIPSD_PEER_HEARD:
        if (fipsd_peer_machine_established(m)) {
            m->state = FIPSD_PEER_ACTIVE;
            m->liveness_at = after(now, m->timing.heartbeat_ms);
        }
        break;
    case FIPSD_PEER_LINK_DEAD:
    case FIPSD_PEER_DISCONNECT:
        if (!fipsd_peer_machine_established(m)) break;
        if (e->kind == FIPSD_PEER_DISCONNECT) action(a, FIPSD_PEER_SEND_DISCONNECT);
        action(a, FIPSD_PEER_INVALIDATE);
        action(a, FIPSD_PEER_TEARDOWN);
        if (e->kind == FIPSD_PEER_LINK_DEAD) action(a, FIPSD_PEER_LOST_LINK);
        close_peer(m, now);
        break;
    case FIPSD_PEER_POLL:
        /* Timeout precedes resend when both are due. Closed never re-dials. */
        if (due(m->timeout_at, now)) {
            action(a, FIPSD_PEER_TEARDOWN);
            if (m->conn.direction == FIPS_LINK_OUTBOUND) action(a, FIPSD_PEER_LOST_HANDSHAKE);
            close_peer(m, now);
        } else if (m->state == FIPSD_PEER_HANDSHAKING && !m->resend_pending &&
                   due(m->resend_at, now) && m->conn.has_handshake_msg1 &&
                   m->conn.resend_count < m->timing.max_resends) {
            fips_fmp_conn_snapshot s = {.link = m->conn.link_id,
                .resend_count = m->conn.resend_count,
                .msg1 = {m->conn.handshake_msg1, m->conn.handshake_msg1_len}};
            fips_fmp_conn_action item;
            fips_fmp_action_list list = {&item, 0, 1};
            if (fips_fmp_poll_resends(&s, 1, now, m->timing.retransmit_ms,
                    m->timing.resend_backoff, &list) == FIPS_OK && list.len) {
                emit(a, FIPSD_PEER_SEND_HANDSHAKE, item.bytes);
                m->resend_at = item.next_resend_at;
                m->resend_pending = true;
            }
        } else if (fipsd_peer_machine_established(m) && due(m->liveness_at, now)) {
            action(a, FIPSD_PEER_SEND_HEARTBEAT);
            m->liveness_at = after(now, m->timing.heartbeat_ms);
        }
        break;
    }
}
fips_instant fipsd_peer_machine_next_wake(const fipsd_peer_machine *m)
{
    fips_instant at = m->timeout_at;
    fips_instant resend = !m->resend_pending && m->conn.has_handshake_msg1 &&
        m->conn.resend_count < m->timing.max_resends ? m->resend_at : (fips_instant){0};
    if (resend.ops && (!at.ops || at.ops->compare(resend, at) < 0)) at = resend;
    if (m->liveness_at.ops && (!at.ops || at.ops->compare(m->liveness_at, at) < 0)) at = m->liveness_at;
    return at;
}
void fipsd_peer_retry_init(fipsd_peer_retry *r) { *r = (fipsd_peer_retry){0}; }
bool fipsd_peer_retry_failed(fipsd_peer_retry *r, fipsd_peer_retry_policy p,
    fips_instant now, bool link_dead, bool configured, bool auto_reconnect,
    bool connected, bool suspended)
{
    if (suspended || (!link_dead && (connected || !p.max_retries))) return false;
    if (link_dead && (!configured || !auto_reconnect)) return false;
    if (!r->scheduled && !configured) return false;
    if (r->scheduled) {
        if (r->count != UINT32_MAX) ++r->count;
        if (link_dead) r->reconnect = true;
        if (!r->reconnect && r->count > p.max_retries) {
            fipsd_peer_retry_init(r); return false;
        }
    } else {
        r->count = link_dead ? 0 : 1;
        r->reconnect = true;
    }
    r->scheduled = true;
    r->after = after(now, fips_fmp_backoff_ms(r->count, p.base_ms, p.cap_ms));
    return true;
}
bool fipsd_peer_retry_take(fipsd_peer_retry *r, fipsd_peer_retry_policy p,
    fips_instant now, bool connected, bool admission_ok, bool suspended)
{
    if (suspended || connected || due(r->expires, now)) {
        fipsd_peer_retry_init(r); return false;
    }
    if (!r->scheduled || !admission_ok || (r->after.ops && !due(r->after, now))) return false;
    r->after = after(now, p.handshake_timeout_ms);
    return true;
}
