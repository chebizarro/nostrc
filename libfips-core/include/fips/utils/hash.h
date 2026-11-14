#ifndef FIPS_UTILS_HASH_H
#define FIPS_UTILS_HASH_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Internal key material: never log it. init draws all 16 bytes in ONE fill. */
typedef struct fips_keyed_hasher { uint64_t k0, k1; } fips_keyed_hasher;
FIPS_CORE_API void fips_keyed_hasher_init(fips_keyed_hasher *h, fips_entropy entropy);
FIPS_CORE_API void fips_keyed_hasher_clear(fips_keyed_hasher *h);
/* Raw bytes, no Rust Hash type/length framing. NULL bytes allowed if len == 0.
 * Applications must encode composite keys unambiguously before hashing. */
FIPS_CORE_API uint64_t fips_siphash13(const fips_keyed_hasher *h,
    const uint8_t *bytes, size_t len);

typedef struct fips_hash_slot {
    const uint8_t *key;
    size_t key_len;
    void *value;
    uint64_t hash;
    uint8_t state; /* Private: empty / occupied / tombstone. */
} fips_hash_slot;
typedef struct fips_hash_map {
    fips_hash_slot *slots;
    size_t capacity, count;
    fips_keyed_hasher hasher;
} fips_hash_map;
/* Caller-owned, nonoverlapping storage; capacity is a power of two >= 4,
 * <= SIZE_MAX / sizeof(fips_hash_slot). No allocation or automatic growth.
 * Max live entries: capacity - capacity/8 (capacity-1 for 4), like hashbrown's
 * 7/8 load policy. Host can double via rehash, bounded by its own policy.
 * Keys and values are borrowed: keys must remain immutable/alive until remove,
 * clear or deinit; values are never freed. Same-key insert replaces only value,
 * retaining the original key borrow. NULL key allowed for length zero; NULL
 * values are valid. Required pointers and initialized objects are preconditions.
 * Slot storage, map object and retained key bytes must not overlap.
 * Fields are private-by-contract. Failures preserve state and outputs and do
 * not consume entropy. Tombstones never cause false capacity refusal.
 */
FIPS_CORE_API fips_status fips_hash_map_init(fips_hash_map *m,
    fips_hash_slot *storage, size_t capacity, fips_entropy entropy);
FIPS_CORE_API size_t fips_hash_map_limit(const fips_hash_map *m);
FIPS_CORE_API size_t fips_hash_map_count(const fips_hash_map *m);
FIPS_CORE_API fips_status fips_hash_map_insert(fips_hash_map *m,
    const uint8_t *key, size_t len, void *value);
/* false leaves out unchanged (also for remove). */
FIPS_CORE_API bool fips_hash_map_get(const fips_hash_map *m,
    const uint8_t *key, size_t len, void **out);
FIPS_CORE_API bool fips_hash_map_remove(fips_hash_map *m,
    const uint8_t *key, size_t len, void **out);
/* Rehash into disjoint caller storage, retaining the key, with no entropy draw.
 * Old slots are cleared on success; on failure neither storage is changed. */
FIPS_CORE_API fips_status fips_hash_map_rehash(fips_hash_map *m,
    fips_hash_slot *storage, size_t capacity);
FIPS_CORE_API void fips_hash_map_clear(fips_hash_map *m);
/* Release all borrows and wipe the hash key; never frees caller storage. */
FIPS_CORE_API void fips_hash_map_deinit(fips_hash_map *m);
#ifdef __cplusplus
}
#endif
#endif
