#ifndef GO_HASH_MAP_H
#define GO_HASH_MAP_H

#include <nsync.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct HashNode HashNode;
typedef struct HashKey HashKey;

typedef struct {
    HashNode **buckets;
    nsync_mu *bucket_locks; // Lock for each bucket
    size_t num_buckets;
} GoHashMap;

GoHashMap *go_hash_map_create(size_t num_buckets);

void go_hash_map_insert_str(GoHashMap *map, const char *key_str, void *value);

void go_hash_map_insert_int(GoHashMap *map, int64_t key_int, void *value);

void go_hash_map_insert(GoHashMap *map, HashKey *key, void *value);

void *go_hash_map_get_string(GoHashMap *map, const char *key_str);

void *go_hash_map_get_int(GoHashMap *map, int64_t key_int);

void *go_hash_map_get(GoHashMap *map, HashKey *key);

void go_hash_map_for_each(GoHashMap *map, bool (*foreach)(HashKey *, void *));

void go_hash_map_for_each_with_data(GoHashMap *map, bool (*foreach)(HashKey *, void *, void *), void *user_data);

void go_hash_map_remove_str(GoHashMap *map, const char *key_str);

void go_hash_map_remove_int(GoHashMap *map, int64_t key);

void go_hash_map_remove(GoHashMap *map, HashKey *key);

void go_hash_map_destroy(GoHashMap *map);

#endif // GO_HASH_MAP_H
