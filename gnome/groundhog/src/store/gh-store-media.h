#ifndef GH_STORE_MEDIA_H
#define GH_STORE_MEDIA_H

#include "gh-store.h"

G_BEGIN_DECLS

/* The decrypted attachment cache (privacy charter §3.3 table `media`, §6
 * "Cache and save", G21): plaintext that the user chose to download, kept
 * only inside the encrypted store, never in $XDG_CACHE_HOME, $TMPDIR or any
 * other file (P3, AT-5). Rows are keyed by the file's x (lowercase hex SHA-256
 * of its ciphertext), which the message names, so a second "Download" of the
 * same file needs no network. The cache is an LRU capped at
 * GH_STORE_MEDIA_CAP bytes: each put evicts the least recently used rows
 * until the total fits. The store's secure_delete overwrites evicted pages.
 * Store thread only. */

#define GH_STORE_MEDIA_CAP (200 * 1024 * 1024)

/* Keeps bytes (non-empty, at most GH_STORE_MAX_VALUE_SIZE and the cap) under
 * sha256 with mime (nullable, at most 127 bytes), marked used now, then
 * evicts least recently used rows beyond the cap. INVALID for a malformed
 * key or size; CORRUPT on a read-only store; FULL when the disk is. */
gboolean gh_store_media_put(GhStore *store, const gchar *sha256, const gchar *mime,
                            GBytes *bytes, GError **error);
/* The bytes kept under sha256 (in memory that is wiped when freed), marking
 * them used now, and *out_mime (nullable). NULL without an error when the
 * cache has none. */
GBytes *gh_store_media_get(GhStore *store, const gchar *sha256, gchar **out_mime,
                           GError **error);
/* Removes sha256's row, if any. */
gboolean gh_store_media_remove(GhStore *store, const gchar *sha256, GError **error);
/* Evicts least recently used rows until at most cap bytes remain (0 empties
 * the cache). */
gboolean gh_store_media_prune(GhStore *store, gint64 cap, GError **error);
/* The bytes the cache holds in total. */
gboolean gh_store_media_get_total(GhStore *store, gint64 *out_bytes, GError **error);

G_END_DECLS
#endif
