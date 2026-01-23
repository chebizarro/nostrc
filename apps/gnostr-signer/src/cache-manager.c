/* cache-manager.c - LRU cache implementation
 *
 * Implements a thread-safe LRU cache with TTL support
 * and memory-aware eviction.
 *
 * SPDX-License-Identifier: MIT
 */
#include "cache-manager.h"
#include "memory-profile.h"
#include <string.h>

/* Cache entry */
typedef struct _GnCacheEntry {
  gchar *key;
  gpointer value;
  gsize value_size;
  gint64 created_at;
  gint64 last_access;
  gint64 expires_at;      /* 0 = no expiration */
  guint access_count;     /* For LFU policy */
  struct _GnCacheEntry *prev;
  struct _GnCacheEntry *next;
} GnCacheEntry;

/* Cache structure */
struct _GnCache {
  gchar *name;
  GMutex lock;

  /* Hash table for O(1) lookup */
  GHashTable *entries;    /* key -> GnCacheEntry* */

  /* Doubly linked list for LRU ordering */
  GnCacheEntry *head;     /* Most recently used */
  GnCacheEntry *tail;     /* Least recently used */

  /* Configuration */
  guint max_entries;
  gsize max_bytes;
  guint default_ttl_sec;
  GnCacheEvictPolicy policy;

  /* Callbacks */
  GnCacheValueFree value_free;
  GnCacheValueSize value_size_func;

  /* Statistics */
  GnCacheStats stats;
};

/* Forward declarations */
static void entry_free(GnCacheEntry *entry, GnCache *cache);
static void move_to_front(GnCache *cache, GnCacheEntry *entry);
static void remove_entry(GnCache *cache, GnCacheEntry *entry);
static void evict_if_needed(GnCache *cache);
static gboolean is_expired(GnCacheEntry *entry);

GnCache *
gn_cache_new(const gchar *name,
             guint max_entries,
             gsize max_bytes,
             guint default_ttl_sec,
             GnCacheValueFree value_free)
{
  GnCache *cache = g_new0(GnCache, 1);

  g_mutex_init(&cache->lock);
  cache->name = g_strdup(name ? name : "unnamed");
  cache->entries = g_hash_table_new(g_str_hash, g_str_equal);
  cache->head = NULL;
  cache->tail = NULL;
  cache->max_entries = max_entries;
  cache->max_bytes = max_bytes;
  cache->default_ttl_sec = default_ttl_sec;
  cache->policy = GN_CACHE_EVICT_LRU;
  cache->value_free = value_free;
  cache->value_size_func = NULL;

  memset(&cache->stats, 0, sizeof(cache->stats));
  cache->stats.max_entries = max_entries;
  cache->stats.max_bytes = max_bytes;
  cache->stats.created_at = g_get_monotonic_time();

  g_debug("cache: Created cache '%s' (max_entries=%u, max_bytes=%zu, ttl=%us)",
          cache->name, max_entries, max_bytes, default_ttl_sec);

  return cache;
}

void
gn_cache_free(GnCache *cache)
{
  if (!cache) return;

  g_mutex_lock(&cache->lock);

  /* Free all entries */
  GnCacheEntry *entry = cache->head;
  while (entry) {
    GnCacheEntry *next = entry->next;
    entry_free(entry, cache);
    entry = next;
  }

  g_hash_table_destroy(cache->entries);
  g_free(cache->name);

  g_mutex_unlock(&cache->lock);
  g_mutex_clear(&cache->lock);

  g_free(cache);
}

static void
entry_free(GnCacheEntry *entry, GnCache *cache)
{
  if (!entry) return;

  /* Track memory reduction */
  GN_MEM_FREE(GN_MEM_COMPONENT_CACHE, entry->value_size);
  gn_mem_profile_cache_remove(entry->value_size);

  if (cache->value_free && entry->value) {
    cache->value_free(entry->value);
  }

  g_free(entry->key);
  g_free(entry);
}

static gboolean
is_expired(GnCacheEntry *entry)
{
  if (entry->expires_at == 0) {
    return FALSE;
  }
  return g_get_monotonic_time() >= entry->expires_at;
}

static void
move_to_front(GnCache *cache, GnCacheEntry *entry)
{
  if (entry == cache->head) {
    return;  /* Already at front */
  }

  /* Remove from current position */
  if (entry->prev) {
    entry->prev->next = entry->next;
  }
  if (entry->next) {
    entry->next->prev = entry->prev;
  }
  if (entry == cache->tail) {
    cache->tail = entry->prev;
  }

  /* Move to front */
  entry->prev = NULL;
  entry->next = cache->head;
  if (cache->head) {
    cache->head->prev = entry;
  }
  cache->head = entry;

  if (!cache->tail) {
    cache->tail = entry;
  }
}

static void
remove_entry(GnCache *cache, GnCacheEntry *entry)
{
  /* Remove from hash table */
  g_hash_table_remove(cache->entries, entry->key);

  /* Update linked list */
  if (entry->prev) {
    entry->prev->next = entry->next;
  } else {
    cache->head = entry->next;
  }

  if (entry->next) {
    entry->next->prev = entry->prev;
  } else {
    cache->tail = entry->prev;
  }

  /* Update stats */
  cache->stats.entries--;
  if (cache->stats.bytes >= entry->value_size) {
    cache->stats.bytes -= entry->value_size;
  }

  entry_free(entry, cache);
}

static void
evict_if_needed(GnCache *cache)
{
  /* First, expire old entries */
  GnCacheEntry *entry = cache->tail;
  while (entry) {
    GnCacheEntry *prev = entry->prev;
    if (is_expired(entry)) {
      remove_entry(cache, entry);
      cache->stats.expirations++;
    }
    entry = prev;
  }

  /* Check entry limit */
  while (cache->max_entries > 0 && cache->stats.entries >= cache->max_entries) {
    if (!cache->tail) break;

    /* Evict based on policy */
    GnCacheEntry *victim = NULL;

    switch (cache->policy) {
      case GN_CACHE_EVICT_LRU:
      case GN_CACHE_EVICT_FIFO:
        /* Remove from tail (oldest/least recently used) */
        victim = cache->tail;
        break;

      case GN_CACHE_EVICT_LFU:
        /* Find least frequently used */
        victim = cache->tail;
        for (GnCacheEntry *e = cache->head; e; e = e->next) {
          if (e->access_count < victim->access_count) {
            victim = e;
          }
        }
        break;

      case GN_CACHE_EVICT_TTL:
        /* Only evict expired entries (already done above) */
        return;
    }

    if (victim) {
      g_debug("cache '%s': Evicting key '%s' (policy=%d)",
              cache->name, victim->key, cache->policy);
      remove_entry(cache, victim);
      cache->stats.evictions++;
    } else {
      break;
    }
  }

  /* Check byte limit */
  while (cache->max_bytes > 0 && cache->stats.bytes > cache->max_bytes) {
    if (!cache->tail) break;

    g_debug("cache '%s': Evicting key '%s' (bytes limit)",
            cache->name, cache->tail->key);
    remove_entry(cache, cache->tail);
    cache->stats.evictions++;
  }
}

void
gn_cache_set_evict_policy(GnCache *cache, GnCacheEvictPolicy policy)
{
  if (!cache) return;
  g_mutex_lock(&cache->lock);
  cache->policy = policy;
  g_mutex_unlock(&cache->lock);
}

void
gn_cache_set_value_size_func(GnCache *cache, GnCacheValueSize size_func)
{
  if (!cache) return;
  g_mutex_lock(&cache->lock);
  cache->value_size_func = size_func;
  g_mutex_unlock(&cache->lock);
}

void
gn_cache_put(GnCache *cache, const gchar *key, gpointer value)
{
  gn_cache_put_with_ttl(cache, key, value, cache ? cache->default_ttl_sec : 0);
}

void
gn_cache_put_with_ttl(GnCache *cache,
                      const gchar *key,
                      gpointer value,
                      guint ttl_sec)
{
  if (!cache || !key) return;

  g_mutex_lock(&cache->lock);

  /* Check if key already exists */
  GnCacheEntry *existing = g_hash_table_lookup(cache->entries, key);
  if (existing) {
    /* Update existing entry */
    if (cache->value_free && existing->value) {
      cache->value_free(existing->value);
    }

    /* Update stats for size change */
    gsize old_size = existing->value_size;
    gsize new_size = cache->value_size_func ? cache->value_size_func(value) : 0;

    existing->value = value;
    existing->value_size = new_size;
    existing->last_access = g_get_monotonic_time();
    existing->access_count++;

    if (ttl_sec > 0) {
      existing->expires_at = g_get_monotonic_time() + (ttl_sec * G_USEC_PER_SEC);
    } else {
      existing->expires_at = 0;
    }

    cache->stats.bytes = cache->stats.bytes - old_size + new_size;

    move_to_front(cache, existing);
    g_mutex_unlock(&cache->lock);
    return;
  }

  /* Evict if needed before adding */
  evict_if_needed(cache);

  /* Create new entry */
  GnCacheEntry *entry = g_new0(GnCacheEntry, 1);
  entry->key = g_strdup(key);
  entry->value = value;
  entry->value_size = cache->value_size_func ? cache->value_size_func(value) : 0;
  entry->created_at = g_get_monotonic_time();
  entry->last_access = entry->created_at;
  entry->access_count = 1;

  if (ttl_sec > 0) {
    entry->expires_at = entry->created_at + (ttl_sec * G_USEC_PER_SEC);
  } else {
    entry->expires_at = 0;
  }

  /* Add to hash table */
  g_hash_table_insert(cache->entries, entry->key, entry);

  /* Add to front of list */
  entry->prev = NULL;
  entry->next = cache->head;
  if (cache->head) {
    cache->head->prev = entry;
  }
  cache->head = entry;
  if (!cache->tail) {
    cache->tail = entry;
  }

  /* Update stats */
  cache->stats.entries++;
  cache->stats.bytes += entry->value_size;
  cache->stats.last_access = entry->last_access;

  /* Track memory */
  GN_MEM_ALLOC(GN_MEM_COMPONENT_CACHE, entry->value_size);
  gn_mem_profile_cache_add(entry->value_size);

  g_mutex_unlock(&cache->lock);
}

gpointer
gn_cache_get(GnCache *cache, const gchar *key)
{
  if (!cache || !key) return NULL;

  g_mutex_lock(&cache->lock);

  GnCacheEntry *entry = g_hash_table_lookup(cache->entries, key);

  if (!entry) {
    cache->stats.misses++;
    GN_CACHE_MISS();
    g_mutex_unlock(&cache->lock);
    return NULL;
  }

  /* Check expiration */
  if (is_expired(entry)) {
    remove_entry(cache, entry);
    cache->stats.misses++;
    cache->stats.expirations++;
    GN_CACHE_MISS();
    g_mutex_unlock(&cache->lock);
    return NULL;
  }

  /* Update access time and count */
  entry->last_access = g_get_monotonic_time();
  entry->access_count++;
  cache->stats.hits++;
  cache->stats.last_access = entry->last_access;

  /* Move to front for LRU */
  if (cache->policy == GN_CACHE_EVICT_LRU) {
    move_to_front(cache, entry);
  }

  GN_CACHE_HIT();
  gpointer value = entry->value;
  g_mutex_unlock(&cache->lock);

  return value;
}

gboolean
gn_cache_contains(GnCache *cache, const gchar *key)
{
  if (!cache || !key) return FALSE;

  g_mutex_lock(&cache->lock);

  GnCacheEntry *entry = g_hash_table_lookup(cache->entries, key);
  gboolean exists = (entry != NULL && !is_expired(entry));

  g_mutex_unlock(&cache->lock);
  return exists;
}

gboolean
gn_cache_remove(GnCache *cache, const gchar *key)
{
  if (!cache || !key) return FALSE;

  g_mutex_lock(&cache->lock);

  GnCacheEntry *entry = g_hash_table_lookup(cache->entries, key);
  if (!entry) {
    g_mutex_unlock(&cache->lock);
    return FALSE;
  }

  remove_entry(cache, entry);

  g_mutex_unlock(&cache->lock);
  return TRUE;
}

void
gn_cache_clear(GnCache *cache)
{
  if (!cache) return;

  g_mutex_lock(&cache->lock);

  GnCacheEntry *entry = cache->head;
  while (entry) {
    GnCacheEntry *next = entry->next;
    entry_free(entry, cache);
    entry = next;
  }

  g_hash_table_remove_all(cache->entries);
  cache->head = NULL;
  cache->tail = NULL;
  cache->stats.entries = 0;
  cache->stats.bytes = 0;

  g_mutex_unlock(&cache->lock);

  g_debug("cache '%s': Cleared all entries", cache->name);
}

guint
gn_cache_expire(GnCache *cache)
{
  if (!cache) return 0;

  g_mutex_lock(&cache->lock);

  guint expired = 0;
  GnCacheEntry *entry = cache->head;

  while (entry) {
    GnCacheEntry *next = entry->next;
    if (is_expired(entry)) {
      remove_entry(cache, entry);
      expired++;
      cache->stats.expirations++;
    }
    entry = next;
  }

  g_mutex_unlock(&cache->lock);

  if (expired > 0) {
    g_debug("cache '%s': Expired %u entries", cache->name, expired);
  }

  return expired;
}

guint
gn_cache_evict(GnCache *cache, guint count)
{
  if (!cache || count == 0) return 0;

  g_mutex_lock(&cache->lock);

  guint evicted = 0;

  while (evicted < count && cache->tail) {
    GnCacheEntry *victim = cache->tail;
    remove_entry(cache, victim);
    evicted++;
    cache->stats.evictions++;
  }

  g_mutex_unlock(&cache->lock);

  g_debug("cache '%s': Force evicted %u entries", cache->name, evicted);
  return evicted;
}

GnCacheStats
gn_cache_get_stats(GnCache *cache)
{
  GnCacheStats stats;
  memset(&stats, 0, sizeof(stats));

  if (!cache) return stats;

  g_mutex_lock(&cache->lock);
  stats = cache->stats;
  g_mutex_unlock(&cache->lock);

  return stats;
}

gchar **
gn_cache_get_keys(GnCache *cache)
{
  if (!cache) return NULL;

  g_mutex_lock(&cache->lock);

  guint count = g_hash_table_size(cache->entries);
  gchar **keys = g_new0(gchar*, count + 1);

  GHashTableIter iter;
  gpointer key;
  guint i = 0;

  g_hash_table_iter_init(&iter, cache->entries);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    keys[i++] = g_strdup((gchar*)key);
  }
  keys[i] = NULL;

  g_mutex_unlock(&cache->lock);
  return keys;
}

void
gn_cache_foreach(GnCache *cache,
                 void (*func)(const gchar *key, gpointer value, gpointer user_data),
                 gpointer user_data)
{
  if (!cache || !func) return;

  g_mutex_lock(&cache->lock);

  for (GnCacheEntry *entry = cache->head; entry; entry = entry->next) {
    func(entry->key, entry->value, user_data);
  }

  g_mutex_unlock(&cache->lock);
}

void
gn_cache_resize(GnCache *cache, guint max_entries, gsize max_bytes)
{
  if (!cache) return;

  g_mutex_lock(&cache->lock);

  cache->max_entries = max_entries;
  cache->max_bytes = max_bytes;
  cache->stats.max_entries = max_entries;
  cache->stats.max_bytes = max_bytes;

  /* Evict if we're now over the new limits */
  evict_if_needed(cache);

  g_mutex_unlock(&cache->lock);

  g_debug("cache '%s': Resized to max_entries=%u, max_bytes=%zu",
          cache->name, max_entries, max_bytes);
}
