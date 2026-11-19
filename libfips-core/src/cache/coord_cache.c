/* Port of upstream core/src/cache/coord_cache.rs. The Rust HashMap becomes an
 * open-addressing table over caller-owned slots (SipHash under a key drawn
 * from host entropy, backward-shift deletion); each BTreeMap<(stamp, seq)>
 * index becomes a binary heap of slot indexes with per-slot positions, so
 * pop_first/remove/insert stay O(log n) without allocation. Keys are unique
 * through seq, so heap order equals the Rust map order. A fourth heap (no
 * upstream counterpart) holds every resident slot by expires_at, so purging
 * expired entries costs what it removes, not a sweep of the slots. */
#include <fips/cache/coord_cache.h>
#include <sodium/crypto_shorthash_siphash24.h>
#include <string.h>

#define NONE UINT32_MAX

typedef struct heap { uint32_t *items; size_t *len; } heap;

static heap heap_of(fips_coord_cache *c, fips_cache_place place)
{
    switch (place) {
    case FIPS_PLACE_PARKED: return (heap){c->parked, &c->parked_len};
    case FIPS_PLACE_PROBATION: return (heap){c->probation, &c->probation_len};
    case FIPS_PLACE_BY_USE: break;
    }
    return (heap){c->by_use, &c->by_use_len};
}

static bool key_less(const fips_coord_cache *c, uint32_t a, uint32_t b)
{
    const fips_coord_slot *x = &c->slots[a], *y = &c->slots[b];
    return x->key_stamp < y->key_stamp || (x->key_stamp == y->key_stamp && x->key_seq < y->key_seq);
}

static void heap_set(fips_coord_cache *c, heap h, size_t pos, uint32_t idx)
{
    h.items[pos] = idx;
    c->slots[idx].heap_pos = (uint32_t)pos;
}

static void sift_up(fips_coord_cache *c, heap h, size_t pos)
{
    uint32_t idx = h.items[pos];
    while (pos > 0) {
        size_t parent = (pos - 1) / 2;
        if (!key_less(c, idx, h.items[parent])) break;
        heap_set(c, h, pos, h.items[parent]);
        pos = parent;
    }
    heap_set(c, h, pos, idx);
}

static void sift_down(fips_coord_cache *c, heap h, size_t pos)
{
    size_t len = *h.len;
    uint32_t idx = h.items[pos];
    for (;;) {
        size_t child = 2 * pos + 1;
        if (child >= len) break;
        if (child + 1 < len && key_less(c, h.items[child + 1], h.items[child])) ++child;
        if (!key_less(c, h.items[child], idx)) break;
        heap_set(c, h, pos, h.items[child]);
        pos = child;
    }
    heap_set(c, h, pos, idx);
}

static void heap_push(fips_coord_cache *c, heap h, uint32_t idx)
{
    size_t pos = (*h.len)++;
    heap_set(c, h, pos, idx);
    sift_up(c, h, pos);
}

/* Remove the item at pos (the last item takes its place). */
static void heap_delete(fips_coord_cache *c, heap h, size_t pos)
{
    size_t last = --*h.len;
    if (pos == last) return;
    uint32_t moved = h.items[last];
    heap_set(c, h, pos, moved);
    sift_down(c, h, pos);
    sift_up(c, h, c->slots[moved].heap_pos);
}

static uint32_t heap_pop(fips_coord_cache *c, heap h)
{
    uint32_t idx = h.items[0];
    heap_delete(c, h, 0);
    return idx;
}

/* The expiry index: every filed slot, a min-heap by expires_at (slot index
 * breaks ties). Kept in step wherever a filed entry's expires_at changes. */
static bool expiry_less(const fips_coord_cache *c, uint32_t a, uint32_t b)
{
    uint64_t x = c->slots[a].entry.expires_at, y = c->slots[b].entry.expires_at;
    return x < y || (x == y && a < b);
}

static void expiry_set(fips_coord_cache *c, size_t pos, uint32_t idx)
{
    c->expiry[pos] = idx;
    c->slots[idx].expiry_pos = (uint32_t)pos;
}

static void expiry_up(fips_coord_cache *c, size_t pos)
{
    uint32_t idx = c->expiry[pos];
    while (pos > 0) {
        size_t parent = (pos - 1) / 2;
        if (!expiry_less(c, idx, c->expiry[parent])) break;
        expiry_set(c, pos, c->expiry[parent]);
        pos = parent;
    }
    expiry_set(c, pos, idx);
}

static void expiry_down(fips_coord_cache *c, size_t pos)
{
    uint32_t idx = c->expiry[pos];
    for (;;) {
        size_t child = 2 * pos + 1;
        if (child >= c->expiry_len) break;
        if (child + 1 < c->expiry_len && expiry_less(c, c->expiry[child + 1], c->expiry[child])) ++child;
        if (!expiry_less(c, c->expiry[child], idx)) break;
        expiry_set(c, pos, c->expiry[child]);
        pos = child;
    }
    expiry_set(c, pos, idx);
}

/* After the entry of a filed slot changed its expires_at. */
static void expiry_fix(fips_coord_cache *c, uint32_t idx)
{
    expiry_up(c, c->slots[idx].expiry_pos);
    expiry_down(c, c->slots[idx].expiry_pos);
}

static void expiry_delete(fips_coord_cache *c, size_t pos)
{
    size_t last = --c->expiry_len;
    if (pos == last) return;
    uint32_t moved = c->expiry[last];
    expiry_set(c, pos, moved);
    expiry_fix(c, moved);
}

static void set_key(fips_coord_cache *c, uint32_t idx, uint64_t stamp)
{
    c->slots[idx].key_stamp = stamp;
    c->slots[idx].key_seq = ++c->seq;
}

static uint64_t hash_addr(const fips_coord_cache *c, const uint8_t addr[16])
{
    uint8_t digest[crypto_shorthash_siphash24_BYTES];
    /* SipHash has no fallible inputs for these fixed key/output sizes. */
    crypto_shorthash_siphash24(digest, addr, 16, c->hash_key);
    uint64_t hash = 0;
    for (size_t i = 0; i < sizeof(digest); ++i) hash |= (uint64_t)digest[i] << (i * 8);
    return hash;
}

static size_t home(const fips_coord_cache *c, uint64_t hash)
{
    return (size_t)(((uint64_t)(uint32_t)(hash >> 32) * (uint64_t)c->table_len) >> 32);
}

static size_t next_pos(const fips_coord_cache *c, size_t pos)
{
    return pos + 1 == c->table_len ? 0 : pos + 1;
}

static uint32_t find(const fips_coord_cache *c, const uint8_t addr[16], uint64_t *hash_out)
{
    uint64_t hash = hash_addr(c, addr);
    if (hash_out) *hash_out = hash;
    for (size_t pos = home(c, hash);; pos = next_pos(c, pos)) {
        uint32_t idx = c->table[pos];
        if (idx == NONE) return NONE;
        const fips_coord_slot *slot = &c->slots[idx];
        if (slot->hash == hash && memcmp(slot->addr, addr, 16) == 0) return idx;
    }
}

static void table_insert(fips_coord_cache *c, uint32_t idx)
{
    size_t pos = home(c, c->slots[idx].hash);
    while (c->table[pos] != NONE) pos = next_pos(c, pos);
    c->table[pos] = idx;
}

static void table_remove(fips_coord_cache *c, uint32_t idx)
{
    size_t i = home(c, c->slots[idx].hash);
    while (c->table[i] != idx) i = next_pos(c, i);
    c->table[i] = NONE;
    for (size_t j = next_pos(c, i); c->table[j] != NONE; j = next_pos(c, j)) {
        size_t k = home(c, c->slots[c->table[j]].hash);
        /* An item whose home lies cyclically in (i, j] is still reachable. */
        bool reachable = i <= j ? (i < k && k <= j) : (i < k || k <= j);
        if (reachable) continue;
        c->table[i] = c->table[j];
        c->table[j] = NONE;
        i = j;
    }
}

static uint32_t alloc_slot(fips_coord_cache *c)
{
    uint32_t idx = c->free_head;
    if (idx == NONE) return NONE;
    c->free_head = c->slots[idx].link;
    c->slots[idx].used = true;
    return idx;
}

static void free_slot(fips_coord_cache *c, uint32_t idx)
{
    fips_coord_slot *slot = &c->slots[idx];
    slot->used = false;
    slot->link = c->free_head;
    c->free_head = idx;
}

static fips_segment segment_of_place(fips_cache_place place)
{
    return place == FIPS_PLACE_PROBATION ? FIPS_SEGMENT_PROBATION : FIPS_SEGMENT_MAIN;
}

static uint64_t weight_of(const fips_cached_coord *coords)
{
    return coords->len > FIPS_ADDRS_PER_ENTRY ? coords->len : FIPS_ADDRS_PER_ENTRY;
}

static bool covers(fips_funding funding, uint64_t weight)
{
    return !funding.charged || weight <= funding.upto;
}

/* Rust file_new: the slot already holds entry, addr, hash and weight. */
static void file_new(fips_coord_cache *c, uint32_t idx, fips_segment segment)
{
    fips_coord_slot *slot = &c->slots[idx];
    set_key(c, idx, slot->entry.last_used);
    if (segment == FIPS_SEGMENT_MAIN) {
        c->stored_weight += slot->weight;
        slot->place = FIPS_PLACE_BY_USE;
    } else {
        slot->place = FIPS_PLACE_PROBATION;
    }
    heap_push(c, heap_of(c, slot->place), idx);
    expiry_set(c, c->expiry_len++, idx);
    expiry_up(c, slot->expiry_pos);
    table_insert(c, idx);
    ++c->len;
    ++c->generation;
}

/* Take a slot out of the map and its index without freeing it, so the caller
 * can file it again; in_heap is false for a victim already popped. */
static void unfile(fips_coord_cache *c, uint32_t idx, bool in_heap)
{
    fips_coord_slot *slot = &c->slots[idx];
    if (in_heap) heap_delete(c, heap_of(c, slot->place), slot->heap_pos);
    if (slot->expiry_pos != NONE) expiry_delete(c, slot->expiry_pos);
    slot->expiry_pos = NONE;
    if (segment_of_place(slot->place) == FIPS_SEGMENT_MAIN) c->stored_weight -= slot->weight;
    table_remove(c, idx);
    --c->len;
    ++c->generation;
}

static void remove_slot(fips_coord_cache *c, uint32_t idx)
{
    unfile(c, idx, true);
    free_slot(c, idx);
}

static size_t probation_bound(const fips_coord_cache *c)
{
    return FIPS_COORD_CACHE_PROBATION_BOUND(c->max_entries);
}

static uint64_t weight_bound(const fips_coord_cache *c)
{
    uint64_t m = (uint64_t)c->max_entries;
    return m > UINT64_MAX / FIPS_ADDRS_PER_ENTRY ? UINT64_MAX : m * FIPS_ADDRS_PER_ENTRY;
}

static void make_probation_room(fips_coord_cache *c)
{
    heap h = heap_of(c, FIPS_PLACE_PROBATION);
    while (c->probation_len >= probation_bound(c)) {
        if (c->probation_len == 0) return;
        uint32_t idx = heap_pop(c, h);
        ++c->eviction_examined;
        fips_coord_slot *slot = &c->slots[idx];
        if (slot->entry.last_used != slot->key_stamp) {
            /* Used by a frame since it was filed: restamp, as main does. */
            set_key(c, idx, slot->entry.last_used);
            heap_push(c, h, idx);
        } else {
            unfile(c, idx, false);
            free_slot(c, idx);
        }
    }
}

static uint32_t next_victim(fips_coord_cache *c, uint64_t now)
{
    heap parked = heap_of(c, FIPS_PLACE_PARKED), by_use = heap_of(c, FIPS_PLACE_BY_USE);
    /* Entries whose verification has lapsed are victims again. */
    while (c->parked_len > 0 && c->slots[parked.items[0]].key_stamp < now) {
        uint32_t idx = heap_pop(c, parked);
        ++c->eviction_examined;
        set_key(c, idx, c->slots[idx].entry.last_used);
        c->slots[idx].place = FIPS_PLACE_BY_USE;
        heap_push(c, by_use, idx);
    }
    while (c->by_use_len > 0) {
        uint32_t idx = heap_pop(c, by_use);
        ++c->eviction_examined;
        fips_coord_slot *slot = &c->slots[idx];
        if (slot->entry.last_used != slot->key_stamp) {
            set_key(c, idx, slot->entry.last_used);
            heap_push(c, by_use, idx);
            continue;
        }
        if (fips_cache_entry_is_verified(&slot->entry, now)) {
            set_key(c, idx, slot->entry.verified_until);
            slot->place = FIPS_PLACE_PARKED;
            heap_push(c, parked, idx);
            continue;
        }
        return idx;
    }
    return NONE;
}

/* Evict until one more entry of weight fits both bounds. A refusal puts the
 * victims found back, so a refused write leaves every entry where it was.
 * Victims wait on a chain through slot.link (unused while a slot is filed). */
static bool make_room(fips_coord_cache *c, uint64_t weight, uint64_t now)
{
    uint64_t bound = weight_bound(c);
    if (weight > bound) weight = bound;
    size_t len = fips_coord_cache_main_len(c);
    uint64_t stored = c->stored_weight;
    uint32_t chain = NONE;
    while (len >= c->max_entries || stored > bound - weight) {
        uint32_t victim = next_victim(c, now);
        if (victim == NONE) {
            heap by_use = heap_of(c, FIPS_PLACE_BY_USE);
            for (uint32_t idx = chain; idx != NONE; idx = c->slots[idx].link)
                heap_push(c, by_use, idx);
            return false;
        }
        --len;
        stored -= c->slots[victim].weight;
        c->slots[victim].link = chain;
        chain = victim;
    }
    while (chain != NONE) {
        uint32_t idx = chain;
        chain = c->slots[idx].link;
        unfile(c, idx, false);
        free_slot(c, idx);
    }
    return true;
}

/* Re-file a slot in by_use (or probation) at its entry's current last use. */
static void refile(fips_coord_cache *c, uint32_t idx)
{
    fips_coord_slot *slot = &c->slots[idx];
    ++c->generation;
    heap_delete(c, heap_of(c, slot->place), slot->heap_pos);
    set_key(c, idx, slot->entry.last_used);
    if (slot->place != FIPS_PLACE_PROBATION) slot->place = FIPS_PLACE_BY_USE;
    heap_push(c, heap_of(c, slot->place), idx);
}

static void update_in_place(fips_coord_cache *c, uint32_t idx, const fips_cached_coord *coords,
                            uint64_t now, uint64_t ttl_ms, uint64_t weight)
{
    fips_coord_slot *slot = &c->slots[idx];
    fips_cache_entry_update(&slot->entry, coords, now, ttl_ms);
    expiry_fix(c, idx);
    uint64_t charged = slot->weight > weight ? slot->weight : weight;
    if (segment_of_place(slot->place) == FIPS_SEGMENT_MAIN)
        c->stored_weight = c->stored_weight - slot->weight + charged;
    slot->weight = charged;
    refile(c, idx);
}

static fips_placed placed(fips_hint_outcome outcome, uint64_t added, fips_segment segment,
                          bool unfunded)
{
    return (fips_placed){outcome, added, segment, unfunded};
}

/* A new key's slot, filled but not yet filed. NONE when storage is exhausted. */
static uint32_t new_slot(fips_coord_cache *c, const uint8_t addr[16], uint64_t hash,
                         const fips_cached_coord *coords, uint64_t now, uint64_t ttl_ms,
                         uint64_t weight)
{
    uint32_t idx = alloc_slot(c);
    if (idx == NONE) return NONE;
    fips_coord_slot *slot = &c->slots[idx];
    fips_cache_entry_init(&slot->entry, coords, now, ttl_ms);
    memcpy(slot->addr, addr, 16);
    slot->hash = hash;
    slot->weight = weight;
    return idx;
}

static fips_status place_hint(fips_coord_cache *c, const uint8_t addr[16],
                              const fips_cached_coord *coords, uint64_t now, uint64_t ttl_ms,
                              fips_funding funding, bool stated, fips_placed *out)
{
    uint64_t weight = weight_of(coords), hash = 0;
    uint32_t idx = find(c, addr, &hash);
    /* An expired entry is no route: a write of its key is a new key's write. */
    if (idx != NONE && fips_cache_entry_is_expired(&c->slots[idx].entry, now)) {
        remove_slot(c, idx);
        idx = NONE;
    }
    if (idx == NONE) {
        if (!covers(funding, weight)) {
            make_probation_room(c);
            idx = new_slot(c, addr, hash, coords, now, ttl_ms, weight);
            if (idx == NONE) return FIPS_ERR_RANGE;
            file_new(c, idx, FIPS_SEGMENT_PROBATION);
            *out = placed(FIPS_HINT_INSERTED, 0, FIPS_SEGMENT_PROBATION, true);
            return FIPS_OK;
        }
        /* Eviction declines when every entry is live-verified: a link peer's
         * write goes to probation, an uncharged write is refused. */
        if (!make_room(c, weight, now)) {
            if (funding.charged) {
                make_probation_room(c);
                idx = new_slot(c, addr, hash, coords, now, ttl_ms, weight);
                if (idx == NONE) return FIPS_ERR_RANGE;
                file_new(c, idx, FIPS_SEGMENT_PROBATION);
                *out = placed(FIPS_HINT_INSERTED, 0, FIPS_SEGMENT_PROBATION, false);
                return FIPS_OK;
            }
            *out = placed(FIPS_HINT_REJECTED, 0, FIPS_SEGMENT_MAIN, false);
            return FIPS_OK;
        }
        idx = new_slot(c, addr, hash, coords, now, ttl_ms, weight);
        if (idx == NONE) return FIPS_ERR_RANGE;
        file_new(c, idx, FIPS_SEGMENT_MAIN);
        *out = placed(FIPS_HINT_INSERTED, weight, FIPS_SEGMENT_MAIN, false);
        return FIPS_OK;
    }

    fips_coord_slot *slot = &c->slots[idx];
    bool differs = !fips_cached_coord_equal(&slot->entry.coords, coords);
    fips_segment segment = segment_of_place(slot->place);
    if (fips_cache_entry_is_verified(&slot->entry, now) && !(stated && differs)) {
        *out = placed(FIPS_HINT_REJECTED, 0, segment, false);
        return FIPS_OK;
    }
    fips_hint_outcome outcome = differs ? FIPS_HINT_CHANGED : FIPS_HINT_UNCHANGED;
    uint64_t old_weight = slot->weight;
    uint64_t charge = weight > old_weight ? weight : old_weight;

    if (segment == FIPS_SEGMENT_PROBATION && covers(funding, charge)) {
        /* Promotion: pays its whole weight, at least what probation held. */
        unfile(c, idx, true);
        fips_cache_entry_update(&slot->entry, coords, now, ttl_ms);
        slot->weight = charge;
        if (make_room(c, charge, now)) {
            file_new(c, idx, FIPS_SEGMENT_MAIN);
            *out = placed(outcome, charge, FIPS_SEGMENT_MAIN, false);
        } else {
            file_new(c, idx, FIPS_SEGMENT_PROBATION);
            *out = placed(outcome, 0, FIPS_SEGMENT_PROBATION, false);
        }
    } else if (segment == FIPS_SEGMENT_PROBATION) {
        update_in_place(c, idx, coords, now, ttl_ms, weight);
        *out = placed(outcome, 0, FIPS_SEGMENT_PROBATION, false);
    } else if (weight <= old_weight) {
        update_in_place(c, idx, coords, now, ttl_ms, weight);
        *out = placed(outcome, 0, FIPS_SEGMENT_MAIN, false);
    } else if (!covers(funding, weight - old_weight)) {
        *out = placed(FIPS_HINT_REJECTED, 0, FIPS_SEGMENT_MAIN, true);
    } else {
        /* A growing update makes room like a new key; taken out first so it
         * cannot be its own victim, and put back unchanged if no room can be
         * made, unless it is a node's statement of its own coordinates. */
        unfile(c, idx, true);
        if (!make_room(c, weight, now) && !stated) {
            file_new(c, idx, FIPS_SEGMENT_MAIN);
            *out = placed(FIPS_HINT_REJECTED, 0, FIPS_SEGMENT_MAIN, false);
            return FIPS_OK;
        }
        fips_cache_entry_update(&slot->entry, coords, now, ttl_ms);
        slot->weight = weight;
        file_new(c, idx, FIPS_SEGMENT_MAIN);
        *out = placed(outcome, weight - old_weight, FIPS_SEGMENT_MAIN, false);
    }
    return FIPS_OK;
}

fips_status fips_coord_cache_init(fips_coord_cache *c, fips_coord_slot *slots,
                                  size_t slot_capacity, uint32_t *index, size_t index_words,
                                  size_t max_entries, uint64_t default_ttl_ms,
                                  fips_instant epoch, fips_entropy entropy)
{
    if (slot_capacity > INT32_MAX || max_entries > slot_capacity ||
        slot_capacity < FIPS_COORD_CACHE_SLOTS(max_entries) ||
        index_words < FIPS_COORD_CACHE_INDEX_WORDS(slot_capacity))
        return FIPS_ERR_RANGE;
    memset(c, 0, sizeof(*c));
    c->slots = slots;
    c->slot_capacity = slot_capacity;
    c->by_use = index;
    c->parked = index + slot_capacity;
    c->probation = index + 2 * slot_capacity;
    c->expiry = index + 3 * slot_capacity;
    c->table = index + 4 * slot_capacity;
    c->table_len = 2 * slot_capacity + 1;
    c->max_entries = max_entries;
    c->default_ttl_ms = default_ttl_ms;
    c->epoch = epoch;
    entropy.fill(entropy.ctx, c->hash_key, sizeof(c->hash_key));
    fips_coord_cache_clear(c);
    return FIPS_OK;
}

void fips_coord_cache_clear(fips_coord_cache *c)
{
    for (size_t i = 0; i < c->table_len; ++i) c->table[i] = NONE;
    c->free_head = NONE;
    for (size_t i = c->slot_capacity; i-- > 0;) {
        c->slots[i].used = false;
        c->slots[i].link = c->free_head;
        c->free_head = (uint32_t)i;
    }
    c->len = c->by_use_len = c->parked_len = c->probation_len = c->expiry_len = 0;
    c->stored_weight = 0;
    ++c->generation;
}

uint64_t fips_coord_cache_millis(const fips_coord_cache *c, fips_instant now)
{
    fips_duration d = now.ops->saturating_duration_since(now, c->epoch);
    uint64_t fraction = d.nanos / 1000000;
    if (d.secs > (UINT64_MAX - fraction) / 1000) return UINT64_MAX;
    return d.secs * 1000 + fraction;
}

size_t fips_coord_cache_max_entries(const fips_coord_cache *c) { return c->max_entries; }
uint64_t fips_coord_cache_default_ttl_ms(const fips_coord_cache *c) { return c->default_ttl_ms; }
void fips_coord_cache_set_default_ttl_ms(fips_coord_cache *c, uint64_t ttl_ms)
{
    c->default_ttl_ms = ttl_ms;
}

fips_status fips_coord_cache_insert(fips_coord_cache *c, const uint8_t addr[16],
                                    const fips_cached_coord *coords, fips_instant now,
                                    fips_hint_outcome *out)
{
    return fips_coord_cache_insert_with_ttl(c, addr, coords, now, c->default_ttl_ms, out);
}

fips_status fips_coord_cache_insert_funded(fips_coord_cache *c, const uint8_t addr[16],
                                           const fips_cached_coord *coords, fips_instant now,
                                           fips_funding funding, fips_placed *out)
{
    uint64_t now_ms = fips_coord_cache_millis(c, now);
    return place_hint(c, addr, coords, now_ms, c->default_ttl_ms, funding, false, out);
}

fips_status fips_coord_cache_insert_stated(fips_coord_cache *c, const uint8_t addr[16],
                                           const fips_cached_coord *coords, fips_instant now,
                                           fips_funding funding, fips_placed *out)
{
    uint64_t now_ms = fips_coord_cache_millis(c, now);
    bool stated = memcmp(fips_cached_coord_node_addr(coords), addr, 16) == 0;
    return place_hint(c, addr, coords, now_ms, c->default_ttl_ms, funding, stated, out);
}

fips_status fips_coord_cache_insert_with_ttl(fips_coord_cache *c, const uint8_t addr[16],
                                             const fips_cached_coord *coords, fips_instant now,
                                             uint64_t ttl_ms, fips_hint_outcome *out)
{
    uint64_t now_ms = fips_coord_cache_millis(c, now);
    fips_placed result;
    fips_status status = place_hint(c, addr, coords, now_ms, ttl_ms, FIPS_FUNDING_UNCHARGED,
                                    false, &result);
    if (status == FIPS_OK) *out = result.outcome;
    return status;
}

fips_status fips_coord_cache_insert_verified(fips_coord_cache *c, const uint8_t addr[16],
                                             const fips_cached_coord *coords, fips_instant now)
{
    uint64_t now_ms = fips_coord_cache_millis(c, now), weight = weight_of(coords), hash = 0;
    uint32_t idx = find(c, addr, &hash);
    if (idx != NONE) {
        unfile(c, idx, true);
        fips_cache_entry_update_verified(&c->slots[idx].entry, coords, now_ms, c->default_ttl_ms);
    }
    (void)make_room(c, weight, now_ms);
    if (idx == NONE) {
        idx = alloc_slot(c);
        if (idx == NONE) return FIPS_ERR_RANGE;
        fips_coord_slot *slot = &c->slots[idx];
        fips_cache_entry_init_verified(&slot->entry, coords, now_ms, c->default_ttl_ms);
        memcpy(slot->addr, addr, 16);
        slot->hash = hash;
    }
    c->slots[idx].weight = weight;
    file_new(c, idx, FIPS_SEGMENT_MAIN);
    return FIPS_OK;
}

fips_status fips_coord_cache_insert_verified_with_path_mtu(fips_coord_cache *c,
    const uint8_t addr[16], const fips_cached_coord *coords, fips_instant now, uint16_t path_mtu)
{
    fips_status status = fips_coord_cache_insert_verified(c, addr, coords, now);
    if (status != FIPS_OK) return status;
    uint32_t idx = find(c, addr, NULL);
    if (idx != NONE) { fips_cache_entry_set_path_mtu(&c->slots[idx].entry, path_mtu); ++c->generation; }
    return FIPS_OK;
}

const fips_cached_coord *fips_coord_cache_get(const fips_coord_cache *c, const uint8_t addr[16],
                                              fips_instant now)
{
    uint32_t idx = find(c, addr, NULL);
    if (idx == NONE) return NULL;
    const fips_cache_entry *entry = &c->slots[idx].entry;
    return fips_cache_entry_is_expired(entry, fips_coord_cache_millis(c, now)) ? NULL
                                                                                : &entry->coords;
}

const fips_cached_coord *fips_coord_cache_get_and_touch(fips_coord_cache *c,
                                                        const uint8_t addr[16], fips_instant now)
{
    uint32_t idx = find(c, addr, NULL);
    if (idx == NONE) return NULL;
    uint64_t now_ms = fips_coord_cache_millis(c, now);
    fips_cache_entry *entry = &c->slots[idx].entry;
    if (fips_cache_entry_is_expired(entry, now_ms)) {
        remove_slot(c, idx);
        return NULL;
    }
    fips_cache_entry_refresh(entry, now_ms, c->default_ttl_ms);
    expiry_fix(c, idx);
    ++c->generation;
    return &entry->coords;
}

const fips_cache_entry *fips_coord_cache_get_entry(const fips_coord_cache *c,
                                                   const uint8_t addr[16])
{
    uint32_t idx = find(c, addr, NULL);
    return idx == NONE ? NULL : &c->slots[idx].entry;
}

bool fips_coord_cache_remove(fips_coord_cache *c, const uint8_t addr[16], fips_cache_entry *out)
{
    uint32_t idx = find(c, addr, NULL);
    if (idx == NONE) return false;
    if (out) *out = c->slots[idx].entry;
    remove_slot(c, idx);
    return true;
}

bool fips_coord_cache_demote(fips_coord_cache *c, const uint8_t addr[16])
{
    uint32_t idx = find(c, addr, NULL);
    if (idx == NONE) return false;
    fips_cache_entry_mark_hint(&c->slots[idx].entry);
    if (c->slots[idx].place == FIPS_PLACE_PARKED) refile(c, idx);
    ++c->generation;
    return true;
}

void fips_coord_cache_clear_path_mtu(fips_coord_cache *c, const uint8_t addr[16])
{
    uint32_t idx = find(c, addr, NULL);
    if (idx == NONE) return;
    fips_cache_entry_clear_path_mtu(&c->slots[idx].entry);
    ++c->generation;
}

bool fips_coord_cache_contains(const fips_coord_cache *c, const uint8_t addr[16],
                               fips_instant now)
{
    return fips_coord_cache_get(c, addr, now) != NULL;
}

size_t fips_coord_cache_len(const fips_coord_cache *c) { return c->len; }
uint64_t fips_coord_cache_generation(const fips_coord_cache *c) { return c->generation; }
bool fips_coord_cache_is_empty(const fips_coord_cache *c) { return c->len == 0; }

void fips_coord_cache_iter(const fips_coord_cache *c, fips_instant now,
                           fips_coord_cache_visitor visit, void *ctx)
{
    uint64_t now_ms = fips_coord_cache_millis(c, now);
    for (size_t i = 0; i < c->slot_capacity; ++i) {
        const fips_coord_slot *slot = &c->slots[i];
        if (!slot->used || fips_cache_entry_is_expired(&slot->entry, now_ms)) continue;
        if (!visit(ctx, slot->addr, &slot->entry)) return;
    }
}

typedef bool (*keep_fn)(const fips_cache_entry *entry, const uint8_t addr[16]);

static size_t retain_entries(fips_coord_cache *c, keep_fn keep, const uint8_t addr[16])
{
    size_t before = c->len;
    for (size_t i = 0; i < c->slot_capacity; ++i)
        if (c->slots[i].used && !keep(&c->slots[i].entry, addr)) remove_slot(c, (uint32_t)i);
    return before - c->len;
}

static bool keep_not_via(const fips_cache_entry *entry, const uint8_t addr[16])
{
    return !fips_cached_coord_contains(&entry->coords, addr);
}

static bool keep_same_root(const fips_cache_entry *entry, const uint8_t addr[16])
{
    return memcmp(fips_cached_coord_root_id(&entry->coords), addr, 16) == 0;
}

/* In-place heapsort of slot indexes: purge takes no scratch and allocates nothing. */
static void sort_down(uint32_t *v, size_t n, size_t pos)
{
    uint32_t x = v[pos];
    for (size_t child; (child = 2 * pos + 1) < n; pos = child) {
        if (child + 1 < n && v[child + 1] > v[child]) ++child;
        if (v[child] <= x) break;
        v[pos] = v[child];
    }
    v[pos] = x;
}

static void sort_indexes(uint32_t *v, size_t n)
{
    for (size_t i = n / 2; i-- > 0;) sort_down(v, n, i);
    for (size_t end = n; end > 1;) {
        uint32_t top = v[0];
        v[0] = v[--end];
        v[end] = top;
        sort_down(v, end, 0);
    }
}

/* Pop the expired entries off the expiry heap, parking each in the array slot
 * the shrinking heap gives up, then remove them in ascending slot order: what
 * the storage-order sweep upstream's retain amounts to removes, in its order,
 * so the free list (and with it where later inserts land) is the sweep's. */
size_t fips_coord_cache_purge_expired(fips_coord_cache *c, fips_instant now)
{
    uint64_t now_ms = fips_coord_cache_millis(c, now);
    size_t end = c->expiry_len;
    while (c->expiry_len > 0 && fips_cache_entry_is_expired(&c->slots[c->expiry[0]].entry, now_ms)) {
        uint32_t idx = c->expiry[0];
        expiry_delete(c, 0);
        c->slots[idx].expiry_pos = NONE;
        c->expiry[c->expiry_len] = idx;
    }
    uint32_t *expired = c->expiry + c->expiry_len;
    size_t count = end - c->expiry_len;
    sort_indexes(expired, count);
    for (size_t i = 0; i < count; ++i) remove_slot(c, expired[i]);
    return count;
}

size_t fips_coord_cache_invalidate_via_node(fips_coord_cache *c, const uint8_t node_addr[16])
{
    return retain_entries(c, keep_not_via, node_addr);
}

size_t fips_coord_cache_invalidate_other_roots(fips_coord_cache *c, const uint8_t current_root[16])
{
    return retain_entries(c, keep_same_root, current_root);
}

fips_cache_stats fips_coord_cache_stats(const fips_coord_cache *c, fips_instant now)
{
    uint64_t now_ms = fips_coord_cache_millis(c, now), total_age = 0, next_expiry = UINT64_MAX;
    size_t expired = 0;
    for (size_t i = 0; i < c->slot_capacity; ++i) {
        const fips_coord_slot *slot = &c->slots[i];
        if (!slot->used) continue;
        if (fips_cache_entry_is_expired(&slot->entry, now_ms)) ++expired;
        else if (slot->entry.expires_at < next_expiry) next_expiry = slot->entry.expires_at;
        uint64_t age = fips_cache_entry_age(&slot->entry, now_ms);
        total_age = total_age > UINT64_MAX - age ? UINT64_MAX : total_age + age;
    }
    return (fips_cache_stats){c->len, c->max_entries, expired,
                              c->len == 0 ? 0 : total_age / (uint64_t)c->len, next_expiry};
}

typedef struct scan_acc { uint64_t now_ms, total_age, next_expiry; size_t expired; } scan_acc;

static unsigned low_bit(uint64_t bits) /* bits != 0 */
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(bits);
#else
    unsigned n = 0;
    while (!(bits & 1)) { bits >>= 1; ++n; }
    return n;
#endif
}

static void scan_slot(const fips_coord_slot *slot, scan_acc *acc, fips_coord_cache_visitor *visit, void *ctx)
{
    bool expired = fips_cache_entry_is_expired(&slot->entry, acc->now_ms);
    /* Branch-free: this loop is bound by memory latency over ~51k slots. */
    uint64_t live_expiry = expired ? UINT64_MAX : slot->entry.expires_at;
    acc->expired += expired;
    acc->next_expiry = live_expiry < acc->next_expiry ? live_expiry : acc->next_expiry;
    uint64_t age = fips_cache_entry_age(&slot->entry, acc->now_ms);
    acc->total_age = acc->total_age > UINT64_MAX - age ? UINT64_MAX : acc->total_age + age;
    if (!expired && *visit && !(*visit)(ctx, slot->addr, &slot->entry)) *visit = NULL;
}

/* Every resident slot is filed in exactly one of the three heaps (unfile and
 * file keep that whole between public calls), so marking heap members finds
 * them without touching the free slots; the bitmap restores storage order. */
fips_cache_stats fips_coord_cache_scan(const fips_coord_cache *c, fips_instant now,
                                       uint64_t *marks, size_t mark_words,
                                       fips_coord_cache_visitor visit, void *ctx)
{
    scan_acc acc = {fips_coord_cache_millis(c, now), 0, UINT64_MAX, 0};
    size_t words = FIPS_COORD_CACHE_MARK_WORDS(c->slot_capacity);
    bool indexed = marks && mark_words >= words && c->by_use_len + c->parked_len + c->probation_len == c->len;
    if (indexed) {
        memset(marks, 0, words * sizeof *marks);
        const uint32_t *heaps[3] = {c->by_use, c->parked, c->probation};
        size_t lens[3] = {c->by_use_len, c->parked_len, c->probation_len};
        for (size_t h = 0; h < 3; ++h)
            for (size_t i = 0; i < lens[h]; ++i) marks[heaps[h][i] / 64] |= UINT64_C(1) << (heaps[h][i] % 64);
        for (size_t w = 0; w < words; ++w)
            for (uint64_t bits = marks[w]; bits; bits &= bits - 1)
                scan_slot(&c->slots[w * 64 + low_bit(bits)], &acc, &visit, ctx);
    } else {
        for (size_t i = 0; i < c->slot_capacity; ++i)
            if (c->slots[i].used) scan_slot(&c->slots[i], &acc, &visit, ctx);
    }
    return (fips_cache_stats){c->len, c->max_entries, acc.expired,
                              c->len == 0 ? 0 : acc.total_age / (uint64_t)c->len, acc.next_expiry};
}

uint64_t fips_coord_cache_eviction_examined(const fips_coord_cache *c)
{
    return c->eviction_examined;
}

bool fips_coord_cache_segment_of(const fips_coord_cache *c, const uint8_t addr[16],
                                 fips_segment *out)
{
    uint32_t idx = find(c, addr, NULL);
    if (idx == NONE) return false;
    *out = segment_of_place(c->slots[idx].place);
    return true;
}

size_t fips_coord_cache_probation_len(const fips_coord_cache *c) { return c->probation_len; }

size_t fips_coord_cache_stored_addrs(const fips_coord_cache *c)
{
    size_t total = 0;
    for (size_t i = 0; i < c->slot_capacity; ++i)
        if (c->slots[i].used) total += c->slots[i].entry.coords.len;
    return total;
}

size_t fips_coord_cache_main_len(const fips_coord_cache *c)
{
    return c->by_use_len + c->parked_len;
}

size_t fips_coord_cache_probation_bound(const fips_coord_cache *c) { return probation_bound(c); }
uint64_t fips_coord_cache_weight_bound(const fips_coord_cache *c) { return weight_bound(c); }
