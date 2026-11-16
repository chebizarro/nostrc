#include "gateway/pool.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULE "gateway"
#define MAX_POOL_BITS 16u

const char *fipsd_gw_pool_error_reason(fipsd_gw_pool_error error)
{
    switch (error) {
    case FIPSD_GW_POOL_AT_CEILING: return "ceiling";
    case FIPSD_GW_POOL_RATE_LIMITED: return "rate-limited";
    case FIPSD_GW_POOL_EXHAUSTED: return "exhausted";
    case FIPSD_GW_POOL_AWAITING_MARK: return "awaiting-mark";
    case FIPSD_GW_POOL_INSIDE_POOL: return "inside-pool";
    case FIPSD_GW_POOL_INVALID_CIDR: case FIPSD_GW_POOL_INVALID_PREFIX: return "invalid";
    case FIPSD_GW_POOL_OK: break;
    }
    return "ok";
}
const char *fipsd_gw_pool_error_text(fipsd_gw_pool_error error, size_t count, const struct in6_addr *addr,
                                     const char *cidr, char *buf, size_t cap)
{
    char text[48];
    switch (error) {
    case FIPSD_GW_POOL_INVALID_CIDR: snprintf(buf, cap, "invalid CIDR: %s", cidr ? cidr : ""); break;
    case FIPSD_GW_POOL_EXHAUSTED: snprintf(buf, cap, "pool exhausted (%zu addresses in use)", count); break;
    case FIPSD_GW_POOL_INVALID_PREFIX: snprintf(buf, cap, "prefix length must be between 1 and 127"); break;
    case FIPSD_GW_POOL_AT_CEILING: snprintf(buf, cap, "live-mapping ceiling reached (%zu mappings)", count); break;
    case FIPSD_GW_POOL_RATE_LIMITED: snprintf(buf, cap, "new-mapping rate limit reached"); break;
    case FIPSD_GW_POOL_AWAITING_MARK: snprintf(buf, cap, "pool state write not yet confirmed"); break;
    case FIPSD_GW_POOL_INSIDE_POOL:
        snprintf(buf, cap, "mesh address %s lies inside the pool prefix", addr ? fipsd_gw_ip6_text(addr, text, sizeof text) : "?"); break;
    case FIPSD_GW_POOL_OK: snprintf(buf, cap, "ok"); break;
    }
    return buf;
}

void fipsd_gw_pool_state_clear(fipsd_gw_pool_state *state) { free(state->held); memset(state, 0, sizeof *state); }
int fipsd_gw_pool_state_copy(fipsd_gw_pool_state *dst, const fipsd_gw_pool_state *src)
{
    fipsd_gw_pool_state_clear(dst);
    *dst = *src; dst->held = NULL; dst->held_len = 0;
    if (src->held_len) {
        dst->held = malloc(src->held_len * sizeof *dst->held);
        if (!dst->held) return -1;
        memcpy(dst->held, src->held, src->held_len * sizeof *dst->held);
        dst->held_len = src->held_len;
    }
    return 0;
}
bool fipsd_gw_pool_state_eq(const fipsd_gw_pool_state *a, const fipsd_gw_pool_state *b)
{
    if (a->version != b->version || strcmp(a->pool, b->pool) || a->total != b->total || a->from != b->from ||
        a->span != b->span || a->hold_secs != b->hold_secs || a->held_len != b->held_len) return false;
    for (size_t i = 0; i < a->held_len; ++i) if (a->held[i][0] != b->held[i][0] || a->held[i][1] != b->held[i][1]) return false;
    return true;
}

/* Token bucket for new mappings, in token-nanoseconds so refill is exact. */
typedef struct bucket { uint64_t level, capacity, rate; bool has_last; fipsd_gw_instant last; } bucket;
static void bucket_init(bucket *b, uint32_t capacity, uint32_t rate)
{
    b->capacity = (uint64_t)capacity * FIPSD_GW_SEC; b->level = b->capacity; b->rate = rate; b->has_last = false; b->last = 0;
}
static void bucket_refill(bucket *b, fipsd_gw_instant now)
{
    if (b->has_last) {
        uint64_t elapsed = fipsd_gw_since(now, b->last), add;
        if (b->rate && elapsed > b->capacity / b->rate) add = b->capacity;
        else add = elapsed * b->rate;
        b->level = add > b->capacity - b->level ? b->capacity : b->level + add;
    }
    /* Never move backwards, so a stale now cannot credit time twice. */
    if (!b->has_last || now > b->last) b->last = now;
    b->has_last = true;
}
static bool bucket_has_token(const bucket *b) { return b->level >= FIPSD_GW_SEC; }
static void bucket_take(bucket *b) { b->level = b->level >= FIPSD_GW_SEC ? b->level - FIPSD_GW_SEC : 0; }

typedef enum mark_mode { MARK_PENDING, MARK_ENFORCED, MARK_IGNORED } mark_mode;
typedef struct releasing { uint32_t offset; struct in6_addr mesh_addr; fipsd_gw_instant deadline; bool removed; uint32_t reads; } releasing;

struct fipsd_gw_pool {
    char cidr[64];
    struct in6_addr network; uint8_t prefix_len;
    uint8_t *free_bits; size_t free_count;
    fipsd_gw_mapping *slots; size_t slots_len, slots_cap, live;
    fipsd_gw_map by_node, by_vip; /* 16-byte keys -> slot index */
    uint64_t ttl_secs, grace_secs;
    uint32_t total; size_t ceiling;
    bucket bucket;
    uint32_t start; uint64_t cursor;
    bool has_mark; uint64_t durable_mark; mark_mode marks;
    uint8_t *held_bits; size_t held_count; bool has_hold_until; fipsd_gw_instant hold_until;
    fipsd_gw_instant clock;
    bool refusal_open; fipsd_gw_instant refusal_last; uint64_t refusal_count;
    uint64_t refused_total, evicted_total;
    releasing *releasing; size_t releasing_len, releasing_cap;
    fipsd_gw_map surviving; /* offset (4 bytes BE) | reply source -> consecutive misses */
    bool trusted; uint32_t unread, read;
    uint32_t reads_seen, reads_needed;
};

static bool bit_test(const uint8_t *bits, uint32_t i) { return (bits[i >> 3] >> (i & 7)) & 1; }
static void bit_set(uint8_t *bits, uint32_t i) { bits[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static void bit_clear(uint8_t *bits, uint32_t i) { bits[i >> 3] &= (uint8_t)~(1u << (i & 7)); }
static void free_add(fipsd_gw_pool *p, uint32_t o) { if (!bit_test(p->free_bits, o)) { bit_set(p->free_bits, o); p->free_count++; } }
static void free_remove(fipsd_gw_pool *p, uint32_t o) { if (bit_test(p->free_bits, o)) { bit_clear(p->free_bits, o); p->free_count--; } }
static void held_add(fipsd_gw_pool *p, uint32_t o) { if (!bit_test(p->held_bits, o)) { bit_set(p->held_bits, o); p->held_count++; } }

static uint32_t ring_step(uint32_t offset, uint64_t steps, uint32_t total)
{ return 1 + (uint32_t)(((uint64_t)offset - 1 + steps % total) % total); }
static uint32_t offset_at(const fipsd_gw_pool *p, uint64_t position) { return ring_step(p->start, position, p->total); }
static uint64_t ring_distance(const fipsd_gw_pool *p, uint32_t from, uint32_t to)
{ return ((uint64_t)to + p->total - from) % p->total; }
static void offset_addr(const fipsd_gw_pool *p, uint32_t offset, struct in6_addr *out) { fipsd_gw_ip6_add(&p->network, offset, out); }
static bool addr_offset(const fipsd_gw_pool *p, const struct in6_addr *addr, uint32_t *offset)
{
    uint32_t o;
    if (!fipsd_gw_ip6_offset(addr, &p->network, &o) || o < 1 || o > p->total) return false;
    *offset = o;
    return true;
}
static bool in_prefix(const fipsd_gw_pool *p, const struct in6_addr *addr) { return fipsd_gw_ip6_in_prefix(addr, &p->network, p->prefix_len); }

/* Free offsets in ring order from the cursor, each with its position, within one lap. */
typedef struct free_iter { const fipsd_gw_pool *p; uint32_t here, next; int phase; } free_iter;
static void free_iter_init(free_iter *it, const fipsd_gw_pool *p) { it->p = p; it->here = offset_at(p, p->cursor); it->next = it->here; it->phase = 0; }
static bool free_iter_next(free_iter *it, uint32_t *offset, uint64_t *position)
{
    const fipsd_gw_pool *p = it->p;
    for (;;) {
        if (it->phase == 0) {
            if (it->next > p->total) { it->phase = 1; it->next = 1; continue; }
        } else if (it->next >= it->here) return false;
        uint32_t o = it->next++;
        if (!bit_test(p->free_bits, o)) continue;
        *offset = o; *position = p->cursor + ring_distance(p, it->here, o);
        return true;
    }
}

fipsd_gw_pool_error fipsd_gw_pool_total(const char *cidr, uint32_t *total)
{
    struct in6_addr addr; uint32_t prefix_len;
    if (!fipsd_gw_cidr_parse(cidr, &addr, &prefix_len)) return FIPSD_GW_POOL_INVALID_CIDR;
    if (prefix_len == 0 || prefix_len >= 128) return FIPSD_GW_POOL_INVALID_PREFIX;
    uint32_t host_bits = 128 - prefix_len;
    /* Cap at 2^16 addresses, and skip offset 0, the network address. */
    uint32_t addrs = host_bits >= MAX_POOL_BITS ? (1u << MAX_POOL_BITS) : (1u << host_bits);
    *total = addrs - 1;
    return FIPSD_GW_POOL_OK;
}
size_t fipsd_gw_small_pool_threshold(uint64_t ttl_secs, uint64_t grace_secs)
{
    uint64_t wait = ttl_secs > UINT64_MAX - grace_secs ? UINT64_MAX : ttl_secs + grace_secs;
    if (wait > SIZE_MAX / FIPSD_GW_MAPPING_RATE) return SIZE_MAX;
    size_t extra = (size_t)wait * FIPSD_GW_MAPPING_RATE;
    return extra > SIZE_MAX - FIPSD_GW_MAPPING_CEILING ? SIZE_MAX : FIPSD_GW_MAPPING_CEILING + extra;
}
fipsd_gw_pool *fipsd_gw_pool_new(const char *cidr, uint64_t ttl_secs, uint64_t grace_secs, fipsd_gw_pool_error *error)
{
    return fipsd_gw_pool_with_limits(cidr, ttl_secs, grace_secs, FIPSD_GW_MAPPING_CEILING, FIPSD_GW_MAPPING_BURST,
                                     FIPSD_GW_MAPPING_RATE, error);
}
fipsd_gw_pool *fipsd_gw_pool_with_limits(const char *cidr, uint64_t ttl_secs, uint64_t grace_secs, size_t ceiling,
                                         uint32_t burst, uint32_t rate, fipsd_gw_pool_error *error)
{
    struct in6_addr addr; uint32_t prefix_len, total;
    fipsd_gw_pool_error e = FIPSD_GW_POOL_OK;
    if (!fipsd_gw_cidr_parse(cidr, &addr, &prefix_len)) e = FIPSD_GW_POOL_INVALID_CIDR;
    else e = fipsd_gw_pool_total(cidr, &total);
    if (e != FIPSD_GW_POOL_OK) { if (error) *error = e; return NULL; }
    fipsd_gw_pool *p = calloc(1, sizeof *p);
    if (!p) { if (error) *error = FIPSD_GW_POOL_INVALID_CIDR; return NULL; }
    snprintf(p->cidr, sizeof p->cidr, "%s", cidr);
    /* The kernel routes the prefix, not the address as written. */
    fipsd_gw_ip6_network(&addr, prefix_len, &p->network);
    p->prefix_len = (uint8_t)prefix_len;
    if (!fipsd_gw_ip6_eq(&p->network, &addr)) {
        char a[48], n[48];
        FIPSD_WARN(MODULE, "Pool CIDR has host bits set; the pool uses its network address: cidr=%s network=%s",
                   cidr, fipsd_gw_ip6_text(&p->network, n, sizeof n));
        (void)fipsd_gw_ip6_text(&addr, a, sizeof a);
    }
    p->total = total; p->ceiling = ceiling; p->ttl_secs = ttl_secs; p->grace_secs = grace_secs;
    size_t bytes = ((size_t)total + 1 + 7) / 8;
    p->free_bits = calloc(bytes, 1); p->held_bits = calloc(bytes, 1);
    if (!p->free_bits || !p->held_bits) { fipsd_gw_pool_free(p); if (error) *error = FIPSD_GW_POOL_INVALID_CIDR; return NULL; }
    for (uint32_t o = 1; o <= total; ++o) free_add(p, o);
    fipsd_gw_map_init(&p->by_node, 16); fipsd_gw_map_init(&p->by_vip, 16); fipsd_gw_map_init(&p->surviving, 20);
    bucket_init(&p->bucket, burst, rate);
    p->start = 1; p->cursor = 0; p->has_mark = false; p->marks = MARK_IGNORED;
    p->clock = fipsd_gw_now();
    p->trusted = true;
    FIPSD_INFO(MODULE, "Virtual IP pool initialized: cidr=%s addresses=%u", cidr, total);
    if (error) *error = FIPSD_GW_POOL_OK;
    return p;
}
fipsd_gw_pool *fipsd_gw_pool_start(const char *cidr, uint64_t ttl_secs, uint64_t grace_secs,
                                   const fipsd_gw_pool_begin *start, fipsd_gw_instant now, fipsd_gw_pool_error *error)
{
    fipsd_gw_pool *p = fipsd_gw_pool_new(cidr, ttl_secs, grace_secs, error);
    if (!p) return NULL;
    p->clock = now;
    p->marks = MARK_PENDING;
    /* Until enough reads have been taken, one that missed an entry could let
     * a reissued address read the old entry's reply as its own traffic. */
    p->reads_needed = FIPSD_GW_MAX_ABSENT_READS;
    uint32_t total = p->total;
    if (!start->restored) {
        p->start = 1 + (start->offset ? start->offset - 1 : 0) % total;
    } else {
        const fipsd_gw_pool_state *s = &start->state;
        bool valid = s->version == 1 && s->total == total && s->from >= 1 && s->from <= total && s->span <= total;
        for (size_t i = 0; valid && i < s->held_len; ++i)
            if (!(1 <= s->held[i][0] && s->held[i][0] <= s->held[i][1] && s->held[i][1] <= total)) valid = false;
        if (valid) {
            p->start = ring_step(s->from, s->span, total);
            for (size_t i = 0; i < s->held_len; ++i)
                for (uint32_t o = s->held[i][0]; o <= s->held[i][1]; ++o) { held_add(p, o); free_remove(p, o); }
            for (uint64_t k = 0; k < s->span; ++k) { uint32_t o = ring_step(s->from, k, total); held_add(p, o); free_remove(p, o); }
            p->has_hold_until = true; p->hold_until = fipsd_gw_later(now, s->hold_secs);
        }
    }
    return p;
}
void fipsd_gw_pool_free(fipsd_gw_pool *p)
{
    if (!p) return;
    free(p->free_bits); free(p->held_bits); free(p->slots); free(p->releasing);
    fipsd_gw_map_clear(&p->by_node); fipsd_gw_map_clear(&p->by_vip); fipsd_gw_map_clear(&p->surviving);
    free(p);
}
void fipsd_gw_pool_network(const fipsd_gw_pool *p, struct in6_addr *network, uint8_t *prefix_len)
{ *network = p->network; *prefix_len = p->prefix_len; }

static fipsd_gw_mapping *slot_of(const fipsd_gw_pool *p, const fipsd_gw_map *map, const void *key)
{
    uint32_t index;
    if (!fipsd_gw_map_get(map, key, &index) || index >= p->slots_len || !p->slots[index].live) return NULL;
    return &p->slots[index];
}
static fipsd_gw_mapping *mapping_of_node(const fipsd_gw_pool *p, const fips_node_addr *node) { return slot_of(p, &p->by_node, node); }
static fipsd_gw_mapping *mapping_of_vip(const fipsd_gw_pool *p, const struct in6_addr *vip) { return slot_of(p, &p->by_vip, vip); }
static fipsd_gw_mapping *slot_new(fipsd_gw_pool *p)
{
    for (size_t i = 0; i < p->slots_len; ++i) if (!p->slots[i].live) return &p->slots[i];
    if (p->slots_len == p->slots_cap) {
        size_t cap = p->slots_cap ? p->slots_cap * 2 : 16;
        fipsd_gw_mapping *grown = realloc(p->slots, cap * sizeof *grown);
        if (!grown) return NULL;
        p->slots = grown; p->slots_cap = cap;
    }
    return &p->slots[p->slots_len++];
}
static void slot_remove(fipsd_gw_pool *p, fipsd_gw_mapping *m)
{
    fipsd_gw_map_remove(&p->by_node, &m->node_addr); fipsd_gw_map_remove(&p->by_vip, &m->virtual_ip);
    m->live = false; p->live--;
}

/* Return held and removed offsets to the free set once nothing can still name them. */
static void release_due(fipsd_gw_pool *p, fipsd_gw_instant now)
{
    bool reads_count = p->trusted;
    if (p->has_hold_until && now >= p->hold_until && (!reads_count || p->reads_seen >= FIPSD_GW_MAX_ABSENT_READS)) {
        for (uint32_t o = 1; o <= p->total; ++o) if (bit_test(p->held_bits, o)) { bit_clear(p->held_bits, o); free_add(p, o); }
        p->held_count = 0; p->has_hold_until = false;
    }
    for (size_t i = 0; i < p->releasing_len;) {
        releasing *r = &p->releasing[i];
        if (now >= r->deadline && r->removed && (!reads_count || r->reads >= FIPSD_GW_MAX_ABSENT_READS)) {
            free_add(p, r->offset);
            p->releasing[i] = p->releasing[--p->releasing_len];
        } else ++i;
    }
}
static void observe_evidence(fipsd_gw_pool *p, bool read)
{
    if (read) {
        p->unread = 0; if (p->read != UINT32_MAX) p->read++;
        if (p->reads_seen != UINT32_MAX) p->reads_seen++;
        for (size_t i = 0; i < p->releasing_len; ++i) if (p->releasing[i].removed && p->releasing[i].reads != UINT32_MAX) p->releasing[i].reads++;
    } else {
        if (p->unread != UINT32_MAX) p->unread++;
        p->read = 0;
    }
    if (p->trusted && p->unread >= FIPSD_GW_MAX_UNREAD_TICKS) {
        p->trusted = false;
        FIPSD_WARN(MODULE, "Conntrack evidence lost; mappings created from now on cannot count as carrying traffic, and removed addresses return once their NAT removal is reported: unread_ticks=%u", p->unread);
    } else if (!p->trusted && p->read >= FIPSD_GW_MAX_ABSENT_READS) {
        p->trusted = true;
        FIPSD_INFO(MODULE, "Conntrack evidence restored; new mappings count traffic again: read_ticks=%u", p->read);
    }
}
static void surviving_key(uint8_t key[20], uint32_t offset, const struct in6_addr *src)
{
    key[0] = (uint8_t)(offset >> 24); key[1] = (uint8_t)(offset >> 16); key[2] = (uint8_t)(offset >> 8); key[3] = (uint8_t)offset;
    memcpy(key + 4, src, 16);
}
static int observe_bindings(fipsd_gw_pool *p, const fipsd_gw_ct_snapshot *conntrack)
{
    fipsd_gw_map seen; fipsd_gw_map_init(&seen, 20);
    size_t cursor = 0; struct in6_addr dst, src; bool replied;
    while (fipsd_gw_ct_snapshot_next_binding(conntrack, &cursor, &dst, &src, &replied)) {
        uint32_t offset;
        if (!addr_offset(p, &dst, &offset)) continue;
        const fipsd_gw_mapping *current = mapping_of_vip(p, &dst);
        if (fipsd_gw_ip6_eq(&src, &dst) || (current && fipsd_gw_ip6_eq(&current->mesh_addr, &src))) continue;
        uint8_t key[20]; surviving_key(key, offset, &src);
        if (fipsd_gw_map_set(&seen, key, 0) != 0) { fipsd_gw_map_clear(&seen); return -1; }
    }
    cursor = 0; const uint8_t *key; uint32_t *misses;
    while (fipsd_gw_map_next(&p->surviving, &cursor, &key, &misses)) {
        if (*misses != UINT32_MAX) ++*misses;
        if (!fipsd_gw_map_contains(&seen, key) && *misses >= FIPSD_GW_MAX_ABSENT_READS) fipsd_gw_map_remove_at(&p->surviving, cursor);
    }
    cursor = 0;
    while (fipsd_gw_map_next(&seen, &cursor, &key, &misses))
        if (fipsd_gw_map_set(&p->surviving, key, 0) != 0) { fipsd_gw_map_clear(&seen); return -1; }
    fipsd_gw_map_clear(&seen);
    return 0;
}
void fipsd_gw_pool_set_evidence(fipsd_gw_pool *p, bool available) { p->trusted = available; p->read = 0; }

/* Hold a removed mapping's address until no answer can still name it. */
static int start_releasing(fipsd_gw_pool *p, const fipsd_gw_mapping *m)
{
    uint32_t offset;
    if (!addr_offset(p, &m->virtual_ip, &offset)) return 0;
    fipsd_gw_instant deadline = fipsd_gw_later(m->last_answer, p->grace_secs);
    if (m->answer_deadline > deadline) deadline = m->answer_deadline;
    releasing r = {offset, m->mesh_addr, deadline, false, 0};
    for (size_t i = 0; i < p->releasing_len; ++i) if (p->releasing[i].offset == offset) { p->releasing[i] = r; return 0; }
    if (p->releasing_len == p->releasing_cap) {
        size_t cap = p->releasing_cap ? p->releasing_cap * 2 : 16;
        releasing *grown = realloc(p->releasing, cap * sizeof *grown);
        if (!grown) return -1;
        p->releasing = grown; p->releasing_cap = cap;
    }
    p->releasing[p->releasing_len++] = r;
    return 0;
}
static bool evict(fipsd_gw_pool *p, fipsd_gw_mapping *m, fipsd_gw_evicted *out)
{
    fipsd_gw_mapping copy = *m;
    slot_remove(p, m);
    p->evicted_total++;
    (void)start_releasing(p, &copy);
    char v[48], a[48];
    FIPSD_DEBUG(MODULE, "Evicted never-used mapping: virtual_ip=%s mesh_addr=%s",
                fipsd_gw_ip6_text(&copy.virtual_ip, v, sizeof v), fipsd_gw_ip6_text(&copy.mesh_addr, a, sizeof a));
    out->present = true; out->virtual_ip = copy.virtual_ip; out->mesh_addr = copy.mesh_addr;
    return true;
}
static uint32_t answer_ttl(const fipsd_gw_pool *p, const fipsd_gw_mapping *m, fipsd_gw_instant now)
{
    uint64_t ttl;
    if (m->used) ttl = p->ttl_secs;
    else {
        ttl = fipsd_gw_since(fipsd_gw_later(m->created, p->ttl_secs), now) / FIPSD_GW_SEC;
        if (ttl > p->ttl_secs) ttl = p->ttl_secs;
    }
    return ttl > UINT32_MAX ? UINT32_MAX : (uint32_t)ttl;
}
static int held_ranges(const fipsd_gw_pool *p, fipsd_gw_pool_state *state)
{
    size_t bytes = ((size_t)p->total + 1 + 7) / 8;
    uint8_t *held = calloc(bytes, 1);
    if (!held) return -1;
    size_t cursor = 0; const uint8_t *key; uint32_t *index;
    while (fipsd_gw_map_next(&p->by_vip, &cursor, &key, &index)) {
        struct in6_addr vip; memcpy(&vip, key, 16); uint32_t o;
        if (addr_offset(p, &vip, &o)) bit_set(held, o);
    }
    for (uint32_t o = 1; o <= p->total; ++o) if (bit_test(p->held_bits, o)) bit_set(held, o);
    for (size_t i = 0; i < p->releasing_len; ++i) bit_set(held, p->releasing[i].offset);
    size_t cap = 0;
    for (uint32_t o = 1; o <= p->total; ++o) {
        if (!bit_test(held, o)) continue;
        if (state->held_len && state->held[state->held_len - 1][1] + 1 == o) { state->held[state->held_len - 1][1] = o; continue; }
        if (state->held_len == cap) {
            cap = cap ? cap * 2 : 8;
            uint32_t (*grown)[2] = realloc(state->held, cap * sizeof *grown);
            if (!grown) { free(held); return -1; }
            state->held = grown;
        }
        state->held[state->held_len][0] = o; state->held[state->held_len][1] = o; state->held_len++;
    }
    free(held);
    return 0;
}
static uint64_t hold_secs(const fipsd_gw_pool *p)
{
    uint64_t restart = 0;
    if (p->has_hold_until) { restart = fipsd_gw_since(p->hold_until, p->clock) / FIPSD_GW_SEC; if (restart != UINT64_MAX) restart++; }
    uint64_t base = p->ttl_secs > UINT64_MAX - p->grace_secs ? UINT64_MAX : p->ttl_secs + p->grace_secs;
    return base > restart ? base : restart;
}
static int state_of(const fipsd_gw_pool *p, uint32_t span, fipsd_gw_pool_state *out)
{
    memset(out, 0, sizeof *out);
    out->version = 1; snprintf(out->pool, sizeof out->pool, "%s", p->cidr);
    out->total = p->total; out->from = offset_at(p, p->cursor); out->span = span; out->hold_secs = hold_secs(p);
    return held_ranges(p, out);
}
bool fipsd_gw_pool_mark_request(const fipsd_gw_pool *p, fipsd_gw_pool_state *out)
{
    free_iter it; uint32_t offset; uint64_t position;
    if (p->has_mark) {
        size_t ahead = 0;
        free_iter_init(&it, p);
        while (ahead < FIPSD_GW_MARK_LOW_WATER && free_iter_next(&it, &offset, &position) && position < p->durable_mark) ahead++;
        if (ahead >= FIPSD_GW_MARK_LOW_WATER) return false;
    }
    uint64_t mark = p->cursor; size_t taken = 0;
    free_iter_init(&it, p);
    while (taken < FIPSD_GW_MARK_RESERVE && free_iter_next(&it, &offset, &position)) { mark = position + 1; taken++; }
    uint64_t span = mark - p->cursor;
    if (span > p->total) span = p->total;
    return state_of(p, (uint32_t)span, out) == 0;
}
int fipsd_gw_pool_shutdown_state(const fipsd_gw_pool *p, fipsd_gw_pool_state *out) { return state_of(p, 0, out); }
bool fipsd_gw_pool_carry_state(const fipsd_gw_pool *p, fipsd_gw_pool_state *out)
{
    if (!p->has_hold_until) return false;
    if (state_of(p, 0, out) != 0) return false;
    uint64_t remaining = fipsd_gw_since(p->hold_until, p->clock) / FIPSD_GW_SEC;
    out->hold_secs = remaining == UINT64_MAX ? remaining : remaining + 1;
    return true;
}
void fipsd_gw_pool_confirm_mark(fipsd_gw_pool *p, const fipsd_gw_pool_state *state)
{
    uint32_t here = offset_at(p, p->cursor);
    uint64_t back = ring_distance(p, state->from, here);
    uint64_t from = p->cursor > back ? p->cursor - back : 0;
    uint64_t mark = from + state->span;
    if (!p->has_mark || mark > p->durable_mark) p->durable_mark = mark;
    p->has_mark = true; p->marks = MARK_ENFORCED;
}
void fipsd_gw_pool_mark_failed(fipsd_gw_pool *p) { p->marks = MARK_IGNORED; }

static fipsd_gw_pool_error refuse(fipsd_gw_pool *p, fipsd_gw_pool_error error, fipsd_gw_instant now)
{
    p->refused_total++;
    if (p->refusal_open) {
        if (now > p->refusal_last) p->refusal_last = now;
        p->refusal_count++;
    } else {
        p->refusal_open = true; p->refusal_last = now; p->refusal_count = 1;
        uint64_t bound = 0;
        if (error == FIPSD_GW_POOL_AT_CEILING) bound = p->ceiling;
        else if (error == FIPSD_GW_POOL_EXHAUSTED) bound = p->total;
        else if (error == FIPSD_GW_POOL_AWAITING_MARK) bound = p->has_mark ? offset_at(p, p->durable_mark) : 0;
        char text[128];
        FIPSD_WARN(MODULE, "Pool refusing new names: reason=%s bound=%llu error=%s", fipsd_gw_pool_error_reason(error),
                   (unsigned long long)bound, fipsd_gw_pool_error_text(error, p->live, NULL, NULL, text, sizeof text));
    }
    return error;
}
void fipsd_gw_pool_nat_removed(fipsd_gw_pool *p, const struct in6_addr *virtual_ip, const struct in6_addr *mesh_addr)
{
    uint32_t offset;
    if (!addr_offset(p, virtual_ip, &offset)) return;
    for (size_t i = 0; i < p->releasing_len; ++i) {
        releasing *r = &p->releasing[i];
        if (r->offset == offset && fipsd_gw_ip6_eq(&r->mesh_addr, mesh_addr) && !r->removed) { r->removed = true; r->reads = 0; }
    }
}
bool fipsd_gw_pool_refresh_at(fipsd_gw_pool *p, const fips_node_addr *node_addr, fipsd_gw_instant now)
{
    fipsd_gw_mapping *m = mapping_of_node(p, node_addr);
    if (!m || !m->used) return false;
    m->last_referenced = now;
    if (m->state == FIPSD_GW_MAPPING_DRAINING) { m->state = FIPSD_GW_MAPPING_ALLOCATED; m->has_drain_start = false; }
    return true;
}
fipsd_gw_pool_error fipsd_gw_pool_allocate_at(fipsd_gw_pool *p, const fips_node_addr *node_addr,
                                              const struct in6_addr *mesh_addr, const char *dns_name,
                                              fipsd_gw_instant now, fipsd_gw_allocation *out)
{
    memset(out, 0, sizeof *out);
    /* The gateway routes the whole pool prefix to itself, so traffic to a mesh
     * address inside it never reaches the mesh. */
    if (in_prefix(p, mesh_addr)) return FIPSD_GW_POOL_INSIDE_POOL;
    if (now > p->clock) p->clock = now;
    release_due(p, now);
    /* Idempotent: return the existing mapping, refreshed if it has carried traffic. */
    (void)fipsd_gw_pool_refresh_at(p, node_addr, now);
    fipsd_gw_mapping *existing = mapping_of_node(p, node_addr);
    if (existing) {
        existing->last_answer = now;
        out->virtual_ip = existing->virtual_ip; out->is_new = false; out->ttl = answer_ttl(p, existing, now);
        return FIPSD_GW_POOL_OK;
    }
    /* Ceiling first, so a refusal there costs no token. */
    fipsd_gw_mapping *victim = NULL;
    if (p->live >= p->ceiling) {
        for (size_t i = 0; i < p->slots_len; ++i) {
            fipsd_gw_mapping *m = &p->slots[i];
            if (!m->live || m->used) continue;
            if (!victim || m->created < victim->created ||
                (m->created == victim->created && fipsd_gw_ip6_cmp(&m->virtual_ip, &victim->virtual_ip) < 0)) victim = m;
        }
        if (!victim) return refuse(p, FIPSD_GW_POOL_AT_CEILING, now);
    }
    bucket_refill(&p->bucket, now);
    if (!bucket_has_token(&p->bucket)) return FIPSD_GW_POOL_RATE_LIMITED;
    if (p->marks == MARK_PENDING) return refuse(p, FIPSD_GW_POOL_AWAITING_MARK, now);
    /* Skip an address that a surviving conntrack entry still binds to this node. */
    free_iter it; free_iter_init(&it, p);
    uint32_t offset = 0; uint64_t position = 0; bool found = false;
    while (free_iter_next(&it, &offset, &position)) {
        uint8_t key[20]; surviving_key(key, offset, mesh_addr);
        if (!fipsd_gw_map_contains(&p->surviving, key)) { found = true; break; }
    }
    if (!found) return refuse(p, FIPSD_GW_POOL_EXHAUSTED, now);
    if (p->marks == MARK_ENFORCED && p->has_mark && position >= p->durable_mark) return refuse(p, FIPSD_GW_POOL_AWAITING_MARK, now);
    if (victim) evict(p, victim, &out->evicted);
    bucket_take(&p->bucket);
    free_remove(p, offset);
    p->cursor = position + 1;
    fipsd_gw_mapping *m = slot_new(p);
    if (!m) return FIPSD_GW_POOL_EXHAUSTED;
    memset(m, 0, sizeof *m);
    m->live = true; m->node_addr = *node_addr; offset_addr(p, offset, &m->virtual_ip); m->mesh_addr = *mesh_addr;
    snprintf(m->dns_name, sizeof m->dns_name, "%s", dns_name ? dns_name : "");
    m->state = FIPSD_GW_MAPPING_ALLOCATED; m->created = now; m->last_referenced = now; m->session_count = 0;
    m->used = false; m->blind = !p->trusted || p->reads_seen < p->reads_needed;
    uint64_t life = p->ttl_secs > UINT64_MAX - p->grace_secs ? UINT64_MAX : p->ttl_secs + p->grace_secs;
    m->answer_deadline = fipsd_gw_later(now, life); m->last_answer = now;
    uint32_t index = (uint32_t)(m - p->slots);
    if (fipsd_gw_map_set(&p->by_node, node_addr, index) != 0 || fipsd_gw_map_set(&p->by_vip, &m->virtual_ip, index) != 0) {
        fipsd_gw_map_remove(&p->by_node, node_addr); m->live = false; free_add(p, offset);
        return FIPSD_GW_POOL_EXHAUSTED;
    }
    p->live++;
    char v[48], a[48];
    FIPSD_INFO(MODULE, "Allocated virtual IP: virtual_ip=%s mesh_addr=%s dns_name=%s",
               fipsd_gw_ip6_text(&m->virtual_ip, v, sizeof v), fipsd_gw_ip6_text(mesh_addr, a, sizeof a), m->dns_name);
    out->virtual_ip = m->virtual_ip; out->is_new = true;
    out->ttl = p->ttl_secs > UINT32_MAX ? UINT32_MAX : (uint32_t)p->ttl_secs;
    return FIPSD_GW_POOL_OK;
}
int fipsd_gw_pool_tick(fipsd_gw_pool *p, fipsd_gw_instant now, const fipsd_gw_ct_snapshot *conntrack, fipsd_gw_events *events)
{
    if (now > p->clock) p->clock = now;
    observe_evidence(p, conntrack->read);
    if (conntrack->read && observe_bindings(p, conntrack) != 0) return -1;
    /* Before this tick's own frees, so an address freed now cannot leave in the same tick. */
    release_due(p, now);
    if (p->refusal_open && fipsd_gw_since(now, p->refusal_last) >= (uint64_t)FIPSD_GW_BOUND_LOG_HOLD_SECS * FIPSD_GW_SEC) {
        FIPSD_INFO(MODULE, "Pool accepting new names again: refused=%llu", (unsigned long long)p->refusal_count);
        p->refusal_open = false;
    }
    uint64_t ttl = p->ttl_secs > UINT64_MAX / FIPSD_GW_SEC ? UINT64_MAX : p->ttl_secs * FIPSD_GW_SEC;
    uint64_t grace = p->grace_secs > UINT64_MAX / FIPSD_GW_SEC ? UINT64_MAX : p->grace_secs * FIPSD_GW_SEC;
    bool trusted = p->trusted;
    char v[48], a[48];
    for (size_t i = 0; i < p->slots_len; ++i) {
        fipsd_gw_mapping *m = &p->slots[i];
        if (!m->live) continue;
        uint32_t sessions = fipsd_gw_ct_snapshot_sessions_for(conntrack, &m->virtual_ip);
        m->session_count = sessions;
        /* Only a reply from the mapped node counts as use. */
        if (trusted && !m->used && !m->blind && fipsd_gw_ct_snapshot_replied_from(conntrack, &m->virtual_ip, &m->mesh_addr)) {
            m->used = true;
            FIPSD_DEBUG(MODULE, "Mapping carried traffic: virtual_ip=%s", fipsd_gw_ip6_text(&m->virtual_ip, v, sizeof v));
        }
        /* Live data-plane traffic pins the mapping. */
        if (sessions > 0) m->last_referenced = now;
        switch (m->state) {
        case FIPSD_GW_MAPPING_ALLOCATED:
            if (sessions > 0) {
                m->state = FIPSD_GW_MAPPING_ACTIVE;
                FIPSD_DEBUG(MODULE, "Mapping activated: virtual_ip=%s sessions=%u", fipsd_gw_ip6_text(&m->virtual_ip, v, sizeof v), sessions);
            } else if (fipsd_gw_since(now, m->last_referenced) > ttl) {
                m->state = FIPSD_GW_MAPPING_DRAINING; m->has_drain_start = true; m->drain_start = now;
                FIPSD_DEBUG(MODULE, "Allocated mapping TTL expired, draining: virtual_ip=%s", fipsd_gw_ip6_text(&m->virtual_ip, v, sizeof v));
            }
            break;
        case FIPSD_GW_MAPPING_ACTIVE:
            if (fipsd_gw_since(now, m->last_referenced) > ttl) { m->state = FIPSD_GW_MAPPING_DRAINING; m->has_drain_start = true; m->drain_start = now; }
            break;
        case FIPSD_GW_MAPPING_DRAINING:
            if (sessions > 0) {
                m->state = FIPSD_GW_MAPPING_ACTIVE; m->has_drain_start = false;
                FIPSD_DEBUG(MODULE, "Draining mapping recovered to active (traffic resumed): virtual_ip=%s sessions=%u",
                            fipsd_gw_ip6_text(&m->virtual_ip, v, sizeof v), sessions);
            } else if (m->has_drain_start && fipsd_gw_since(now, m->drain_start) > grace) {
                /* Free: the address waits in releasing until nothing can name it. */
                fipsd_gw_mapping copy = *m;
                slot_remove(p, m);
                if (start_releasing(p, &copy) != 0) return -1;
                FIPSD_INFO(MODULE, "Reclaimed virtual IP: virtual_ip=%s mesh_addr=%s",
                           fipsd_gw_ip6_text(&copy.virtual_ip, v, sizeof v), fipsd_gw_ip6_text(&copy.mesh_addr, a, sizeof a));
                fipsd_gw_pool_event event = {FIPSD_GW_EVENT_MAPPING_REMOVED, copy.virtual_ip, copy.mesh_addr};
                if (fipsd_gw_events_push(events, &event) != 0) return -1;
            }
            break;
        }
    }
    return 0;
}
void fipsd_gw_pool_get_status(const fipsd_gw_pool *p, fipsd_gw_pool_status *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < p->slots_len; ++i) {
        const fipsd_gw_mapping *m = &p->slots[i];
        if (!m->live) continue;
        if (m->state == FIPSD_GW_MAPPING_ALLOCATED) out->allocated++;
        else if (m->state == FIPSD_GW_MAPPING_ACTIVE) out->active++;
        else out->draining++;
    }
    out->total = p->total; out->free = p->free_count; out->evicted = p->evicted_total;
    out->releasing = p->releasing_len + p->held_count; out->refused = p->refused_total;
}
int fipsd_gw_pool_mapping_info(const fipsd_gw_pool *p, fipsd_gw_instant now, fipsd_gw_mapping_info **out, size_t *count)
{
    fipsd_gw_mapping_info *info = p->live ? malloc(p->live * sizeof *info) : NULL;
    if (p->live && !info) return -1;
    size_t n = 0;
    for (size_t i = 0; i < p->slots_len && n < p->live; ++i) {
        const fipsd_gw_mapping *m = &p->slots[i];
        if (!m->live) continue;
        fipsd_gw_mapping_info *row = &info[n++];
        row->virtual_ip = m->virtual_ip; row->mesh_addr = m->mesh_addr; row->node_addr = m->node_addr;
        memcpy(row->dns_name, m->dns_name, sizeof row->dns_name);
        row->state = m->state; row->session_count = m->session_count;
        row->age_secs = fipsd_gw_since(now, m->created) / FIPSD_GW_SEC;
        row->last_ref_secs = fipsd_gw_since(now, m->last_referenced) / FIPSD_GW_SEC;
        row->used = m->used;
    }
    *out = info; *count = n;
    return 0;
}
const fipsd_gw_mapping *fipsd_gw_pool_lookup_virtual_ip(const fipsd_gw_pool *p, const struct in6_addr *virtual_ip) { return mapping_of_vip(p, virtual_ip); }
const fipsd_gw_mapping *fipsd_gw_pool_lookup_node(const fipsd_gw_pool *p, const fips_node_addr *node_addr) { return mapping_of_node(p, node_addr); }

uint64_t fipsd_gw_pool_test_tokens(const fipsd_gw_pool *p) { return p->bucket.level / FIPSD_GW_SEC; }
size_t fipsd_gw_pool_test_free_len(const fipsd_gw_pool *p) { return p->free_count; }
bool fipsd_gw_pool_test_free_contains(const fipsd_gw_pool *p, uint32_t offset) { return offset <= p->total && bit_test(p->free_bits, offset); }
uint32_t fipsd_gw_pool_test_total(const fipsd_gw_pool *p) { return p->total; }
uint64_t fipsd_gw_pool_test_cursor(const fipsd_gw_pool *p) { return p->cursor; }
uint32_t fipsd_gw_pool_test_start(const fipsd_gw_pool *p) { return p->start; }
uint32_t fipsd_gw_pool_test_offset_at(const fipsd_gw_pool *p, uint64_t position) { return offset_at(p, position); }
uint64_t fipsd_gw_pool_test_ring_distance(const fipsd_gw_pool *p, uint32_t from, uint32_t to) { return ring_distance(p, from, to); }
bool fipsd_gw_pool_test_addr_offset(const fipsd_gw_pool *p, const struct in6_addr *addr, uint32_t *offset) { return addr_offset(p, addr, offset); }
void fipsd_gw_pool_test_offset_addr(const fipsd_gw_pool *p, uint32_t offset, struct in6_addr *out) { offset_addr(p, offset, out); }
uint64_t fipsd_gw_pool_test_refused_total(const fipsd_gw_pool *p) { return p->refused_total; }
bool fipsd_gw_pool_test_refusal_open(const fipsd_gw_pool *p, uint64_t *count) { if (count) *count = p->refusal_count; return p->refusal_open; }
uint64_t fipsd_gw_pool_test_evicted_total(const fipsd_gw_pool *p) { return p->evicted_total; }
bool fipsd_gw_pool_test_evidence_trusted(const fipsd_gw_pool *p) { return p->trusted; }
size_t fipsd_gw_pool_len(const fipsd_gw_pool *p) { return p->live; }
size_t fipsd_gw_pool_test_mappings_len(const fipsd_gw_pool *p) { return p->live; }
