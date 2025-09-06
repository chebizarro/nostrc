#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nostr-storage.h"

typedef struct StorageRegNode {
  char *name;
  NostrStorageFactory make;
  struct StorageRegNode *next;
} StorageRegNode;

static StorageRegNode *g_head = NULL;

int nostr_storage_register(const char *name, NostrStorageFactory make) {
  if (!name || !*name || !make) return -1;
  // Check duplicates
  for (StorageRegNode *it = g_head; it; it = it->next) {
    if (strcmp(it->name, name) == 0) {
      // Replace existing
      it->make = make;
      return 0;
    }
  }
  StorageRegNode *n = (StorageRegNode*)calloc(1, sizeof(*n));
  if (!n) return -1;
  n->name = strdup(name);
  if (!n->name) { free(n); return -1; }
  n->make = make;
  n->next = g_head;
  g_head = n;
  return 0;
}

NostrStorage* nostr_storage_create(const char *name) {
  if (!name || !*name) return NULL;
  for (StorageRegNode *it = g_head; it; it = it->next) {
    if (strcmp(it->name, name) == 0) {
      return it->make ? it->make() : NULL;
    }
  }
  return NULL;
}
