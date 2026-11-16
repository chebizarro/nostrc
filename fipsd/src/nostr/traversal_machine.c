#include "nostr/traversal_machine.h"
#include <string.h>
static uint64_t add(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }
void fipsd_tr_config_default(fipsd_tr_config *c)
{
    *c = (fipsd_tr_config){.answer_ms = 120000, .attempt_ms = 10000, .start_delay_ms = 2000,
        .interval_ms = 200, .duration_ms = 10000, .retry_ms = 5000, .max_retry_ms = 300000,
        .cooldown_ms = 1800000, .retries = 5, .failure_threshold = 5};
}
void fipsd_tr_init(fipsd_tr_machine *m, const fipsd_tr_config *c)
{ *m = (fipsd_tr_machine){.config = *c}; }
bool fipsd_tr_suppress(const uint8_t ours[16], const uint8_t peer[16], bool active)
{ return active && memcmp(ours, peer, 16) < 0; }
typedef enum { ALWAYS, OUTBOUND, INBOUND, ACCEPTED, REJECTED, EXACT, REMAPPED,
    DUE, EXPIRED, SETTLED, SEND_DUE } guard;
typedef struct { fipsd_tr_state from; fipsd_tr_event event; guard when; fipsd_tr_state to; unsigned effects; } transition;
#define CANCEL_ROW(s) {s, FIPSD_TR_CANCEL, ALWAYS, FIPSD_TR_CANCELLED, FIPSD_TR_FINISHED}
static const transition table[] = {
    {FIPSD_TR_IDLE, FIPSD_TR_OFFER, OUTBOUND, FIPSD_TR_ANSWER, FIPSD_TR_SEND_OFFER},
    {FIPSD_TR_IDLE, FIPSD_TR_OFFER, INBOUND, FIPSD_TR_PUNCHING, FIPSD_TR_SEND_ANSWER},
    {FIPSD_TR_ANSWER, FIPSD_TR_ANSWERED, ACCEPTED, FIPSD_TR_PUNCHING, 0},
    {FIPSD_TR_ANSWER, FIPSD_TR_ANSWERED, REJECTED, FIPSD_TR_COOLDOWN, FIPSD_TR_FAILED | FIPSD_TR_FINISHED},
    {FIPSD_TR_PUNCHING, FIPSD_TR_ANSWERED, REJECTED, FIPSD_TR_COOLDOWN, FIPSD_TR_FAILED | FIPSD_TR_FINISHED},
    {FIPSD_TR_SETTLING, FIPSD_TR_ANSWERED, REJECTED, FIPSD_TR_COOLDOWN, FIPSD_TR_FAILED | FIPSD_TR_FINISHED},
    {FIPSD_TR_ANSWER, FIPSD_TR_TIMEOUT, EXPIRED, FIPSD_TR_COOLDOWN, FIPSD_TR_FAILED | FIPSD_TR_FINISHED},
    {FIPSD_TR_PUNCHING, FIPSD_TR_PUNCH, EXACT, FIPSD_TR_ESTABLISHED, FIPSD_TR_HANDOFF},
    {FIPSD_TR_SETTLING, FIPSD_TR_PUNCH, EXACT, FIPSD_TR_ESTABLISHED, FIPSD_TR_HANDOFF},
    {FIPSD_TR_PUNCHING, FIPSD_TR_PUNCH, REMAPPED, FIPSD_TR_SETTLING, 0},
    {FIPSD_TR_SETTLING, FIPSD_TR_PUNCH, REMAPPED, FIPSD_TR_SETTLING, 0},
    {FIPSD_TR_ESTABLISHED, FIPSD_TR_PUNCH, EXACT, FIPSD_TR_ESTABLISHED, 0},
    {FIPSD_TR_SETTLING, FIPSD_TR_TIMEOUT, SETTLED, FIPSD_TR_ESTABLISHED, FIPSD_TR_HANDOFF},
    {FIPSD_TR_PUNCHING, FIPSD_TR_TIMEOUT, EXPIRED, FIPSD_TR_COOLDOWN, FIPSD_TR_FAILED | FIPSD_TR_FINISHED},
    {FIPSD_TR_PUNCHING, FIPSD_TR_TIMEOUT, SEND_DUE, FIPSD_TR_PUNCHING, FIPSD_TR_SEND_PUNCH},
    {FIPSD_TR_SETTLING, FIPSD_TR_TIMEOUT, SEND_DUE, FIPSD_TR_SETTLING, FIPSD_TR_SEND_PUNCH},
    {FIPSD_TR_ESTABLISHED, FIPSD_TR_TIMEOUT, EXPIRED, FIPSD_TR_IDLE, FIPSD_TR_FINISHED},
    {FIPSD_TR_COOLDOWN, FIPSD_TR_TIMEOUT, DUE, FIPSD_TR_IDLE, FIPSD_TR_RETRY},
    CANCEL_ROW(FIPSD_TR_IDLE), CANCEL_ROW(FIPSD_TR_ANSWER), CANCEL_ROW(FIPSD_TR_PUNCHING),
    CANCEL_ROW(FIPSD_TR_SETTLING), CANCEL_ROW(FIPSD_TR_ESTABLISHED), CANCEL_ROW(FIPSD_TR_COOLDOWN)
};
static bool matches(guard g, const fipsd_tr_machine *m, fipsd_tr_input in, uint64_t now)
{
    switch (g) {
    case ALWAYS: return true;
    case OUTBOUND: return in.initiator;
    case INBOUND: return !in.initiator;
    case ACCEPTED: return in.accepted;
    case REJECTED: return !in.accepted && (!in.repair || m->state == FIPSD_TR_ANSWER);
    case EXACT: return in.rank == 2 && now < m->deadline;
    case REMAPPED: return in.rank == 1 && now < m->deadline;
    case DUE: case EXPIRED: return now >= m->deadline;
    case SETTLED: return now >= m->settle_at || now >= m->deadline;
    case SEND_DUE: return now >= m->next_punch && now < m->send_until;
    }
    return false;
}
unsigned fipsd_tr_step(fipsd_tr_machine *m, fipsd_tr_input in, uint64_t now)
{
    /* A delayed wake can skip the last send opportunity; do not keep selecting
     * that past cadence after its window has closed. */
    if (in.event == FIPSD_TR_TIMEOUT &&
        (m->state == FIPSD_TR_PUNCHING || m->state == FIPSD_TR_SETTLING) && now >= m->send_until)
        m->next_punch = UINT64_MAX;
    for (size_t i = 0; i < sizeof table / sizeof *table; ++i) {
        const transition *t = &table[i];
        if (m->state != t->from || in.event != t->event || !matches(t->when, m, in, now)) continue;
        fipsd_tr_state old = m->state; m->state = t->to; unsigned effects = t->effects;
        if (in.event == FIPSD_TR_OFFER) { m->initiator = in.initiator; ++m->attempts; m->sequence = 0; }
        if (t->to == FIPSD_TR_ANSWER) m->deadline = add(now, m->config.answer_ms);
        if (t->to == FIPSD_TR_PUNCHING && old != FIPSD_TR_PUNCHING && old != FIPSD_TR_SETTLING) {
            m->deadline = add(now, m->config.attempt_ms);
            m->next_punch = add(now, m->config.start_delay_ms);
            m->send_until = add(m->next_punch, m->config.duration_ms ? m->config.duration_ms : 1);
        }
        if (t->to == FIPSD_TR_SETTLING && old != FIPSD_TR_SETTLING) m->settle_at = add(now, 250);
        if (effects & FIPSD_TR_SEND_PUNCH) {
            ++m->sequence; m->next_punch = add(now, m->config.interval_ms < 20 ? 20 : m->config.interval_ms);
        }
        if (in.event == FIPSD_TR_PUNCH && in.probe) effects |= FIPSD_TR_SEND_ACK;
        if (effects & FIPSD_TR_HANDOFF) m->failures = 0;
        if (effects & FIPSD_TR_FAILED) {
            if (m->failures != UINT32_MAX) ++m->failures;
            uint64_t delay = m->config.retry_ms;
            for (uint32_t j = 1; j < m->attempts && delay < m->config.max_retry_ms; ++j) delay = add(delay, delay);
            if (delay > m->config.max_retry_ms) delay = m->config.max_retry_ms;
            if (m->failures >= m->config.failure_threshold && delay < m->config.cooldown_ms) delay = m->config.cooldown_ms;
            m->deadline = add(now, delay);
        }
        if ((effects & FIPSD_TR_RETRY) && (!m->initiator || m->attempts > m->config.retries)) effects &= ~FIPSD_TR_RETRY;
        return effects;
    }
    return 0;
}
uint64_t fipsd_tr_deadline(const fipsd_tr_machine *m)
{
    if (m->state == FIPSD_TR_IDLE || m->state == FIPSD_TR_CANCELLED) return UINT64_MAX;
    uint64_t next = m->deadline;
    if ((m->state == FIPSD_TR_PUNCHING || m->state == FIPSD_TR_SETTLING) &&
        m->next_punch < m->send_until && m->next_punch < next) next = m->next_punch;
    if (m->state == FIPSD_TR_SETTLING && m->settle_at < next) next = m->settle_at;
    return next;
}
