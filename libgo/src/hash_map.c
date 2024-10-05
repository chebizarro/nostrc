#include "hash_map.h"
#include <string.h>

unsigned long hash_function(const char *key) {
    unsigned long hash = 5381;
    int c;
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c;  // hash * 33 + c
    return hash;
}

HashNode *hash_node_create(const char *key, void *value) {
    HashNode *node = malloc(sizeof(HashNode));
    node->key = strdup(key);  // Duplicate the key string
    node->value = value;
    node->next = NULL;
    return node;
}

ConcurrentHashMap *concurrent_hash_map_create(size_t num_buckets) {
    ConcurrentHashMap *map = malloc(sizeof(ConcurrentHashMap));
    map->num_buckets = num_buckets;
    map->buckets = calloc(num_buckets, sizeof(HashNode *));
    map->bucket_locks = malloc(num_buckets * sizeof(pthread_rwlock_t));

    for (size_t i = 0; i < num_buckets; i++) {
        pthread_rwlock_init(&map->bucket_locks[i], NULL);
    }

    return map;
}

void concurrent_hash_map_insert(ConcurrentHashMap *map, const char *key, void *value) {
    unsigned long hash = hash_function(key);
    size_t bucket_index = hash % map->num_buckets;

    pthread_rwlock_wrlock(&map->bucket_locks[bucket_index]);

    HashNode *node = map->buckets[bucket_index];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            node->value = value;  // Update existing value
            pthread_rwlock_unlock(&map->bucket_locks[bucket_index]);
            return;
        }
        node = node->next;
    }

    HashNode *new_node = hash_node_create(key, value);
    new_node->next = map->buckets[bucket_index];
    map->buckets[bucket_index] = new_node;

    pthread_rwlock_unlock(&map->bucket_locks[bucket_index]);
}

void *concurrent_hash_map_get(ConcurrentHashMap *map, const char *key) {
    unsigned long hash = hash_function(key);
    size_t bucket_index = hash % map->num_buckets;

    pthread_rwlock_rdlock(&map->bucket_locks[bucket_index]);

    HashNode *node = map->buckets[bucket_index];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            pthread_rwlock_unlock(&map->bucket_locks[bucket_index]);
            return node->value;  // Return value if found
        }
        node = node->next;
    }

    pthread_rwlock_unlock(&map->bucket_locks[bucket_index]);
    return NULL;  // Return NULL if not found
}

void concurrent_hash_map_remove(ConcurrentHashMap *map, const char *key) {
    unsigned long hash = hash_function(key);
    size_t bucket_index = hash % map->num_buckets;

    pthread_rwlock_wrlock(&map->bucket_locks[bucket_index]);

    HashNode *node = map->buckets[bucket_index];
    HashNode *prev = NULL;
    while (node) {
        if (strcmp(node->key, key) == 0) {
            if (prev) {
                prev->next = node->next;
            } else {
                map->buckets[bucket_index] = node->next;
            }
            free(node->key);
            free(node);
            break;
        }
        prev = node;
        node = node->next;
    }

    pthread_rwlock_unlock(&map->bucket_locks[bucket_index]);
}

void concurrent_hash_map_destroy(ConcurrentHashMap *map) {
    for (size_t i = 0; i < map->num_buckets; i++) {
        pthread_rwlock_wrlock(&map->bucket_locks[i]);

        HashNode *node = map->buckets[i];
        while (node) {
            HashNode *next = node->next;
            free(node->key);
            free(node);
            node = next;
        }

        pthread_rwlock_unlock(&map->bucket_locks[i]);
        pthread_rwlock_destroy(&map->bucket_locks[i]);
    }
    
    free(map->buckets);
    free(map->bucket_locks);
    free(map);
}
