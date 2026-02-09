#include "hash_map.h"
#include <nsync.h>
#include <stdlib.h>
#include <string.h>

static char *hm_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char*)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

typedef enum {
    KEY_TYPE_STRING,
    KEY_TYPE_INT
} KeyType;

typedef struct HashKey {
    KeyType type; // Type of the key (string or int)
    union {
        char *str_key;
        int64_t int_key;
    } key;
} HashKey;

typedef struct HashNode {
    HashKey key;
    void *value;
    struct HashNode *next;
} HashNode;

unsigned long hash_string(const char *key) {
    unsigned long hash = 5381;
    int c;
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash;
}

// Hash function for int64_t
unsigned long hash_int64(int64_t key) {
    return (unsigned long)(key ^ (key >> 33));
}

// General hash function for HashKey
unsigned long hash_key(const HashKey *key) {
    if (key->type == KEY_TYPE_STRING) {
        return hash_string(key->key.str_key);
    } else if (key->type == KEY_TYPE_INT) {
        return hash_int64(key->key.int_key);
    }
    return 0; // Default case (shouldn't happen)
}

unsigned long hash_function(const char *key) {
    unsigned long hash = 5381;
    int c;
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash;
}

HashKey make_key_from_string(const char *str) {
    HashKey key;
    key.type = KEY_TYPE_STRING;
    key.key.str_key = hm_strdup(str); // Duplicate the string
    return key;
}

HashKey make_key_from_int(int64_t num) {
    HashKey key;
    key.type = KEY_TYPE_INT;
    key.key.int_key = num;
    return key;
}

void free_hash_key(HashKey *key) {
    if (key->type == KEY_TYPE_STRING && key->key.str_key) {
        free(key->key.str_key); // Free the duplicated string
    }
}

GoHashMap *go_hash_map_create(size_t num_buckets) {
    GoHashMap *map = malloc(sizeof(GoHashMap));
    map->num_buckets = num_buckets;
    map->buckets = calloc(num_buckets, sizeof(HashNode *));
    map->bucket_locks = malloc(num_buckets * sizeof(nsync_mu));

    for (size_t i = 0; i < num_buckets; i++) {
        nsync_mu_init(&map->bucket_locks[i]); // Initialize nsync mutex for each bucket
    }

    return map;
}

void go_hash_map_insert_str(GoHashMap *map, const char *key_str, void *value) {
    if (!key_str)
        return; // Handle NULL keys

    HashKey key = make_key_from_string(key_str); // Create key internally
    go_hash_map_insert(map, &key, value);
}

void go_hash_map_insert_int(GoHashMap *map, int64_t key_int, void *value) {
    HashKey key = make_key_from_int(key_int); // Create key internally
    go_hash_map_insert(map, &key, value);
}

void go_hash_map_insert(GoHashMap *map, HashKey *key, void *value) {
    unsigned long hash = hash_key(key);
    size_t bucket_index = hash % map->num_buckets;

    nsync_mu_lock(&map->bucket_locks[bucket_index]);

    HashNode *node = map->buckets[bucket_index];
    while (node) {
        if (key->type == node->key.type) {
            if ((key->type == KEY_TYPE_STRING && strcmp(key->key.str_key, node->key.key.str_key) == 0) ||
                (key->type == KEY_TYPE_INT && key->key.int_key == node->key.key.int_key)) {
                node->value = value; // Update existing value
                nsync_mu_unlock(&map->bucket_locks[bucket_index]);
                return;
            }
        }
        node = node->next;
    }

    // If key is not found, insert a new node
    HashNode *new_node = malloc(sizeof(HashNode));
    new_node->key = *key; // Copy the key
    new_node->value = value;
    new_node->next = map->buckets[bucket_index];
    map->buckets[bucket_index] = new_node;

    nsync_mu_unlock(&map->bucket_locks[bucket_index]);
}

void *go_hash_map_get_string(GoHashMap *map, const char *key_str) {
    if (!key_str)
        return NULL; // Handle NULL keys

    HashKey key = make_key_from_string(key_str); // Create key internally
    void *value = go_hash_map_get(map, &key);
    free_hash_key(&key); // Free key after retrieval
    return value;
}

void *go_hash_map_get_int(GoHashMap *map, int64_t key_int) {
    HashKey key = make_key_from_int(key_int); // Create key internally
    return go_hash_map_get(map, &key);
}

void *go_hash_map_get(GoHashMap *map, HashKey *key) {
    unsigned long hash = hash_key(key);
    size_t bucket_index = hash % map->num_buckets;

    nsync_mu_lock(&map->bucket_locks[bucket_index]);

    HashNode *node = map->buckets[bucket_index];
    while (node) {
        if (key->type == node->key.type) {
            if ((key->type == KEY_TYPE_STRING && strcmp(key->key.str_key, node->key.key.str_key) == 0) ||
                (key->type == KEY_TYPE_INT && key->key.int_key == node->key.key.int_key)) {
                nsync_mu_unlock(&map->bucket_locks[bucket_index]);
                return node->value; // Return value if found
            }
        }
        node = node->next;
    }

    nsync_mu_unlock(&map->bucket_locks[bucket_index]);
    return NULL; // Key not found
}

void go_hash_map_for_each(GoHashMap *map, bool (*foreach)(HashKey *, void *)) {
    for (size_t i = 0; i < map->num_buckets; i++) {
        nsync_mu_lock(&map->bucket_locks[i]);

        HashNode *node = map->buckets[i];
        while (node) {
            HashNode *next = node->next;

            // Call the provided function with the key and value
            bool cont = foreach (&node->key, node->value);
            if (!cont) {
                nsync_mu_unlock(&map->bucket_locks[i]);
                return;
            }

            node = next;
        }
        nsync_mu_unlock(&map->bucket_locks[i]);
    }
}

void go_hash_map_for_each_with_data(GoHashMap *map, bool (*foreach)(HashKey *, void *, void *), void *user_data) {
    for (size_t i = 0; i < map->num_buckets; i++) {
        nsync_mu_lock(&map->bucket_locks[i]);

        HashNode *node = map->buckets[i];
        while (node) {
            HashNode *next = node->next;

            bool cont = foreach(&node->key, node->value, user_data);
            if (!cont) {
                nsync_mu_unlock(&map->bucket_locks[i]);
                return;
            }

            node = next;
        }
        nsync_mu_unlock(&map->bucket_locks[i]);
    }
}

void go_hash_map_remove_str(GoHashMap *map, const char *key_str) {
    if (!key_str)
        return;

    HashKey key = make_key_from_string(key_str);
    go_hash_map_remove(map, &key);
    free_hash_key(&key);
}

void go_hash_map_remove_int(GoHashMap *map, int64_t key) {
    HashKey key_int = make_key_from_int(key);
    go_hash_map_remove(map, &key_int);
}

void go_hash_map_remove(GoHashMap *map, HashKey *key) {
    unsigned long hash = hash_key(key);
    size_t bucket_index = hash % map->num_buckets;

    nsync_mu_lock(&map->bucket_locks[bucket_index]);

    HashNode *node = map->buckets[bucket_index];
    HashNode *prev = NULL;
    while (node) {
        if (key->type == node->key.type) {
            if ((key->type == KEY_TYPE_STRING && strcmp(key->key.str_key, node->key.key.str_key) == 0) ||
                (key->type == KEY_TYPE_INT && key->key.int_key == node->key.key.int_key)) {
                if (prev) {
                    prev->next = node->next;
                } else {
                    map->buckets[bucket_index] = node->next;
                }
                if (node->key.type == KEY_TYPE_STRING && node->key.key.str_key) {
                    free(node->key.key.str_key);
                }
                free(node);
                break;
            }
        }
        prev = node;
        node = node->next;
    }

    nsync_mu_unlock(&map->bucket_locks[bucket_index]);
}

void go_hash_map_destroy(GoHashMap *map) {
    if (!map)
        return;

    for (size_t i = 0; i < map->num_buckets; i++) {
        nsync_mu_lock(&map->bucket_locks[i]);

        HashNode *node = map->buckets[i];
        while (node) {
            HashNode *next = node->next;
            if (node->key.type == KEY_TYPE_STRING) {
                free(node->key.key.str_key);
            }
            free(node);
            node = next;
        }
        nsync_mu_unlock(&map->bucket_locks[i]);
    }
    free(map->buckets);
    free(map->bucket_locks);
    free(map);
}
