#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "nostr-storage.h"

typedef struct StorageRegNode {
  char *name;
  NostrStorageFactory make;
  struct StorageRegNode *next;
} StorageRegNode;

static StorageRegNode *g_head = NULL;
/* Guards g_head: register mutates/traverses the list while create traverses it.
 * Without this, a concurrent register + create is a data race on g_head. */
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;

int nostr_storage_register(const char *name, NostrStorageFactory make) {
  if (!name || !*name || !make) return -1;
  pthread_mutex_lock(&g_reg_mu);
  // Check duplicates
  for (StorageRegNode *it = g_head; it; it = it->next) {
    if (strcmp(it->name, name) == 0) {
      // Replace existing
      it->make = make;
      pthread_mutex_unlock(&g_reg_mu);
      return 0;
    }
  }
  StorageRegNode *n = (StorageRegNode*)calloc(1, sizeof(*n));
  if (!n) { pthread_mutex_unlock(&g_reg_mu); return -1; }
  n->name = strdup(name);
  if (!n->name) { free(n); pthread_mutex_unlock(&g_reg_mu); return -1; }
  n->make = make;
  n->next = g_head;
  g_head = n;
  pthread_mutex_unlock(&g_reg_mu);
  return 0;
}

NostrStorage* nostr_storage_create(const char *name) {
  if (!name || !*name) return NULL;
  NostrStorageFactory make = NULL;
  pthread_mutex_lock(&g_reg_mu);
  for (StorageRegNode *it = g_head; it; it = it->next) {
    if (strcmp(it->name, name) == 0) {
      make = it->make;
      break;
    }
  }
  pthread_mutex_unlock(&g_reg_mu);
  /* Invoke the factory outside the lock: it may run arbitrary code (and could
   * even re-enter the registry), so we must not hold g_reg_mu across it. */
  return make ? make() : NULL;
}
