#include "peer/table.h"
#include "peer/link.h"
#include "swap.h"
#include <stdlib.h>
#include <string.h>
typedef struct table_entry { fipsd_active_peer *peer; fipsd_peer_ticket ticket; } table_entry;
struct fipsd_peer_table {
    table_entry *entries;
    size_t count, capacity;
    fipsd_peer_budget *budget;
    GoSwap *published;
    uint64_t generation;
};
static size_t lower(const fipsd_peer_table *t, const fips_node_addr *addr)
{
    size_t lo = 0, hi = t->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const fips_node_addr *key = &fipsd_active_peer_info(t->entries[mid].peer)->identity.node_addr;
        if (memcmp(key->bytes, addr->bytes, sizeof(key->bytes)) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
fipsd_peer_table *fipsd_peer_table_create(size_t capacity, fipsd_peer_caps cap,
    const fipsd_transport_caps *limits, size_t n)
{
    if (!capacity || capacity > SIZE_MAX / sizeof(table_entry) ||
        capacity > (SIZE_MAX - sizeof(fipsd_peer_table_snapshot)) / sizeof(fipsd_peer_info) ||
        n > SIZE_MAX - capacity) return NULL;
    fipsd_peer_table *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->entries = calloc(capacity, sizeof(*t->entries)); t->capacity = capacity;
    t->budget = fipsd_peer_budget_create(cap, limits, n, capacity + n);
    t->published = go_swap_create(NULL, free);
    if (!t->entries || !t->budget || !t->published || !fipsd_peer_table_publish(t)) {
        (void)fipsd_peer_budget_destroy(t->budget); go_swap_free(t->published);
        free(t->entries); free(t); return NULL;
    }
    return t;
}
bool fipsd_peer_table_destroy(fipsd_peer_table *t)
{
    if (!t) return true;
    if (fipsd_peer_budget_used(t->budget).connections) return false;
    for (size_t i = 0; i < t->count; ++i)
        if (fipsd_active_peer_link_attached(t->entries[i].peer)) return false;
    for (size_t i = 0; i < t->count; ++i) {
        fipsd_peer_budget_release(&t->entries[i].ticket);
        (void)fipsd_active_peer_destroy(t->entries[i].peer);
    }
    (void)fipsd_peer_budget_destroy(t->budget); go_swap_free(t->published);
    free(t->entries); free(t); return true;
}
fipsd_active_peer *fipsd_peer_table_find(fipsd_peer_table *t, const fips_node_addr *a)
{
    size_t i = lower(t, a);
    if (i == t->count) return NULL;
    fipsd_active_peer *p = t->entries[i].peer;
    return !memcmp(fipsd_active_peer_info(p)->identity.node_addr.bytes, a->bytes, sizeof(a->bytes)) ? p : NULL;
}
fipsd_active_peer *fipsd_peer_table_endpoint(fipsd_peer_table *t,
    fips_transport_id transport, const fips_transport_addr *a)
{
    if (a->len > FIPS_TRANSPORT_ADDR_CAPACITY) return NULL;
    for (size_t i = 0; i < t->count; ++i) {
        const fipsd_peer_info *p = fipsd_active_peer_info(t->entries[i].peer);
        if (fips_transport_id_equal(p->transport, transport) && fips_transport_addr_equal(&p->addr, a)) return t->entries[i].peer;
    }
    return NULL;
}
fipsd_peer_admission fipsd_peer_table_reserve(fipsd_peer_table *t,
    fips_transport_id transport, fipsd_peer_ticket *ticket)
{
    if (ticket->owner) return FIPSD_ADMIT_BUSY;
    if (fipsd_peer_budget_used(t->budget).connections >= t->capacity) return FIPSD_ADMIT_STORAGE;
    return fipsd_peer_budget_acquire(t->budget, transport, true, ticket);
}
fipsd_peer_admission fipsd_peer_table_insert_reserved(fipsd_peer_table *t,
    fipsd_active_peer *p, fipsd_peer_ticket *reservation)
{
    const fipsd_peer_info *info = fipsd_active_peer_info(p);
    if (fipsd_peer_table_find(t, &info->identity.node_addr) || fipsd_peer_table_endpoint(t, info->transport, &info->addr))
        return FIPSD_ADMIT_DUPLICATE;
    if (t->count == t->capacity) return FIPSD_ADMIT_STORAGE;
    fipsd_peer_ticket ticket = {0};
    fipsd_peer_admission result;
    if (reservation) {
        if (reservation->owner != t->budget || !reservation->pending ||
            !fips_transport_id_equal(reservation->transport, info->transport)) return FIPSD_ADMIT_BUSY;
        result = fipsd_peer_budget_promote(reservation);
        if (result != FIPSD_ADMIT_OK) return result;
        ticket = *reservation; *reservation = (fipsd_peer_ticket){0};
    } else {
        result = fipsd_peer_budget_acquire(t->budget, info->transport, false, &ticket);
        if (result != FIPSD_ADMIT_OK) return result;
    }
    size_t i = lower(t, &info->identity.node_addr);
    memmove(&t->entries[i + 1], &t->entries[i], (t->count - i) * sizeof(*t->entries));
    /* Tickets contain values only; moving transfers their unique ownership. */
    t->entries[i] = (table_entry){p, ticket}; ++t->count;
    return FIPSD_ADMIT_OK;
}
fipsd_peer_admission fipsd_peer_table_insert(fipsd_peer_table *t, fipsd_active_peer *p)
{ return fipsd_peer_table_insert_reserved(t, p, NULL); }
fipsd_peer_admission fipsd_peer_table_remove(fipsd_peer_table *t, const fips_node_addr *addr)
{
    fipsd_active_peer *p = fipsd_peer_table_find(t, addr);
    if (!p) return FIPSD_ADMIT_DUPLICATE;
    if (fipsd_active_peer_link_attached(p)) return FIPSD_ADMIT_BUSY;
    size_t i = lower(t, addr);
    (void)fipsd_active_peer_destroy(p); fipsd_peer_budget_release(&t->entries[i].ticket);
    memmove(&t->entries[i], &t->entries[i + 1], (t->count - i - 1) * sizeof(*t->entries));
    --t->count; memset(&t->entries[t->count], 0, sizeof(*t->entries));
    return FIPSD_ADMIT_OK;
}
fipsd_peer_admission fipsd_peer_table_rebind(fipsd_peer_table *t, const fips_node_addr *node,
    fips_transport_id transport, const fips_transport_addr *addr)
{
    if (addr->len > FIPS_TRANSPORT_ADDR_CAPACITY) return FIPSD_ADMIT_STORAGE;
    fipsd_active_peer *p = fipsd_peer_table_find(t, node);
    if (!p) return FIPSD_ADMIT_DUPLICATE;
    fipsd_active_peer *collision = fipsd_peer_table_endpoint(t, transport, addr);
    if (collision && collision != p) return FIPSD_ADMIT_DUPLICATE;
    /* The dispatcher owns both the link and its send endpoint. A Bundle switch
     * moves that endpoint, not the link/keys/index leases. Collision and budget
     * checks precede mutation, including when a link is attached. */
    size_t i = lower(t, node);
    fipsd_peer_admission result = fipsd_peer_budget_rebind(&t->entries[i].ticket, transport);
    if (result == FIPSD_ADMIT_OK) fipsd_active_peer_set_endpoint(p, transport, addr);
    return result;
}
size_t fipsd_peer_table_count(const fipsd_peer_table *t) { return t->count; }
fipsd_active_peer *fipsd_peer_table_at(fipsd_peer_table *t, size_t i)
{ return i < t->count ? t->entries[i].peer : NULL; }
bool fipsd_peer_table_publish(fipsd_peer_table *t)
{
    fipsd_peer_table_snapshot *s = malloc(sizeof(*s) + t->count * sizeof(*s->peers));
    if (!s) return false;
    s->generation = t->generation + 1; s->count = t->count;
    for (size_t i = 0; i < t->count; ++i) s->peers[i] = *fipsd_active_peer_info(t->entries[i].peer);
    if (!go_swap_store(t->published, s)) { free(s); return false; }
    ++t->generation; return true;
}
GoRefPtr fipsd_peer_table_snapshot_load(fipsd_peer_table *t) { return go_swap_load(t->published); }
