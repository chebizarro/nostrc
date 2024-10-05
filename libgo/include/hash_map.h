#ifndef GO_HASH_MAP_H
#define GO_HASH_MAP_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

typedef struct HashNode {
    char *key;
    void *value;
    struct HashNode *next;
} HashNode;

typedef struct {
    HashNode **buckets;
    pthread_rwlock_t *bucket_locks;  // Lock for each bucket
    size_t num_buckets;
} ConcurrentHashMap;


#endif // GO_HASH_MAP_H