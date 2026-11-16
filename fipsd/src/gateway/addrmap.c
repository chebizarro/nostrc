#include "gateway/addrmap.h"
#include <stdlib.h>
#include <string.h>

static uint64_t hash_key(const uint8_t *key, size_t len)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < len; ++i) { h ^= key[i]; h *= UINT64_C(1099511628211); }
    return h ^ (h >> 29);
}
void fipsd_gw_map_init(fipsd_gw_map *map, size_t key_len)
{
    memset(map, 0, sizeof *map);
    map->key_len = key_len > FIPSD_GW_MAP_KEY_MAX ? FIPSD_GW_MAP_KEY_MAX : key_len;
}
void fipsd_gw_map_clear(fipsd_gw_map *map)
{
    free(map->keys); free(map->values); free(map->slots);
    size_t key_len = map->key_len;
    memset(map, 0, sizeof *map);
    map->key_len = key_len;
}
static size_t probe(const fipsd_gw_map *map, const void *key, bool *found)
{
    size_t mask = map->cap - 1, i = (size_t)hash_key(key, map->key_len) & mask, tomb = SIZE_MAX;
    for (size_t n = 0; n < map->cap; ++n, i = (i + 1) & mask) {
        if (map->slots[i] == 0) { *found = false; return tomb != SIZE_MAX ? tomb : i; }
        if (map->slots[i] == 2) { if (tomb == SIZE_MAX) tomb = i; continue; }
        if (!memcmp(map->keys + i * map->key_len, key, map->key_len)) { *found = true; return i; }
    }
    *found = false;
    return tomb;
}
static int grow(fipsd_gw_map *map, size_t cap)
{
    fipsd_gw_map next; fipsd_gw_map_init(&next, map->key_len);
    next.cap = cap;
    next.keys = calloc(cap, map->key_len); next.values = calloc(cap, sizeof *next.values); next.slots = calloc(cap, 1);
    if (!next.keys || !next.values || !next.slots) { fipsd_gw_map_clear(&next); return -1; }
    for (size_t i = 0; i < map->cap; ++i) {
        if (map->slots[i] != 1) continue;
        bool found; size_t at = probe(&next, map->keys + i * map->key_len, &found);
        memcpy(next.keys + at * next.key_len, map->keys + i * map->key_len, map->key_len);
        next.values[at] = map->values[i]; next.slots[at] = 1; next.len++; next.occupied++;
    }
    fipsd_gw_map_clear(map);
    *map = next;
    return 0;
}
uint32_t *fipsd_gw_map_find(const fipsd_gw_map *map, const void *key)
{
    if (!map->cap) return NULL;
    bool found; size_t at = probe(map, key, &found);
    return found ? &map->values[at] : NULL;
}
bool fipsd_gw_map_get(const fipsd_gw_map *map, const void *key, uint32_t *value)
{
    const uint32_t *v = fipsd_gw_map_find(map, key);
    if (v && value) *value = *v;
    return v != NULL;
}
bool fipsd_gw_map_contains(const fipsd_gw_map *map, const void *key) { return fipsd_gw_map_find(map, key) != NULL; }
int fipsd_gw_map_reserve(fipsd_gw_map *map)
{
    if (map->cap && (map->occupied + 1) * 10 <= map->cap * 7) return 0;
    size_t cap = map->cap ? map->cap : 16;
    while ((map->len + 1) * 10 > cap * 7 / 2) cap *= 2; /* rehash drops tombstones */
    return grow(map, cap);
}
int fipsd_gw_map_set(fipsd_gw_map *map, const void *key, uint32_t value)
{
    if (fipsd_gw_map_reserve(map) != 0) return -1;
    bool found; size_t at = probe(map, key, &found);
    if (!found) {
        if (map->slots[at] == 0) map->occupied++;
        memcpy(map->keys + at * map->key_len, key, map->key_len);
        map->slots[at] = 1; map->len++;
    }
    map->values[at] = value;
    return 0;
}
bool fipsd_gw_map_remove(fipsd_gw_map *map, const void *key)
{
    if (!map->cap) return false;
    bool found; size_t at = probe(map, key, &found);
    if (!found) return false;
    map->slots[at] = 2; map->len--;
    return true;
}
bool fipsd_gw_map_next(const fipsd_gw_map *map, size_t *cursor, const uint8_t **key, uint32_t **value)
{
    for (size_t i = *cursor; i < map->cap; ++i) {
        if (map->slots[i] != 1) continue;
        *cursor = i + 1;
        if (key) *key = map->keys + i * map->key_len;
        if (value) *value = &map->values[i];
        return true;
    }
    *cursor = map->cap;
    return false;
}
void fipsd_gw_map_remove_at(fipsd_gw_map *map, size_t cursor)
{
    if (!cursor || cursor > map->cap || map->slots[cursor - 1] != 1) return;
    map->slots[cursor - 1] = 2; map->len--;
}
int fipsd_gw_map_copy(fipsd_gw_map *dst, const fipsd_gw_map *src)
{
    fipsd_gw_map_clear(dst);
    dst->key_len = src->key_len;
    size_t cursor = 0; const uint8_t *key; uint32_t *value;
    while (fipsd_gw_map_next(src, &cursor, &key, &value))
        if (fipsd_gw_map_set(dst, key, *value) != 0) return -1;
    return 0;
}
