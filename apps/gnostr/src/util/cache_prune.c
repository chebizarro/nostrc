/**
 * Cache pruning implementation for gnostr.
 *
 * Handles automatic cleanup of image cache and nostrdb storage
 * to prevent unbounded disk usage.
 */

#include "cache_prune.h"
#include "gnostr_paths.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* File entry for LRU sorting */
typedef struct {
  char *path;
  gint64 size;
  gint64 mtime; /* modification time for LRU ordering */
} CacheFileEntry;

static int compare_by_mtime_asc(const void *a, const void *b)
{
  const CacheFileEntry *ea = *(const CacheFileEntry **)a;
  const CacheFileEntry *eb = *(const CacheFileEntry **)b;
  /* Sort oldest first (ascending mtime) */
  if (ea->mtime < eb->mtime) return -1;
  if (ea->mtime > eb->mtime) return 1;
  return 0;
}

static void cache_file_entry_free(CacheFileEntry *e)
{
  if (!e) return;
  g_free(e->path);
  g_free(e);
}

/**
 * Get image cache directory path.
 * Returns ~/.cache/gnostr/avatars
 */
static char *get_image_cache_dir(void)
{
  const char *cache_base = g_get_user_cache_dir();
  if (!cache_base || !*cache_base) cache_base = ".";
  return g_build_filename(cache_base, "gnostr", "avatars", NULL);
}

/**
 * Scan directory and collect all files with their sizes and mtimes.
 * Returns GPtrArray of CacheFileEntry*, or NULL on error.
 * Caller must free with g_ptr_array_unref().
 */
static GPtrArray *scan_cache_directory(const char *dir_path, gint64 *total_size_out)
{
  if (!dir_path) return NULL;

  GDir *dir = g_dir_open(dir_path, 0, NULL);
  if (!dir) {
    g_debug("cache_prune: cannot open directory %s", dir_path);
    return NULL;
  }

  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)cache_file_entry_free);
  gint64 total_size = 0;
  const char *name;

  while ((name = g_dir_read_name(dir)) != NULL) {
    char *full_path = g_build_filename(dir_path, name, NULL);
    GStatBuf st;

    if (g_stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
      CacheFileEntry *entry = g_new0(CacheFileEntry, 1);
      entry->path = full_path;
      entry->size = st.st_size;
      entry->mtime = st.st_mtime;
      total_size += entry->size;
      g_ptr_array_add(entries, entry);
    } else {
      g_free(full_path);
    }
  }

  g_dir_close(dir);

  if (total_size_out) *total_size_out = total_size;
  return entries;
}

gint64 gnostr_cache_get_image_size(int *file_count)
{
  g_autofree char *cache_dir = get_image_cache_dir();
  gint64 total_size = 0;

  GPtrArray *entries = scan_cache_directory(cache_dir, &total_size);
  if (!entries) {
    if (file_count) *file_count = 0;
    return 0; /* Directory doesn't exist yet = 0 bytes */
  }

  if (file_count) *file_count = (int)entries->len;
  g_ptr_array_unref(entries);

  return total_size;
}

gint64 gnostr_cache_get_ndb_size(void)
{
  g_autofree char *db_dir = gnostr_get_db_dir();
  if (!db_dir) return -1;

  /* nostrdb uses LMDB, which stores data in data.mdb */
  g_autofree char *data_file = g_build_filename(db_dir, "data.mdb", NULL);
  GStatBuf st;

  if (g_stat(data_file, &st) != 0) {
    /* Try alternate location without subdirectory */
    return 0; /* No database yet */
  }

  return st.st_size;
}

int gnostr_cache_prune_images(int max_size_mb)
{
  if (max_size_mb <= 0) {
    g_debug("cache_prune: image pruning disabled (max_size_mb=%d)", max_size_mb);
    return 0;
  }

  g_autofree char *cache_dir = get_image_cache_dir();
  gint64 total_size = 0;

  GPtrArray *entries = scan_cache_directory(cache_dir, &total_size);
  if (!entries || entries->len == 0) {
    if (entries) g_ptr_array_unref(entries);
    return 0;
  }

  gint64 max_size_bytes = (gint64)max_size_mb * 1024 * 1024;

  if (total_size <= max_size_bytes) {
    g_message("cache_prune: image cache (%.2f MB) is under limit (%d MB), no pruning needed",
              total_size / (1024.0 * 1024.0), max_size_mb);
    g_ptr_array_unref(entries);
    return 0;
  }

  /* Sort by mtime ascending (oldest first) */
  g_ptr_array_sort(entries, compare_by_mtime_asc);

  int deleted_count = 0;
  gint64 freed_bytes = 0;

  for (guint i = 0; i < entries->len && total_size > max_size_bytes; i++) {
    CacheFileEntry *entry = g_ptr_array_index(entries, i);

    if (g_unlink(entry->path) == 0) {
      total_size -= entry->size;
      freed_bytes += entry->size;
      deleted_count++;
      g_debug("cache_prune: deleted %s (%.1f KB)", entry->path, entry->size / 1024.0);
    } else {
      g_warning("cache_prune: failed to delete %s: %s", entry->path, g_strerror(errno));
    }
  }

  g_message("cache_prune: deleted %d image files, freed %.2f MB",
            deleted_count, freed_bytes / (1024.0 * 1024.0));

  g_ptr_array_unref(entries);
  return deleted_count;
}

int gnostr_cache_clear_images(void)
{
  g_autofree char *cache_dir = get_image_cache_dir();
  gint64 total_size = 0;

  GPtrArray *entries = scan_cache_directory(cache_dir, &total_size);
  if (!entries || entries->len == 0) {
    if (entries) g_ptr_array_unref(entries);
    return 0;
  }

  int deleted_count = 0;

  for (guint i = 0; i < entries->len; i++) {
    CacheFileEntry *entry = g_ptr_array_index(entries, i);
    if (g_unlink(entry->path) == 0) {
      deleted_count++;
    }
  }

  g_message("cache_prune: cleared all %d image cache files", deleted_count);
  g_ptr_array_unref(entries);
  return deleted_count;
}

char *gnostr_cache_stats_string(void)
{
  int image_count = 0;
  gint64 image_size = gnostr_cache_get_image_size(&image_count);
  gint64 ndb_size = gnostr_cache_get_ndb_size();

  double image_mb = image_size / (1024.0 * 1024.0);
  double ndb_mb = ndb_size / (1024.0 * 1024.0);

  return g_strdup_printf("Images: %.1f MB (%d files), NDB: %.1f MB",
                         image_mb, image_count, ndb_mb);
}

void gnostr_cache_prune_init(void)
{
  g_message("cache_prune: initializing cache pruning system");

  /* Try to get settings from GSettings */
  g_autoptr(GSettings) settings = NULL;
  gboolean prune_on_startup = TRUE;
  int image_max_mb = 500;
  int ndb_max_mb = 1024;

  /* Check if schema is available */
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (source) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(source, "org.gnostr.Client", TRUE);
    if (schema) {
      g_settings_schema_unref(schema);
      settings = g_settings_new("org.gnostr.Client");

      if (settings) {
        prune_on_startup = g_settings_get_boolean(settings, "cache-prune-on-startup");
        image_max_mb = g_settings_get_int(settings, "image-cache-max-mb");
        ndb_max_mb = g_settings_get_int(settings, "ndb-cache-max-mb");
      }
    } else {
      g_debug("cache_prune: GSettings schema not available, using defaults");
    }
  }

  /* Log current cache stats */
  g_autofree char *stats = gnostr_cache_stats_string();
  g_message("cache_prune: current cache status: %s", stats);
  g_message("cache_prune: settings: prune_on_startup=%s, image_max=%dMB, ndb_max=%dMB",
            prune_on_startup ? "true" : "false", image_max_mb, ndb_max_mb);

  if (!prune_on_startup) {
    g_message("cache_prune: auto-prune disabled by settings");
    return;
  }

  /* Prune image cache */
  if (image_max_mb > 0) {
    int deleted = gnostr_cache_prune_images(image_max_mb);
    if (deleted > 0) {
      g_message("cache_prune: pruned %d image files", deleted);
    }
  }

  /* Note: nostrdb pruning is more complex because LMDB doesn't support
   * simple file deletion. The database would need to be compacted or
   * old entries deleted via the nostrdb API (which doesn't exist yet).
   * For now, we only log the size as informational.
   *
   * Future improvement: Implement nostrdb event pruning based on:
   * - Event age (delete events older than X days)
   * - Event kind priority (keep profiles longer than reactions)
   * - Reference counting (keep events that are referenced)
   */
  gint64 ndb_size = gnostr_cache_get_ndb_size();
  gint64 ndb_limit_bytes = (gint64)ndb_max_mb * 1024 * 1024;

  if (ndb_max_mb > 0 && ndb_size > ndb_limit_bytes) {
    g_warning("cache_prune: nostrdb size (%.1f MB) exceeds limit (%d MB). "
              "Note: Automatic nostrdb pruning is not yet implemented. "
              "Consider deleting %s/data.mdb to reset the database.",
              ndb_size / (1024.0 * 1024.0), ndb_max_mb,
              g_get_user_cache_dir());
  }
}
