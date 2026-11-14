#ifndef FIPS_CACHE_COORD_CACHE_H
#define FIPS_CACHE_COORD_CACHE_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Mirrors upstream core/src/cache/{coord_cache,entry,mod}.rs by name. */
#define FIPS_DEFAULT_COORD_CACHE_SIZE 50000
#define FIPS_DEFAULT_COORD_CACHE_TTL_MS UINT64_C(300000)
#define FIPS_VERIFIED_TTL_MS UINT64_C(300000)
#define FIPS_ADDRS_PER_ENTRY 16
#define FIPS_PROBATION_SHARE_DIVISOR 32

/* Coordinate storage owned by this module only until nostrc-82g9n.1 lands the
 * proto coord codec (upstream core/src/proto/coord.rs TreeCoordinate). This is
 * that type's value layout, (node_addr, sequence, timestamp) per hop from the
 * node to the root, held inline instead of in a Vec, with no codec. Upstream
 * has no depth constant: the wire count is u16, but a frame carries about
 * seventy addresses (coord_cache.rs ADDRS_PER_ENTRY note), so the inline bound
 * is a cache storage choice. 82g9n.1 should replace fips_cached_coord with
 * the real type (or a conversion) without changing the cache API shape. */
#define FIPS_CACHED_COORD_MAX_ENTRIES 80
typedef struct fips_coord_entry {
    uint8_t node_addr[16];
    uint64_t sequence, timestamp;
} fips_coord_entry;
typedef struct fips_cached_coord {
    fips_coord_entry entries[FIPS_CACHED_COORD_MAX_ENTRIES];
    uint16_t len; /* >= 1 for a valid coordinate; depth is len - 1 */
} fips_cached_coord;
/** FIPS_ERR_RANGE for count == 0 or count > FIPS_CACHED_COORD_MAX_ENTRIES
 * (out unchanged). Entries get sequence = timestamp = 0, like from_addrs. */
FIPS_CORE_API fips_status fips_cached_coord_from_addrs(fips_cached_coord *out,
    const uint8_t (*addrs)[16], size_t count);
/** Rust PartialEq: every entry's address, sequence and timestamp. */
FIPS_CORE_API bool fips_cached_coord_equal(const fips_cached_coord *a,
                                           const fips_cached_coord *b);
FIPS_CORE_API bool fips_cached_coord_contains(const fips_cached_coord *c, const uint8_t addr[16]);
FIPS_CORE_API const uint8_t *fips_cached_coord_node_addr(const fips_cached_coord *c);
FIPS_CORE_API const uint8_t *fips_cached_coord_root_id(const fips_cached_coord *c);

typedef enum fips_coord_source { FIPS_COORD_VERIFIED, FIPS_COORD_HINT } fips_coord_source;
/** One cached coordinate. Times are cache-relative milliseconds: the cache
 * converts each fips_instant against its init epoch (fips_coord_cache_millis);
 * the entry helpers take those milliseconds and read no clock. Fields are
 * public for reading; mutate only through the helpers or the cache. */
typedef struct fips_cache_entry {
    fips_cached_coord coords;
    uint64_t created_at, last_used, expires_at;
    uint64_t verified_until; /* 0 for a hint; never extended by refresh/touch */
    fips_coord_source source;
    bool has_path_mtu;
    uint16_t path_mtu;
} fips_cache_entry;
FIPS_CORE_API void fips_cache_entry_init(fips_cache_entry *e, const fips_cached_coord *coords,
                                         uint64_t now_ms, uint64_t ttl_ms);
FIPS_CORE_API void fips_cache_entry_init_verified(fips_cache_entry *e,
    const fips_cached_coord *coords, uint64_t now_ms, uint64_t ttl_ms);
FIPS_CORE_API bool fips_cache_entry_is_verified(const fips_cache_entry *e, uint64_t now_ms);
FIPS_CORE_API void fips_cache_entry_mark_verified(fips_cache_entry *e, uint64_t now_ms);
FIPS_CORE_API void fips_cache_entry_mark_hint(fips_cache_entry *e);
/** Expired strictly after expires_at; at expires_at it is still live. */
FIPS_CORE_API bool fips_cache_entry_is_expired(const fips_cache_entry *e, uint64_t now_ms);
FIPS_CORE_API void fips_cache_entry_touch(fips_cache_entry *e, uint64_t now_ms);
/** Resets expires_at and last_used only: not created_at, not verified_until. */
FIPS_CORE_API void fips_cache_entry_refresh(fips_cache_entry *e, uint64_t now_ms, uint64_t ttl_ms);
/** New coordinates are new provenance: the entry becomes a hint. */
FIPS_CORE_API void fips_cache_entry_update(fips_cache_entry *e, const fips_cached_coord *coords,
                                           uint64_t now_ms, uint64_t ttl_ms);
FIPS_CORE_API void fips_cache_entry_update_verified(fips_cache_entry *e,
    const fips_cached_coord *coords, uint64_t now_ms, uint64_t ttl_ms);
FIPS_CORE_API void fips_cache_entry_set_path_mtu(fips_cache_entry *e, uint16_t mtu);
FIPS_CORE_API void fips_cache_entry_clear_path_mtu(fips_cache_entry *e);
FIPS_CORE_API uint64_t fips_cache_entry_idle_time(const fips_cache_entry *e, uint64_t now_ms);
FIPS_CORE_API uint64_t fips_cache_entry_age(const fips_cache_entry *e, uint64_t now_ms);
FIPS_CORE_API uint64_t fips_cache_entry_time_to_expiry(const fips_cache_entry *e, uint64_t now_ms);

/** Funding::Upto(n) is {true, n}; Funding::Uncharged is {false, 0}. */
typedef struct fips_funding { bool charged; uint64_t upto; } fips_funding;
#define FIPS_FUNDING_UNCHARGED ((fips_funding){false, 0})
#define FIPS_FUNDING_UPTO(n) ((fips_funding){true, (n)})
typedef enum fips_segment { FIPS_SEGMENT_MAIN, FIPS_SEGMENT_PROBATION } fips_segment;
typedef enum fips_hint_outcome {
    FIPS_HINT_INSERTED, FIPS_HINT_CHANGED, FIPS_HINT_UNCHANGED, FIPS_HINT_REJECTED
} fips_hint_outcome;
typedef struct fips_placed {
    fips_hint_outcome outcome;
    uint64_t added_weight;
    fips_segment segment;
    bool unfunded;
} fips_placed;
typedef enum fips_cache_place { FIPS_PLACE_BY_USE, FIPS_PLACE_PARKED, FIPS_PLACE_PROBATION } fips_cache_place;
typedef struct fips_cache_stats { size_t entries, max_entries, expired; uint64_t avg_age_ms; } fips_cache_stats;
FIPS_CORE_API double fips_cache_stats_fill_ratio(const fips_cache_stats *s);
typedef enum fips_cache_error { FIPS_CACHE_FULL, FIPS_CACHE_NOT_FOUND, FIPS_CACHE_EXPIRED } fips_cache_error;
FIPS_CORE_API const char *fips_cache_error_name(fips_cache_error e);

/* Storage layout is public for caller allocation and diagnostics, not for
 * direct mutation. The default-size cache is large (about 140 MB of slots):
 * use host heap/static storage, never an event-loop stack. */
typedef struct fips_coord_slot {
    fips_cache_entry entry;
    uint8_t addr[16];
    uint64_t hash, key_stamp, key_seq; /* (stamp, seq) index key, like Rust */
    uint64_t weight;                   /* charge against the address bound */
    uint32_t heap_pos, link;           /* link: free list / eviction scratch */
    fips_cache_place place;
    bool used;
} fips_coord_slot;
typedef struct fips_coord_cache {
    fips_coord_slot *slots;
    uint32_t *table, *by_use, *parked, *probation; /* slot indexes */
    size_t slot_capacity, table_len, len, by_use_len, parked_len, probation_len;
    uint32_t free_head;
    uint64_t seq, stored_weight, default_ttl_ms, eviction_examined;
    size_t max_entries;
    fips_instant epoch;
    uint8_t hash_key[16];
} fips_coord_cache;
/** Rust's map grows past max_entries only for verified writes (this node's
 * own lookups). C bounds total residency by caller storage: at least
 * FIPS_COORD_CACHE_SLOTS(max_entries) slots (main plus probation) so hint
 * paths never run out, plus whatever headroom the host grants verified excess. */
#define FIPS_COORD_CACHE_PROBATION_BOUND(m) \
    ((m) / FIPS_PROBATION_SHARE_DIVISOR > 0 ? (m) / FIPS_PROBATION_SHARE_DIVISOR : (size_t)1)
#define FIPS_COORD_CACHE_SLOTS(m) ((m) + FIPS_COORD_CACHE_PROBATION_BOUND(m))
/** uint32_t words of index storage for slot_capacity slots: three eviction
 * heaps plus a hash table at most half full. */
#define FIPS_COORD_CACHE_INDEX_WORDS(cap) (5 * (size_t)(cap) + 1)
/** Caller owns cache, slots and index for the whole lifetime; no allocation.
 * Hint keys are peer-chosen, so the hash key is drawn from host entropy at
 * init, like Rust's KeyedHasher (iteration order is per-process). epoch is
 * cache-relative millisecond zero; every later instant must share its clock
 * domain. default_ttl_ms is a plain runtime parameter so a host can map
 * node.cache.coord_ttl_secs / test_coord_ttl_secs (x 1000) onto it.
 * FIPS_ERR_RANGE: slot_capacity below FIPS_COORD_CACHE_SLOTS(max_entries) or
 * above INT32_MAX, or too few index words; the cache is left uninitialized.
 * Probation always holds one entry, so even max_entries == 0 needs one slot. */
FIPS_CORE_API fips_status fips_coord_cache_init(fips_coord_cache *c, fips_coord_slot *slots,
    size_t slot_capacity, uint32_t *index, size_t index_words, size_t max_entries,
    uint64_t default_ttl_ms, fips_instant epoch, fips_entropy entropy);
/** Saturating milliseconds of now since the epoch; what the entries record. */
FIPS_CORE_API uint64_t fips_coord_cache_millis(const fips_coord_cache *c, fips_instant now);
FIPS_CORE_API size_t fips_coord_cache_max_entries(const fips_coord_cache *c);
FIPS_CORE_API uint64_t fips_coord_cache_default_ttl_ms(const fips_coord_cache *c);
FIPS_CORE_API void fips_coord_cache_set_default_ttl_ms(fips_coord_cache *c, uint64_t ttl_ms);
/** Hint writes. Outcomes, charges, segments and refusals follow upstream
 * exactly (see coord_cache.rs insert/insert_funded/insert_stated); eviction
 * takes the least recently used non-live-verified main entry, probation evicts
 * only probation. FIPS_ERR_RANGE only when every free slot is held by verified
 * entries beyond max_entries: nothing changed, out unchanged. */
FIPS_CORE_API fips_status fips_coord_cache_insert(fips_coord_cache *c, const uint8_t addr[16],
    const fips_cached_coord *coords, fips_instant now, fips_hint_outcome *out);
FIPS_CORE_API fips_status fips_coord_cache_insert_funded(fips_coord_cache *c,
    const uint8_t addr[16], const fips_cached_coord *coords, fips_instant now,
    fips_funding funding, fips_placed *out);
FIPS_CORE_API fips_status fips_coord_cache_insert_stated(fips_coord_cache *c,
    const uint8_t addr[16], const fips_cached_coord *coords, fips_instant now,
    fips_funding funding, fips_placed *out);
FIPS_CORE_API fips_status fips_coord_cache_insert_with_ttl(fips_coord_cache *c,
    const uint8_t addr[16], const fips_cached_coord *coords, fips_instant now,
    uint64_t ttl_ms, fips_hint_outcome *out);
/** Unconditional, stored even when no room can be made (Rust grows; C needs a
 * free slot: FIPS_ERR_RANGE with nothing changed if there is none). */
FIPS_CORE_API fips_status fips_coord_cache_insert_verified(fips_coord_cache *c,
    const uint8_t addr[16], const fips_cached_coord *coords, fips_instant now);
FIPS_CORE_API fips_status fips_coord_cache_insert_verified_with_path_mtu(fips_coord_cache *c,
    const uint8_t addr[16], const fips_cached_coord *coords, fips_instant now, uint16_t path_mtu);
/** Borrowed pointers are invalidated by the next mutating call. NULL if
 * absent or expired. get_and_touch removes an expired entry and otherwise
 * refreshes it (last_used, expires_at by the default TTL; no index work). */
FIPS_CORE_API const fips_cached_coord *fips_coord_cache_get(const fips_coord_cache *c,
    const uint8_t addr[16], fips_instant now);
FIPS_CORE_API const fips_cached_coord *fips_coord_cache_get_and_touch(fips_coord_cache *c,
    const uint8_t addr[16], fips_instant now);
FIPS_CORE_API const fips_cache_entry *fips_coord_cache_get_entry(const fips_coord_cache *c,
                                                                 const uint8_t addr[16]);
/** false if absent; out (optional) receives the removed entry. */
FIPS_CORE_API bool fips_coord_cache_remove(fips_coord_cache *c, const uint8_t addr[16],
                                           fips_cache_entry *out);
FIPS_CORE_API bool fips_coord_cache_demote(fips_coord_cache *c, const uint8_t addr[16]);
FIPS_CORE_API void fips_coord_cache_clear_path_mtu(fips_coord_cache *c, const uint8_t addr[16]);
FIPS_CORE_API bool fips_coord_cache_contains(const fips_coord_cache *c, const uint8_t addr[16],
                                             fips_instant now);
FIPS_CORE_API size_t fips_coord_cache_len(const fips_coord_cache *c); /* incl. expired */
FIPS_CORE_API bool fips_coord_cache_is_empty(const fips_coord_cache *c);
/** Rust iter(): non-expired entries, in storage order (upstream order is
 * random per process; nothing may depend on it). Return false to stop. */
typedef bool (*fips_coord_cache_visitor)(void *ctx, const uint8_t addr[16],
                                         const fips_cache_entry *entry);
FIPS_CORE_API void fips_coord_cache_iter(const fips_coord_cache *c, fips_instant now,
                                         fips_coord_cache_visitor visit, void *ctx);
FIPS_CORE_API size_t fips_coord_cache_purge_expired(fips_coord_cache *c, fips_instant now);
FIPS_CORE_API void fips_coord_cache_clear(fips_coord_cache *c);
FIPS_CORE_API size_t fips_coord_cache_invalidate_via_node(fips_coord_cache *c,
                                                          const uint8_t node_addr[16]);
FIPS_CORE_API size_t fips_coord_cache_invalidate_other_roots(fips_coord_cache *c,
                                                             const uint8_t current_root[16]);
FIPS_CORE_API fips_cache_stats fips_coord_cache_stats(const fips_coord_cache *c, fips_instant now);
/* Upstream test-hooks feature, always available here (read-only, O(1) except
 * stored_addrs, which is O(n)). */
FIPS_CORE_API uint64_t fips_coord_cache_eviction_examined(const fips_coord_cache *c);
FIPS_CORE_API bool fips_coord_cache_segment_of(const fips_coord_cache *c, const uint8_t addr[16],
                                               fips_segment *out);
FIPS_CORE_API size_t fips_coord_cache_probation_len(const fips_coord_cache *c);
FIPS_CORE_API size_t fips_coord_cache_stored_addrs(const fips_coord_cache *c);
FIPS_CORE_API size_t fips_coord_cache_main_len(const fips_coord_cache *c);
FIPS_CORE_API size_t fips_coord_cache_probation_bound(const fips_coord_cache *c);
FIPS_CORE_API uint64_t fips_coord_cache_weight_bound(const fips_coord_cache *c);
#ifdef __cplusplus
}
#endif
#endif
