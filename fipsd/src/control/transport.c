#include "control/transport.h"
#include <string.h>
void fipsd_control_transport_read(const fipsd_transport *t, uint64_t now,
                                 fipsd_control_transport_view *out)
{
    memset(out, 0, sizeof *out);
    fipsd_transport_state state = t->ops->state(t);
    const char *label = state == FIPSD_TRANSPORT_FAILED ? "failed" :
        state == FIPSD_TRANSPORT_BOUND || state == FIPSD_TRANSPORT_ABSENT ? "up" :
        t->shared && atomic_load(&t->shared->started) ? "down" : "configured";
    fips_transport_addr address = {0};
    out->row = (fipsd_control_transport){.transport_id = t->id.value, .type = t->type,
        .name = t->name, .state = label, .mtu = t->ops->mtu(t, &address)};
    if (t->ops->local_addr && t->ops->local_addr(t, &address)) {
        size_t n = 0;
        if (fips_transport_addr_format(&address, out->local_addr, sizeof out->local_addr, &n) == FIPS_OK)
            out->row.local_addr = out->local_addr;
    }
    if (!t->shared) return;
    fipsd_transport_stats_view stats = fipsd_transport_stats_read(&t->shared->stats);
    size_t n = 0;
#define COUNTER(name) out->counters[n++] = (fipsd_control_counter){#name, stats.name};
    FIPSD_STATS_ALL(COUNTER)
#undef COUNTER
    out->row.stats = (fipsd_control_counters){out->counters, n};
    if (t->interface_name) {
        fipsd_presence_view v = fipsd_presence_snapshot(&t->shared->presence);
        out->interface = (fipsd_control_interface){.name = t->interface_name,
            .presence = fipsd_presence_name(v.phase), .policy = v.optional ? "optional" : "required",
            .carrier = v.interface.carrier, .medium = fipsd_medium_name(v.interface.medium),
            .since_secs = now >= v.since_ms ? (now - v.since_ms) / 1000 : 0,
            .binds = v.binds, .failed_attempts = v.attempts};
        out->row.interface = &out->interface;
    }
}
