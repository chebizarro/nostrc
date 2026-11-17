#include "transport/transport.h"
#include "transport/stream.h"
#include "select.h"
#include <stdlib.h>
#include <string.h>
void fipsd_transport_shared_init(fipsd_transport *t, fipsd_transport_shared *s, bool optional)
{
    t->shared = s; atomic_init(&s->state, FIPSD_TRANSPORT_STOPPED);
    atomic_init(&s->started, false); fipsd_transport_diagnostics_init(&s->diagnostics);
    fipsd_presence_init(&s->presence, optional, fipsd_transport_now_ms());
    fipsd_transport_stats_init(&s->stats); nsync_mu_init(&s->links_mu);
    memset(s->links, 0, sizeof s->links);
}
fipsd_transport_state fipsd_transport_shared_state(const fipsd_transport *t)
{ return (fipsd_transport_state)atomic_load(&t->shared->state); }
fips_path_medium fipsd_transport_medium(const fipsd_transport *t)
{
    if (!t || !t->shared) return FIPS_PATH_Unknown;
    fipsd_presence_view v = fipsd_presence_snapshot(&t->shared->presence);
    return v.phase == FIPSD_PRESENT ? v.interface.medium : FIPS_PATH_Unknown;
}
int fipsd_transport_emit(GoChannel *events, GoContext *root, GoChannel *stop,
    fipsd_transport_event_kind kind, fips_transport_id id, const fips_transport_addr *addr,
    const uint8_t *bytes, size_t n)
{
    if (!events || !root || (n && !bytes) || n > UINT16_MAX || go_context_is_canceled(root) ||
        (stop && go_channel_is_closed(stop))) return -1;
    fipsd_transport_event *e = calloc(1, sizeof *e + n);
    if (!e) return -1;
    e->kind = kind; e->id = id; e->size = n;
    FIPSD_PERF_STAMP(e);
    if (addr) e->address = *addr;
    if (n) memcpy(e->bytes, bytes, n);
    void *v = NULL;
    GoSelectCase cases[] = {{GO_SELECT_RECEIVE, go_context_done(root), NULL, &v},
        {GO_SELECT_RECEIVE, stop, NULL, &v}, {GO_SELECT_SEND, events, e, NULL}};
    int selected = go_select(cases, 3);
    if (selected != 2) { free(e); return -1; }
    return 0;
}
static fipsd_transport_link *find(fipsd_transport_shared *s, const fips_transport_addr *a)
{
    for (size_t i = 0; i < FIPSD_TRANSPORT_LINKS; ++i)
        if (s->links[i].used && fips_transport_addr_equal(&s->links[i].address, a)) return &s->links[i];
    return NULL;
}
int fipsd_transport_link_add(fipsd_transport_shared *s, const fips_transport_addr *a,
    fips_link_direction d, uint16_t mtu, fips_link_id *out)
{
    if (!a || a->len > FIPS_TRANSPORT_ADDR_CAPACITY || !out) return -1;
    nsync_mu_lock(&s->links_mu);
    fipsd_transport_link *p = find(s, a);
    if (!p) for (size_t i = 0; i < FIPSD_TRANSPORT_LINKS; ++i) if (!s->links[i].used) { p = &s->links[i]; break; }
    if (!p) { nsync_mu_unlock(&s->links_mu); return -1; }
    if (p->used) fipsd_transport_stats_pool(&s->stats, p->direction, false);
    *p = (fipsd_transport_link){.used = true, .address = *a, .direction = d, .mtu = mtu,
        .id = {fipsd_stream_next_id()}};
    *out = p->id; fipsd_transport_stats_pool(&s->stats, d, true);
    nsync_mu_unlock(&s->links_mu); return 0;
}
bool fipsd_transport_link_remove(fipsd_transport_shared *s, const fips_transport_addr *a, fips_link_id id)
{
    nsync_mu_lock(&s->links_mu); fipsd_transport_link *p = find(s, a);
    bool own = p && fips_link_id_equal(p->id, id);
    if (own) { fipsd_transport_stats_pool(&s->stats, p->direction, false); p->used = false; }
    nsync_mu_unlock(&s->links_mu); return own;
}
bool fipsd_transport_link_get(fipsd_transport_shared *s, const fips_transport_addr *a, fipsd_transport_link *out)
{
    nsync_mu_lock(&s->links_mu); fipsd_transport_link *p = find(s, a);
    if (p) *out = *p;
    nsync_mu_unlock(&s->links_mu); return p != NULL;
}
void fipsd_transport_links_clear(fipsd_transport_shared *s)
{
    nsync_mu_lock(&s->links_mu);
    for (size_t i = 0; i < FIPSD_TRANSPORT_LINKS; ++i) if (s->links[i].used) {
        fipsd_transport_stats_pool(&s->stats, s->links[i].direction, false); s->links[i].used = false;
    }
    nsync_mu_unlock(&s->links_mu);
}
bool fipsd_transport_link_record(fipsd_transport_shared *s, const fips_transport_addr *a,
    fips_link_id id, size_t bytes, bool sent, uint64_t now)
{
    nsync_mu_lock(&s->links_mu); fipsd_transport_link *p = find(s, a);
    bool own = p && fips_link_id_equal(p->id, id);
    if (own) {
        if (sent) fips_link_stats_record_sent(&p->stats, bytes);
        else fips_link_stats_record_recv(&p->stats, bytes, now);
    }
    nsync_mu_unlock(&s->links_mu); return own;
}
