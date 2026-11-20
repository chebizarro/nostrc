/* The dataplane's published coordinate cache (nostrc-x345n); see dataplane.h.
 *
 * Forwarding used to scan every published entry with a 16-byte compare per
 * packet, and any cache change re-copied every live entry (~17 ms at 50k) on
 * the next publish. A section now spreads the rows over groups x 64 parts by
 * SipHash of the address under a per-process key (hint keys are peer-chosen,
 * so a fixed hash could be flooded into one part), and an update rebuilds only
 * the parts holding a row the core reports changed: the parts and groups it
 * did not touch are shared with the previous section by reference count. */
#include "node/handlers/internal.h"
#include <sodium/crypto_shorthash_siphash24.h>
#include <stdatomic.h>

#define NONE UINT32_MAX
#define FANOUT FIPSD_DP_CACHE_FANOUT
#define ROWS_PER_PART 16u /* the most a full cache averages */

static uint64_t addr_hash(const uint8_t key[16], const uint8_t addr[16])
{
    uint8_t d[crypto_shorthash_siphash24_BYTES];
    crypto_shorthash_siphash24(d, addr, 16, key);
    uint64_t h = 0;
    for (size_t i = 0; i < sizeof d; ++i) h |= (uint64_t)d[i] << (8 * i);
    return h;
}
static uint32_t part_index(size_t group_mask, uint64_t h)
{
    return (uint32_t)(((h >> 6) & group_mask) * FANOUT + (h & (FANOUT - 1)));
}
static uint32_t tag_of(uint64_t h) { return (uint32_t)(h >> 32); }

static void part_retain(fipsd_dp_cache_part *p)
{
    if (p) atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed);
}
static void part_release(fipsd_dp_cache_part *p)
{
    if (p && atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) == 1) free(p);
}
static void group_retain(fipsd_dp_cache_group *g)
{
    if (g) atomic_fetch_add_explicit(&g->refs, 1, memory_order_relaxed);
}
static void group_release(fipsd_dp_cache_group *g)
{
    if (!g || atomic_fetch_sub_explicit(&g->refs, 1, memory_order_acq_rel) != 1) return;
    for (size_t i = 0; i < FANOUT; ++i) part_release(g->parts[i]);
    free(g);
}
fipsd_dp_cache *fipsd_dp_cache_retain(fipsd_dp_cache *s)
{
    if (s) atomic_fetch_add_explicit(&s->refs, 1, memory_order_relaxed);
    return s;
}
void fipsd_dp_cache_release(fipsd_dp_cache *s)
{
    if (!s || atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) != 1) return;
    for (size_t g = 0; g <= s->group_mask; ++g) group_release(s->groups[g]);
    free(s);
}

static const fipsd_dp_cache_part *section_part(const fipsd_dp_cache *s, uint32_t part)
{
    const fipsd_dp_cache_group *g = s ? s->groups[part / FANOUT] : NULL;
    return g ? g->parts[part % FANOUT] : NULL;
}
const fipsd_dp_cached *fipsd_dp_cache_find(const fipsd_dp_cache *s, const fips_node_addr *addr, uint64_t now_ms)
{
    if (!s) return NULL;
    uint64_t h = addr_hash(s->key, addr->bytes);
    const fipsd_dp_cache_part *p = section_part(s, part_index(s->group_mask, h));
    if (!p) return NULL;
    uint32_t tag = tag_of(h);
    for (uint32_t i = 0; i < p->count; ++i) {
        if (p->tags[i] != tag || memcmp(p->rows[i].addr.bytes, addr->bytes, 16)) continue;
        return now_ms > p->rows[i].expires ? NULL : &p->rows[i];
    }
    return NULL;
}
void fipsd_dp_cache_walk(const fipsd_dp_cache *s, fipsd_dp_cache_visitor visit, void *ctx)
{
    if (!s) return;
    for (size_t g = 0; g <= s->group_mask; ++g) {
        if (!s->groups[g]) continue;
        for (size_t j = 0; j < FANOUT; ++j) {
            const fipsd_dp_cache_part *p = s->groups[g]->parts[j];
            for (uint32_t i = 0; p && i < p->count; ++i)
                if (!visit(ctx, &p->rows[i])) return;
        }
    }
}

void fipsd_dp_cache_view_init(fipsd_dp_cache_view *v, fips_entropy entropy)
{
    memset(v, 0, sizeof *v);
    entropy.fill(entropy.ctx, v->key, sizeof v->key);
}
/* Forget the section and the per-slot state; keep the key and the version
 * (snapshot fingerprints fold it, so it must never repeat). */
static void view_drop(fipsd_dp_cache_view *v)
{
    fipsd_dp_cache_release(v->section);
    free(v->slot_part); free(v->slot_mark); free(v->part_mark); free(v->part_count);
    free(v->part_start); free(v->dirty_parts); free(v->group_mark); free(v->order);
    free(v->change); free(v->built);
    uint8_t key[16]; memcpy(key, v->key, sizeof key);
    uint64_t version = v->version;
    memset(v, 0, sizeof *v);
    memcpy(v->key, key, sizeof key); v->version = version;
}
void fipsd_dp_cache_view_free(fipsd_dp_cache_view *v) { view_drop(v); }
static bool view_alloc(fipsd_dp_cache_view *v, size_t slots)
{
    size_t groups = 1;
    while (groups * FANOUT * ROWS_PER_PART < slots) groups *= 2;
    v->slots = slots; v->groups = groups; v->parts = groups * FANOUT;
    v->slot_part = malloc((slots ? slots : 1) * sizeof *v->slot_part);
    v->slot_mark = calloc(slots ? slots : 1, sizeof *v->slot_mark);
    v->order = malloc((slots ? slots : 1) * sizeof *v->order);
    v->change = malloc((slots ? slots : 1) * sizeof *v->change);
    v->part_mark = calloc(v->parts, sizeof *v->part_mark);
    v->part_count = calloc(v->parts, sizeof *v->part_count);
    v->part_start = calloc(v->parts, sizeof *v->part_start);
    v->dirty_parts = calloc(v->parts, sizeof *v->dirty_parts);
    v->built = calloc(v->parts, sizeof *v->built);
    v->group_mark = calloc(groups, sizeof *v->group_mark);
    if (!v->slot_part || !v->slot_mark || !v->order || !v->change || !v->part_mark || !v->part_count ||
        !v->part_start || !v->dirty_parts || !v->built || !v->group_mark) { view_drop(v); return false; }
    for (size_t i = 0; i < slots; ++i) v->slot_part[i] = NONE;
    return true;
}

static const fipsd_dp_cached *row_of_slot(const fipsd_dp_cache_part *p, uint32_t slot)
{
    for (uint32_t i = 0; p && i < p->count; ++i) if (p->rows[i].slot == slot) return &p->rows[i];
    return NULL;
}
static bool same_row(const fipsd_dp_cached *r, const uint8_t addr[16], const fips_cache_entry *e)
{
    return r && !memcmp(r->addr.bytes, addr, 16) && r->expires == e->expires_at && r->len == e->coords.len &&
        !memcmp(r->coords, e->coords.entries, r->len * sizeof *r->coords);
}
static void dirty(fipsd_dp_cache_view *v, uint32_t part)
{
    if (v->part_mark[part] == v->serial) return;
    v->part_mark[part] = v->serial; v->part_count[part] = 0;
    v->dirty_parts[v->dirty++] = part;
    v->group_mark[part / FANOUT] = v->serial;
}
/* One slot the core reports changed: unless its row would come out the same
 * (a demote, say, changes nothing published), the part that held its row and
 * the part its new row hashes to are rebuilt. */
static bool note(void *ctx, size_t slot, const uint8_t *addr, const fips_cache_entry *e)
{
    fipsd_dp_cache_view *v = ctx;
    uint32_t was = v->slot_part[slot], part = NONE, tag = 0;
    if (addr) { uint64_t h = addr_hash(v->key, addr); part = part_index(v->groups - 1, h); tag = tag_of(h); }
    if (was == NONE && part == NONE) return true;
    if (was == part && same_row(row_of_slot(section_part(v->section, was), (uint32_t)slot), addr, e)) return true;
    v->slot_mark[slot] = v->serial;
    if (was != NONE) dirty(v, was);
    if (part != NONE) { dirty(v, part); ++v->part_count[part]; }
    v->change[v->changes++] = (fipsd_dp_cache_change){addr, e, (uint32_t)slot, part, tag};
    return true;
}
/* A dirty part's rows: the previous part's, less those of changed slots, plus
 * the changed slots' new rows. NULL with *ok when nothing is left. */
static fipsd_dp_cache_part *build_part(fipsd_dp_cache_view *v, uint32_t part, bool *ok)
{
    const fipsd_dp_cache_part *old = section_part(v->section, part);
    uint32_t count = 0, end = v->part_start[part], begin = end - v->part_count[part];
    size_t coords = 0;
    for (uint32_t i = 0; old && i < old->count; ++i)
        if (v->slot_mark[old->rows[i].slot] != v->serial) { ++count; coords += old->rows[i].len; }
    for (uint32_t k = begin; k < end; ++k) { ++count; coords += v->change[v->order[k]].entry->coords.len; }
    *ok = true;
    if (!count) return NULL;
    size_t tags = ((size_t)count + 1) & ~(size_t)1; /* rows stay 8-byte aligned */
    fipsd_dp_cache_part *p = malloc(sizeof *p + tags * sizeof(uint32_t) + count * sizeof(fipsd_dp_cached) +
                                    coords * sizeof(fips_cached_coord_entry));
    if (!p) { *ok = false; return NULL; }
    uint32_t *tag = (uint32_t *)(p + 1);
    fipsd_dp_cached *rows = (fipsd_dp_cached *)(tag + tags);
    fips_cached_coord_entry *pool = (fips_cached_coord_entry *)(rows + count);
    uint32_t n = 0;
    for (uint32_t i = 0; old && i < old->count; ++i) {
        const fipsd_dp_cached *r = &old->rows[i];
        if (v->slot_mark[r->slot] == v->serial) continue;
        tag[n] = old->tags[i]; rows[n] = *r; rows[n].coords = pool;
        memcpy(pool, r->coords, r->len * sizeof *pool); pool += r->len; ++n;
    }
    for (uint32_t k = begin; k < end; ++k) {
        const fipsd_dp_cache_change *c = &v->change[v->order[k]];
        tag[n] = c->tag;
        rows[n] = (fipsd_dp_cached){.expires = c->entry->expires_at, .coords = pool, .slot = c->slot, .len = c->entry->coords.len};
        memcpy(rows[n].addr.bytes, c->addr, 16);
        memcpy(pool, c->entry->coords.entries, c->entry->coords.len * sizeof *pool); pool += c->entry->coords.len; ++n;
    }
    atomic_init(&p->refs, 1);
    p->count = count; p->tags = tag; p->rows = rows;
    return p;
}
static void release_built(fipsd_dp_cache_view *v, size_t upto)
{
    for (size_t i = 0; i < upto; ++i) part_release(v->built[v->dirty_parts[i]]);
}
bool fipsd_dp_cache_view_update(fipsd_dp_cache_view *v, const fips_coord_cache *c)
{
    uint64_t generation = fips_coord_cache_generation(c);
    if (v->slots != c->slot_capacity || generation < v->generation) {
        view_drop(v); /* another cache (a re-init restarts the generation): from nothing */
        if (!view_alloc(v, c->slot_capacity)) return false;
    } else if (generation == v->generation) {
        return true;
    }
    if (++v->serial == 0) { /* marks are compared for equality: clear them on wrap */
        memset(v->slot_mark, 0, v->slots * sizeof *v->slot_mark);
        memset(v->part_mark, 0, v->parts * sizeof *v->part_mark);
        memset(v->group_mark, 0, v->groups * sizeof *v->group_mark);
        v->serial = 1;
    }
    v->changes = v->dirty = 0;
    fips_coord_cache_changes(c, v->generation, note, v);
    if (!v->dirty) { v->generation = generation; return true; }
    /* Group the new rows by part: start offsets, then each change in place. */
    uint32_t run = 0;
    for (size_t i = 0; i < v->dirty; ++i) {
        uint32_t p = v->dirty_parts[i];
        v->part_start[p] = run; run += v->part_count[p];
    }
    for (size_t i = 0; i < v->changes; ++i)
        if (v->change[i].part != NONE) v->order[v->part_start[v->change[i].part]++] = (uint32_t)i;
    for (size_t i = 0; i < v->dirty; ++i) {
        bool ok; uint32_t p = v->dirty_parts[i];
        v->built[p] = build_part(v, p, &ok);
        if (!ok) { release_built(v, i); return false; }
    }
    const fipsd_dp_cache *old = v->section;
    fipsd_dp_cache *s = calloc(1, sizeof *s + v->groups * sizeof s->groups[0]);
    bool ok = s != NULL;
    for (size_t g = 0; ok && g < v->groups; ++g)
        if (v->group_mark[g] == v->serial && !(s->groups[g] = calloc(1, sizeof *s->groups[g]))) ok = false;
    if (!ok) {
        for (size_t g = 0; s && g < v->groups; ++g) if (v->group_mark[g] == v->serial) free(s->groups[g]);
        free(s); release_built(v, v->dirty);
        return false;
    }
    atomic_init(&s->refs, 1);
    memcpy(s->key, v->key, sizeof s->key);
    s->group_mask = v->groups - 1;
    s->count = old ? old->count : 0;
    for (size_t i = 0; i < v->dirty; ++i) {
        uint32_t p = v->dirty_parts[i];
        const fipsd_dp_cache_part *was = section_part(old, p);
        s->count = s->count - (was ? was->count : 0) + (v->built[p] ? v->built[p]->count : 0);
    }
    for (size_t g = 0; g < v->groups; ++g) {
        fipsd_dp_cache_group *og = old ? old->groups[g] : NULL;
        if (v->group_mark[g] != v->serial) { group_retain(og); s->groups[g] = og; continue; }
        fipsd_dp_cache_group *ng = s->groups[g];
        bool any = false;
        atomic_init(&ng->refs, 1);
        for (size_t j = 0; j < FANOUT; ++j) {
            uint32_t p = (uint32_t)(g * FANOUT + j);
            if (v->part_mark[p] == v->serial) ng->parts[j] = v->built[p];
            else { ng->parts[j] = og ? og->parts[j] : NULL; part_retain(ng->parts[j]); }
            any = any || ng->parts[j];
        }
        if (!any) { free(ng); s->groups[g] = NULL; }
    }
    for (size_t i = 0; i < v->changes; ++i) v->slot_part[v->change[i].slot] = v->change[i].part;
    fipsd_dp_cache_release(v->section);
    v->section = s; ++v->version; v->generation = generation;
    return true;
}
