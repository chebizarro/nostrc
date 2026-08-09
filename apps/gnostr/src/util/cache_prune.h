#ifndef APPS_GNOSTR_UTIL_CACHE_PRUNE_H
#define APPS_GNOSTR_UTIL_CACHE_PRUNE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * Cache pruning module for gnostr.
 *
 * Provides automatic cleanup of:
 * - Legacy avatar cache (downloaded images in ~/.cache/gnostr/avatars)
 * - Nostrdb event storage (LMDB files in ~/.cache/gnostr/ndb)
 *
 * Account-namespaced media directories under ~/.cache/gnostr/media are owned
 * and bounded by GnostrMediaService; this module intentionally never scans
 * or removes them.
 *
 * Cache limits are configurable via GSettings (org.gnostr.Client):
 * - avatar-cache-max-mb: Max size for avatar caches (default 8 MB)
 * - ndb-cache-max-mb: Max size for nostrdb (default 1024 MB)
 * - cache-prune-on-startup: Enable/disable auto-prune (default true)
 */

/**
 * Initialize cache pruning system.
 * Call once at app startup before main loop.
 * Reads settings from GSettings.
 */
void gnostr_cache_prune_init(void);

/**
 * Prune the legacy avatar cache to stay under size limit.
 * Deletes oldest avatar files first until cache is within limit.
 *
 * @param max_size_mb Maximum cache size in megabytes. 0 = no limit.
 * @return Number of files deleted, or -1 on error.
 */
int gnostr_cache_prune_images(int max_size_mb);

/**
 * Get current legacy avatar cache size in bytes.
 * Scans ~/.cache/gnostr/avatars only.
 *
 * @param file_count Output for number of cached files (may be NULL).
 * @return Total size in bytes, or -1 on error.
 */
gint64 gnostr_cache_get_image_size(int *file_count);

/**
 * Get current nostrdb size in bytes.
 * Reads LMDB data.mdb file size.
 *
 * @return Size in bytes, or -1 on error.
 */
gint64 gnostr_cache_get_ndb_size(void);

/**
 * Clear all cached images.
 * Use with caution - deletes all avatars.
 *
 * @return Number of files deleted, or -1 on error.
 */
int gnostr_cache_clear_images(void);

/**
 * Get cache statistics as human-readable string.
 * Caller must g_free() the result.
 *
 * @return String like "Images: 45.2 MB (523 files), NDB: 128.5 MB"
 */
char *gnostr_cache_stats_string(void);

G_END_DECLS
#endif /* APPS_GNOSTR_UTIL_CACHE_PRUNE_H */
