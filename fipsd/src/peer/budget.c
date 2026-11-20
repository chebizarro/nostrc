#include "peer/budget.h"
#include <fips/proto/fmp/types.h>
#include <stdlib.h>

typedef struct transport_budget {
    fips_transport_id id;
    fipsd_peer_caps cap, used;
    bool live, configured;
} transport_budget;
struct fipsd_peer_budget {
    fipsd_peer_caps cap, used;
    size_t capacity;
    transport_budget *transports;
};
static bool admits(fipsd_peer_caps cap, fipsd_peer_caps used, bool pending)
{
    return pending ? (!cap.connections || used.connections < cap.connections) :
        ((!cap.peers || used.peers < cap.peers) && (!cap.links || used.links < cap.links));
}
static void count(fipsd_peer_caps *c, bool pending, bool add)
{
    if (pending) { if (add) ++c->connections; else --c->connections; }
    else if (add) { ++c->peers; ++c->links; }
    else { --c->peers; --c->links; }
}
static transport_budget *find(fipsd_peer_budget *b, fips_transport_id id)
{
    for (size_t i = 0; i < b->capacity; ++i)
        if (b->transports[i].live && fips_transport_id_equal(b->transports[i].id, id)) return &b->transports[i];
    return NULL;
}
static transport_budget *slot(fipsd_peer_budget *b, fips_transport_id id)
{
    transport_budget *t = find(b, id);
    if (t) return t;
    for (size_t i = 0; i < b->capacity; ++i) if (!b->transports[i].live) {
        b->transports[i] = (transport_budget){.id = id, .live = true};
        return &b->transports[i];
    }
    return NULL;
}
static void reclaim(transport_budget *t)
{
    if (!t->configured && !t->used.peers && !t->used.links && !t->used.connections) t->live = false;
}
fipsd_peer_budget *fipsd_peer_budget_create(fipsd_peer_caps cap,
    const fipsd_transport_caps *limits, size_t n, size_t capacity)
{
    if (n > capacity || !capacity || capacity > SIZE_MAX / sizeof(transport_budget)) return NULL;
    fipsd_peer_budget *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->transports = calloc(capacity, sizeof(*b->transports));
    if (!b->transports) { free(b); return NULL; }
    b->cap = cap; b->capacity = capacity;
    for (size_t i = 0; i < n; ++i) {
        if (find(b, limits[i].transport)) { free(b->transports); free(b); return NULL; }
        transport_budget *t = slot(b, limits[i].transport);
        t->cap = limits[i].caps; t->configured = true;
    }
    return b;
}
bool fipsd_peer_budget_destroy(fipsd_peer_budget *b)
{
    if (!b) return true;
    if (b->used.peers || b->used.links || b->used.connections) return false;
    free(b->transports); free(b); return true;
}
fipsd_peer_admission fipsd_peer_budget_acquire(fipsd_peer_budget *b,
    fips_transport_id id, bool pending, fipsd_peer_ticket *ticket)
{
    if (ticket->owner) return FIPSD_ADMIT_BUSY;
    if (!admits(b->cap, b->used, pending)) return FIPSD_ADMIT_TOTAL;
    transport_budget *t = slot(b, id);
    if (!t) return FIPSD_ADMIT_STORAGE;
    if (!admits(t->cap, t->used, pending)) return FIPSD_ADMIT_TRANSPORT;
    count(&b->used, pending, true); count(&t->used, pending, true);
    *ticket = (fipsd_peer_ticket){b, id, pending};
    return FIPSD_ADMIT_OK;
}
fipsd_peer_admission fipsd_peer_budget_promote(fipsd_peer_ticket *ticket)
{
    if (!ticket->owner || !ticket->pending) return FIPSD_ADMIT_BUSY;
    fipsd_peer_budget *b = ticket->owner;
    transport_budget *t = find(b, ticket->transport);
    if (!admits(b->cap, b->used, false)) return FIPSD_ADMIT_TOTAL;
    if (!admits(t->cap, t->used, false)) return FIPSD_ADMIT_TRANSPORT;
    count(&b->used, true, false); count(&t->used, true, false);
    count(&b->used, false, true); count(&t->used, false, true);
    ticket->pending = false; return FIPSD_ADMIT_OK;
}
fipsd_peer_admission fipsd_peer_budget_rebind(fipsd_peer_ticket *ticket, fips_transport_id id)
{
    if (!ticket->owner) return FIPSD_ADMIT_BUSY;
    if (fips_transport_id_equal(ticket->transport, id)) return FIPSD_ADMIT_OK;
    fipsd_peer_budget *b = ticket->owner;
    transport_budget *to = slot(b, id);
    if (!to) {
        transport_budget *from = find(b, ticket->transport);
        bool sole = ticket->pending ? from->used.connections == 1 && !from->used.peers :
            from->used.peers == 1 && !from->used.connections;
        if (from->configured || !sole) return FIPSD_ADMIT_STORAGE;
        from->id = id; ticket->transport = id; return FIPSD_ADMIT_OK;
    }
    if (!admits(to->cap, to->used, ticket->pending)) return FIPSD_ADMIT_TRANSPORT;
    transport_budget *from = find(b, ticket->transport);
    count(&to->used, ticket->pending, true); count(&from->used, ticket->pending, false);
    reclaim(from); ticket->transport = id; return FIPSD_ADMIT_OK;
}
void fipsd_peer_budget_release(fipsd_peer_ticket *ticket)
{
    if (!ticket->owner) return;
    fipsd_peer_budget *b = ticket->owner;
    transport_budget *t = find(b, ticket->transport);
    count(&b->used, ticket->pending, false); count(&t->used, ticket->pending, false);
    reclaim(t); *ticket = (fipsd_peer_ticket){0};
}
fipsd_peer_caps fipsd_peer_budget_used(const fipsd_peer_budget *b) { return b->used; }
void fipsd_peer_tokens_init(fipsd_peer_tokens *b, uint32_t rate, uint32_t burst,
    uint32_t initial, fips_instant now)
{
    *b = (fipsd_peer_tokens){(uint64_t)(initial < burst ? initial : burst) * 1000,
        (uint64_t)burst * 1000, rate, now};
}
uint64_t fipsd_peer_tokens_available(fipsd_peer_tokens *b, fips_instant now)
{
    uint64_t ms = fips_fmp_elapsed_ms(now, b->refill);
    uint64_t room = b->capacity - b->milli;
    if (ms) {
        if (b->per_sec && ms > room / b->per_sec) b->milli = b->capacity;
        else b->milli += ms * b->per_sec;
        fips_instant credited;
        if (b->refill.ops->checked_add(b->refill, fips_duration_from_millis(ms), &credited) == FIPS_OK)
            b->refill = credited;
        else b->refill = now;
    }
    return b->milli / 1000;
}
bool fipsd_peer_tokens_spend(fipsd_peer_tokens *b, uint64_t tokens, fips_instant now)
{
    if (tokens > fipsd_peer_tokens_available(b, now)) return false;
    b->milli -= tokens * 1000; return true;
}
