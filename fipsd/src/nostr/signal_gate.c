#include "nostr/signal_gate.h"
void fipsd_signal_gate_init(fipsd_signal_gate *g, uint64_t now)
{ *g = (fipsd_signal_gate){20000, 20000, now, now}; }
static void refill(uint64_t *tokens, uint64_t *at, uint64_t now)
{
    if (now <= *at) return;
    uint64_t elapsed = now - *at;
    *tokens = elapsed >= 4000 || *tokens + elapsed * 5 >= 20000 ? 20000 : *tokens + elapsed * 5;
    *at = now;
}
fipsd_signal_admission fipsd_signal_gate_admit(fipsd_signal_gate *g, bool awaiting, uint64_t now)
{
    refill(&g->shared, &g->shared_at, now);
    if (g->shared >= 1000) { g->shared -= 1000; return FIPSD_SIGNAL_ADMIT; }
    if (!awaiting) return FIPSD_SIGNAL_SHED_SHARED;
    refill(&g->reserve, &g->reserve_at, now);
    if (g->reserve >= 1000) { g->reserve -= 1000; return FIPSD_SIGNAL_ADMIT; }
    return FIPSD_SIGNAL_SHED_RESERVE;
}
