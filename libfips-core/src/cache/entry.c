#include <fips/cache/coord_cache.h>
#include <string.h>

static uint64_t saturating_add(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

fips_status fips_cached_coord_from_addrs(fips_cached_coord *out, const uint8_t (*addrs)[16],
                                         size_t count)
{
    if (count == 0 || count > FIPS_CACHED_COORD_MAX_ENTRIES) return FIPS_ERR_RANGE;
    out->len = (uint16_t)count;
    for (size_t i = 0; i < count; ++i) {
        memcpy(out->entries[i].node_addr, addrs[i], 16);
        out->entries[i].sequence = 0;
        out->entries[i].timestamp = 0;
    }
    return FIPS_OK;
}

bool fips_cached_coord_equal(const fips_cached_coord *a, const fips_cached_coord *b)
{
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; ++i) {
        const fips_coord_entry *x = &a->entries[i], *y = &b->entries[i];
        if (memcmp(x->node_addr, y->node_addr, 16) != 0 || x->sequence != y->sequence ||
            x->timestamp != y->timestamp)
            return false;
    }
    return true;
}

bool fips_cached_coord_contains(const fips_cached_coord *c, const uint8_t addr[16])
{
    for (size_t i = 0; i < c->len; ++i)
        if (memcmp(c->entries[i].node_addr, addr, 16) == 0) return true;
    return false;
}

const uint8_t *fips_cached_coord_node_addr(const fips_cached_coord *c)
{
    return c->entries[0].node_addr;
}

const uint8_t *fips_cached_coord_root_id(const fips_cached_coord *c)
{
    return c->entries[c->len == 0 ? 0 : c->len - 1].node_addr;
}

void fips_cache_entry_init(fips_cache_entry *e, const fips_cached_coord *coords,
                           uint64_t now_ms, uint64_t ttl_ms)
{
    e->coords = *coords;
    e->created_at = now_ms;
    e->last_used = now_ms;
    e->expires_at = saturating_add(now_ms, ttl_ms);
    e->has_path_mtu = false;
    e->path_mtu = 0;
    e->source = FIPS_COORD_HINT;
    e->verified_until = 0;
}

void fips_cache_entry_init_verified(fips_cache_entry *e, const fips_cached_coord *coords,
                                    uint64_t now_ms, uint64_t ttl_ms)
{
    fips_cache_entry_init(e, coords, now_ms, ttl_ms);
    fips_cache_entry_mark_verified(e, now_ms);
}

bool fips_cache_entry_is_verified(const fips_cache_entry *e, uint64_t now_ms)
{
    return e->source == FIPS_COORD_VERIFIED && now_ms <= e->verified_until;
}

void fips_cache_entry_mark_verified(fips_cache_entry *e, uint64_t now_ms)
{
    e->source = FIPS_COORD_VERIFIED;
    e->verified_until = saturating_add(now_ms, FIPS_VERIFIED_TTL_MS);
}

void fips_cache_entry_mark_hint(fips_cache_entry *e)
{
    e->source = FIPS_COORD_HINT;
    e->verified_until = 0;
}

bool fips_cache_entry_is_expired(const fips_cache_entry *e, uint64_t now_ms)
{
    return now_ms > e->expires_at;
}

void fips_cache_entry_touch(fips_cache_entry *e, uint64_t now_ms)
{
    e->last_used = now_ms;
}

void fips_cache_entry_refresh(fips_cache_entry *e, uint64_t now_ms, uint64_t ttl_ms)
{
    e->expires_at = saturating_add(now_ms, ttl_ms);
    e->last_used = now_ms;
}

void fips_cache_entry_update(fips_cache_entry *e, const fips_cached_coord *coords,
                             uint64_t now_ms, uint64_t ttl_ms)
{
    e->coords = *coords;
    e->last_used = now_ms;
    e->expires_at = saturating_add(now_ms, ttl_ms);
    fips_cache_entry_mark_hint(e);
}

void fips_cache_entry_update_verified(fips_cache_entry *e, const fips_cached_coord *coords,
                                      uint64_t now_ms, uint64_t ttl_ms)
{
    fips_cache_entry_update(e, coords, now_ms, ttl_ms);
    fips_cache_entry_mark_verified(e, now_ms);
}

void fips_cache_entry_set_path_mtu(fips_cache_entry *e, uint16_t mtu)
{
    e->has_path_mtu = true;
    e->path_mtu = mtu;
}

void fips_cache_entry_clear_path_mtu(fips_cache_entry *e)
{
    e->has_path_mtu = false;
    e->path_mtu = 0;
}

uint64_t fips_cache_entry_idle_time(const fips_cache_entry *e, uint64_t now_ms)
{
    return now_ms > e->last_used ? now_ms - e->last_used : 0;
}

uint64_t fips_cache_entry_age(const fips_cache_entry *e, uint64_t now_ms)
{
    return now_ms > e->created_at ? now_ms - e->created_at : 0;
}

uint64_t fips_cache_entry_time_to_expiry(const fips_cache_entry *e, uint64_t now_ms)
{
    return e->expires_at > now_ms ? e->expires_at - now_ms : 0;
}

double fips_cache_stats_fill_ratio(const fips_cache_stats *s)
{
    return s->max_entries == 0 ? 0.0 : (double)s->entries / (double)s->max_entries;
}

const char *fips_cache_error_name(fips_cache_error e)
{
    switch (e) {
    case FIPS_CACHE_FULL: return "cache full";
    case FIPS_CACHE_NOT_FOUND: return "entry not found";
    case FIPS_CACHE_EXPIRED: return "entry expired";
    }
    return "unknown";
}
