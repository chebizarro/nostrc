#ifndef GO_HASH_MAP_H
#define GO_HASH_MAP_H

#include <nsync.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct HashNode HashNode;
typedef struct HashKey HashKey;

typedef struct {
    HashNode **buckets;
    nsync_mu *bucket_locks; // Lock for each bucket
    size_t num_buckets;
} ConcurrentHashMap;

ConcurrentHashMap *concurrent_hash_map_create(size_t num_buckets);

void concurrent_hash_map_insert_string(ConcurrentHashMap *map, const char *key_str, void *value);

void concurrent_hash_map_insert_int(ConcurrentHashMap *map, int64_t key_int, void *value);

void concurrent_hash_map_insert(ConcurrentHashMap *map, HashKey *key, void *value);

void *concurrent_hash_map_get_string(ConcurrentHashMap *map, const char *key_str);

void *concurrent_hash_map_get_int(ConcurrentHashMap *map, int64_t key_int);

void *concurrent_hash_map_get(ConcurrentHashMap *map, HashKey *key);

void concurrent_hash_map_for_each(ConcurrentHashMap *map, bool (*foreach)(HashKey *, void *));

void concurrent_hash_map_remove(ConcurrentHashMap *map, HashKey *key);

void concurrent_hash_map_destroy(ConcurrentHashMap *map);

#endif // GO_HASH_MAP_H