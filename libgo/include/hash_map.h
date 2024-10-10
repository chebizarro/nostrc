#ifndef GO_HASH_MAP_H
#define GO_HASH_MAP_H

#include <nsync.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct HashNode {
    char *key;
    void *value;
    struct HashNode *next;
} HashNode;

typedef struct {
    HashNode **buckets;
    nsync_mu *bucket_locks; // Lock for each bucket
    size_t num_buckets;
} ConcurrentHashMap;

ConcurrentHashMap *concurrent_hash_map_create(size_t num_buckets);

void concurrent_hash_map_insert(ConcurrentHashMap *map, const char *key, void *value);

void *concurrent_hash_map_get(ConcurrentHashMap *map, const char *key);

void concurrent_hash_map_for_each(ConcurrentHashMap *map, bool (*foreach)(const char *, void *));

void concurrent_hash_map_remove(ConcurrentHashMap *map, const char *key);

void concurrent_hash_map_destroy(ConcurrentHashMap *map);

#endif // GO_HASH_MAP_H