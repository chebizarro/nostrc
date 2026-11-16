#ifndef FIPSD_GATEWAY_ADDRMAP_H
#define FIPSD_GATEWAY_ADDRMAP_H
/* A hash map from fixed-size byte keys (addresses, address pairs, offsets)
 * to 32-bit values: the port of the HashMap/HashSet instances in upstream's
 * pool, conntrack and NAT modules. Open addressing with tombstones; a key is
 * at most FIPSD_GW_MAP_KEY_MAX bytes. Removal during iteration is allowed. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FIPSD_GW_MAP_KEY_MAX 32u
typedef struct fipsd_gw_map {
    uint8_t *keys; uint32_t *values; uint8_t *slots; /* 0 empty, 1 used, 2 tombstone */
    size_t key_len, cap, len, occupied;
} fipsd_gw_map;
void fipsd_gw_map_init(fipsd_gw_map *map, size_t key_len);
void fipsd_gw_map_clear(fipsd_gw_map *map);
/* A pointer to the value stored for key, or NULL. Invalidated by set. */
uint32_t *fipsd_gw_map_find(const fipsd_gw_map *map, const void *key);
bool fipsd_gw_map_get(const fipsd_gw_map *map, const void *key, uint32_t *value);
bool fipsd_gw_map_contains(const fipsd_gw_map *map, const void *key);
/* Insert or replace; -1 on allocation failure. */
int fipsd_gw_map_set(fipsd_gw_map *map, const void *key, uint32_t value);
bool fipsd_gw_map_remove(fipsd_gw_map *map, const void *key);
static inline size_t fipsd_gw_map_len(const fipsd_gw_map *map) { return map->len; }
/* Iterate: start with *cursor == 0; each call yields the next live entry. */
bool fipsd_gw_map_next(const fipsd_gw_map *map, size_t *cursor, const uint8_t **key, uint32_t **value);
/* Remove the entry the last fipsd_gw_map_next yielded (cursor - 1). */
void fipsd_gw_map_remove_at(fipsd_gw_map *map, size_t cursor);
/* Copy src into dst (dst is cleared first); -1 on allocation failure. */
int fipsd_gw_map_copy(fipsd_gw_map *dst, const fipsd_gw_map *src);
#endif
