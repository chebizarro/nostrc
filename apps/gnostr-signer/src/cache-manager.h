/* cache-manager.h - LRU cache with eviction policies
 *
 * Provides a generic LRU (Least Recently Used) cache implementation
 * with configurable size limits and TTL (Time To Live) support.
 *
 * Features:
 * - LRU eviction when cache is full
 * - TTL-based expiration
 * - Memory-aware eviction (bytes limit)
 * - Statistics tracking
 * - Thread-safe operations
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GNOSTR_CACHE_MANAGER_H
#define GNOSTR_CACHE_MANAGER_H

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

/* Cache eviction policies */
typedef enum {
  GN_CACHE_EVICT_LRU,      /* Least Recently Used (default) */
  GN_CACHE_EVICT_LFU,      /* Least Frequently Used */
  GN_CACHE_EVICT_FIFO,     /* First In First Out */
  GN_CACHE_EVICT_TTL       /* Time-based only */
} GnCacheEvictPolicy;

/* Cache statistics */
typedef struct {
  guint entries;           /* Current number of entries */
  gsize bytes;             /* Current bytes used */
  guint max_entries;       /* Maximum entries allowed */
  gsize max_bytes;         /* Maximum bytes allowed */
  guint hits;              /* Cache hits */
  guint misses;            /* Cache misses */
  guint evictions;         /* Evictions due to capacity */
  guint expirations;       /* Expirations due to TTL */
  gint64 created_at;       /* Cache creation time */
  gint64 last_access;      /* Last access time */
} GnCacheStats;

/* Opaque cache type */
typedef struct _GnCache GnCache;

/* Value free function type */
typedef void (*GnCacheValueFree)(gpointer value);

/* Value size function type (for memory-aware caching) */
typedef gsize (*GnCacheValueSize)(gpointer value);

/**
 * gn_cache_new:
 * @name: Cache name for debugging
 * @max_entries: Maximum number of entries (0 = unlimited)
 * @max_bytes: Maximum bytes to store (0 = unlimited)
 * @default_ttl_sec: Default TTL in seconds (0 = no expiration)
 * @value_free: Function to free values
 *
 * Creates a new cache instance.
 *
 * Returns: (transfer full): A new #GnCache
 */
GnCache *gn_cache_new(const gchar *name,
                      guint max_entries,
                      gsize max_bytes,
                      guint default_ttl_sec,
                      GnCacheValueFree value_free);

/**
 * gn_cache_free:
 * @cache: A #GnCache
 *
 * Frees the cache and all its entries.
 */
void gn_cache_free(GnCache *cache);

/**
 * gn_cache_set_evict_policy:
 * @cache: A #GnCache
 * @policy: The eviction policy to use
 *
 * Sets the cache eviction policy.
 */
void gn_cache_set_evict_policy(GnCache *cache, GnCacheEvictPolicy policy);

/**
 * gn_cache_set_value_size_func:
 * @cache: A #GnCache
 * @size_func: Function to calculate value size
 *
 * Sets a function to calculate the size of cached values.
 * Required for memory-aware (max_bytes) caching.
 */
void gn_cache_set_value_size_func(GnCache *cache, GnCacheValueSize size_func);

/**
 * gn_cache_put:
 * @cache: A #GnCache
 * @key: The key to store
 * @value: The value to store
 *
 * Stores a value in the cache with the default TTL.
 */
void gn_cache_put(GnCache *cache, const gchar *key, gpointer value);

/**
 * gn_cache_put_with_ttl:
 * @cache: A #GnCache
 * @key: The key to store
 * @value: The value to store
 * @ttl_sec: Time to live in seconds
 *
 * Stores a value with a specific TTL.
 */
void gn_cache_put_with_ttl(GnCache *cache,
                           const gchar *key,
                           gpointer value,
                           guint ttl_sec);

/**
 * gn_cache_get:
 * @cache: A #GnCache
 * @key: The key to look up
 *
 * Gets a value from the cache. Updates access time for LRU.
 *
 * Returns: (transfer none) (nullable): The cached value or %NULL
 */
gpointer gn_cache_get(GnCache *cache, const gchar *key);

/**
 * gn_cache_contains:
 * @cache: A #GnCache
 * @key: The key to check
 *
 * Checks if a key exists in the cache.
 *
 * Returns: %TRUE if the key exists
 */
gboolean gn_cache_contains(GnCache *cache, const gchar *key);

/**
 * gn_cache_remove:
 * @cache: A #GnCache
 * @key: The key to remove
 *
 * Removes an entry from the cache.
 *
 * Returns: %TRUE if the entry was removed
 */
gboolean gn_cache_remove(GnCache *cache, const gchar *key);

/**
 * gn_cache_clear:
 * @cache: A #GnCache
 *
 * Removes all entries from the cache.
 */
void gn_cache_clear(GnCache *cache);

/**
 * gn_cache_expire:
 * @cache: A #GnCache
 *
 * Removes all expired entries from the cache.
 *
 * Returns: Number of entries expired
 */
guint gn_cache_expire(GnCache *cache);

/**
 * gn_cache_evict:
 * @cache: A #GnCache
 * @count: Number of entries to evict
 *
 * Forces eviction of entries based on the cache policy.
 *
 * Returns: Number of entries evicted
 */
guint gn_cache_evict(GnCache *cache, guint count);

/**
 * gn_cache_get_stats:
 * @cache: A #GnCache
 *
 * Gets cache statistics.
 *
 * Returns: Cache statistics
 */
GnCacheStats gn_cache_get_stats(GnCache *cache);

/**
 * gn_cache_get_keys:
 * @cache: A #GnCache
 *
 * Gets all keys in the cache.
 *
 * Returns: (transfer full): Array of keys (free with g_strfreev)
 */
gchar **gn_cache_get_keys(GnCache *cache);

/**
 * gn_cache_foreach:
 * @cache: A #GnCache
 * @func: Function to call for each entry
 * @user_data: User data for the function
 *
 * Calls a function for each cache entry.
 */
void gn_cache_foreach(GnCache *cache,
                      void (*func)(const gchar *key, gpointer value, gpointer user_data),
                      gpointer user_data);

/**
 * gn_cache_resize:
 * @cache: A #GnCache
 * @max_entries: New maximum entries (0 = unlimited)
 * @max_bytes: New maximum bytes (0 = unlimited)
 *
 * Resizes the cache, evicting entries if necessary.
 */
void gn_cache_resize(GnCache *cache, guint max_entries, gsize max_bytes);

G_END_DECLS

#endif /* GNOSTR_CACHE_MANAGER_H */
