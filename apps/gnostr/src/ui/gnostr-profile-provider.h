#ifndef GNOSTR_PROFILE_PROVIDER_H
#define GNOSTR_PROFILE_PROVIDER_H

#include <glib.h>
#include <glib-object.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GnostrProfileProvider: On-demand profile metadata provider
 * 
 * Provides a small LRU cache of profile metadata backed by NostrDB.
 * Replaces the large meta_by_pubkey JSON cache with minimal structs.
 * 
 * Architecture:
 * 1. Check LRU cache (fast, in-memory)
 * 2. On miss, query NostrDB (local, fast)
 * 3. On DB miss, queue for relay fetch (existing system)
 */

/* Minimal profile metadata struct - only fields we render */
typedef struct {
  char *pubkey_hex;      /* owned, 64-char hex */
  char *display_name;    /* owned, nullable */
  char *name;            /* owned, nullable */
  char *picture;         /* owned, nullable */
  char *banner;          /* owned, nullable - banner image URL */
  char *nip05;           /* owned, nullable - NIP-05 identifier */
  char *lud16;           /* owned, nullable - Lightning address */
  gint64 created_at;     /* timestamp for staleness checks */
} GnostrProfileMeta;

/* Initialize the profile provider system.
 * Must be called before any other profile provider functions.
 * 
 * Parameters:
 *   cap: Maximum number of profiles to keep in LRU cache (0 = use env/default)
 */
void gnostr_profile_provider_init(guint cap);

/* Shutdown and free all resources. Safe to call multiple times. */
void gnostr_profile_provider_shutdown(void);

/* Get profile metadata for a pubkey (hex string).
 * 
 * Returns:
 *   - Cached profile (caller must free with gnostr_profile_meta_free)
 *   - NULL if not in cache or DB
 * 
 * On cache miss, this function:
 *   1. Queries NostrDB synchronously (fast, local)
 *   2. If found, caches and returns it
 *   3. If not found, returns NULL (caller should queue for relay fetch)
 * 
 * This function is synchronous and fast (< 1ms typical).
 */
GnostrProfileMeta *gnostr_profile_provider_get(const char *pubkey_hex);

/* Batch get profiles for multiple pubkeys.
 * More efficient than calling get() in a loop.
 * 
 * Parameters:
 *   pubkeys: Array of pubkey hex strings
 *   count: Number of pubkeys
 *   out_metas: Output array of GnostrProfileMeta* (caller must free each + array)
 *   out_count: Number of profiles found
 * 
 * Returns: 0 on success, -1 on error
 */
int gnostr_profile_provider_get_batch(
  const char **pubkeys,
  guint count,
  GnostrProfileMeta ***out_metas,
  guint *out_count
);

/* Update/insert a profile in the cache from JSON.
 * Called when a new profile event arrives from relays or DB.
 * 
 * Parameters:
 *   pubkey_hex: 64-char hex pubkey
 *   profile_json: kind:0 profile JSON content
 * 
 * Returns: 0 on success, -1 on error
 */
int gnostr_profile_provider_update(const char *pubkey_hex, const char *profile_json);

/* Free a profile metadata struct */
void gnostr_profile_meta_free(GnostrProfileMeta *meta);

/* Get cache statistics for monitoring/logging */
typedef struct {
  guint cache_size;      /* current LRU size */
  guint cache_cap;       /* max LRU capacity */
  guint64 hits;          /* cache hits */
  guint64 misses;        /* cache misses */
  guint64 db_hits;       /* DB hits (after cache miss) */
  guint64 db_misses;     /* DB misses (need relay fetch) */
} GnostrProfileProviderStats;

void gnostr_profile_provider_get_stats(GnostrProfileProviderStats *stats);
void gnostr_profile_provider_log_stats(void);

/* Profile update watcher: get notified when a specific pubkey's profile is updated.
 * Callback is dispatched on the GLib main thread via g_idle_add.
 *
 * Parameters:
 *   pubkey_hex: 64-char hex pubkey to watch
 *   callback: called with (pubkey_hex, meta, user_data) when profile updates
 *   user_data: passed to callback
 *
 * Returns: watch ID (> 0) for use with gnostr_profile_provider_unwatch(), 0 on error
 */
typedef void (*GnostrProfileWatchCallback)(const char *pubkey_hex,
                                           const GnostrProfileMeta *meta,
                                           gpointer user_data);

guint gnostr_profile_provider_watch(const char *pubkey_hex,
                                    GnostrProfileWatchCallback callback,
                                    gpointer user_data);

void gnostr_profile_provider_unwatch(guint watch_id);

/* hq-yrqwk: Pre-warm the LRU cache from NDB for a user and their follow list.
 * Runs asynchronously in a GTask worker thread.
 * Call after login when user_pubkey_hex is known. Also callable from
 * sync bridge when kind:0 profiles are synced to refresh stale cache. */
void gnostr_profile_provider_prewarm_async(const char *user_pubkey_hex);

#ifdef __cplusplus
}
#endif

#endif /* GNOSTR_PROFILE_PROVIDER_H */
