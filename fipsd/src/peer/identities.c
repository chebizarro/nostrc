#include "peer/identities.h"
#include <fips/identity/node_addr.h>
#include <stdlib.h>
#include <string.h>
struct fipsd_identities { fips_identity **items; size_t count, capacity; };
fipsd_identities *fipsd_identities_create(size_t capacity)
{
    if (!capacity || capacity > SIZE_MAX / sizeof(fips_identity *)) return NULL;
    fipsd_identities *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->items = calloc(capacity, sizeof(*s->items));
    if (!s->items) { free(s); return NULL; }
    s->capacity = capacity; return s;
}
void fipsd_identities_destroy(fipsd_identities *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->count; ++i) fips_identity_destroy(s->items[i]);
    free(s->items); free(s);
}
const fips_identity *fipsd_identities_find(const fipsd_identities *s, const fips_node_addr *addr)
{
    for (size_t i = 0; i < s->count; ++i) {
        fips_node_addr a = fips_identity_node_addr(s->items[i]);
        if (!memcmp(a.bytes, addr->bytes, sizeof(a.bytes))) return s->items[i];
    }
    return NULL;
}
bool fipsd_identities_add(fipsd_identities *s, fips_identity *id)
{
    if (!id || s->count == s->capacity) return false;
    fips_node_addr a = fips_identity_node_addr(id);
    if (fipsd_identities_find(s, &a)) return false;
    s->items[s->count++] = id; return true;
}
bool fipsd_identities_remove(fipsd_identities *s, const fips_node_addr *addr)
{
    const fips_identity *id = fipsd_identities_find(s, addr);
    if (!id) return false;
    for (size_t i = 0; i < s->count; ++i) if (s->items[i] == id) {
        fips_identity_destroy(s->items[i]);
        memmove(&s->items[i], &s->items[i + 1], (--s->count - i) * sizeof(*s->items));
        return true;
    }
    return false;
}
size_t fipsd_identities_count(const fipsd_identities *s) { return s->count; }
void fipsd_identity_cache_init(fipsd_identity_cache *c, fipsd_identity_entry *entries, size_t capacity)
{ *c = (fipsd_identity_cache){entries, 0, capacity}; }
static size_t cache_index(const fipsd_identity_cache *c, fips_node_prefix prefix)
{
    for (size_t i = 0; i < c->count; ++i) {
        fips_node_prefix p = fips_node_addr_prefix(&c->entries[i].peer.node_addr);
        if (!memcmp(p.bytes, prefix.bytes, sizeof(p.bytes))) return i;
    }
    return c->count;
}
bool fipsd_identity_cache_put(fipsd_identity_cache *c, fips_peer_identity p, uint64_t now_ms)
{
    size_t i = cache_index(c, fips_node_addr_prefix(&p.node_addr));
    if (i == c->count) { if (c->count == c->capacity) return false; ++c->count; }
    c->entries[i] = (fipsd_identity_entry){p, now_ms}; return true;
}
const fips_peer_identity *fipsd_identity_cache_touch(fipsd_identity_cache *c, fips_node_prefix prefix, uint64_t ms)
{
    size_t i = cache_index(c, prefix);
    if (i == c->count) return NULL;
    c->entries[i].used_ms = ms; return &c->entries[i].peer;
}
bool fipsd_identity_cache_remove(fipsd_identity_cache *c, fips_node_prefix prefix)
{
    size_t i = cache_index(c, prefix);
    if (i == c->count) return false;
    memmove(&c->entries[i], &c->entries[i + 1], (c->count - i - 1) * sizeof(*c->entries));
    --c->count; return true;
}
