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
 *
 * Bound to its messages (nostrc-5x5b; charter §3.7, P3). A row exists only
 * while a stored kind-15 message names its x: a put for a file no stored
 * message names is refused, and every store operation that deletes messages
 * (the expiry and retention purge, forget conversation, and with it block
 * and delete request, and outbox delete; gh-store.h) deletes, in the same
 * transaction, each row a deleted message named and no remaining message
 * does, before the WAL checkpoint that operation already makes. A
 * disappearing photo disappears from the cache with its message. Forgetting
 * the account crypto-shreds the whole store.
 *
 * Store thread only. */

#define GH_STORE_MEDIA_CAP (200 * 1024 * 1024)

/* SQL: the x a kind-15 message row m names (its canonical rumor's
 * ["x", "<hex>"] tag, lowercased; compact JSON cannot hold that text
 * anywhere but in a tag), and whether m is such a message. Shared by the
 * cache and the store's message deletions. */
#define GH_STORE_MEDIA_X_OF(m) \
  "lower(substr(" m ".raw_json, instr(" m ".raw_json, '[\"x\",\"') + 6, 64))"
#define GH_STORE_MEDIA_FILE(m) \
  "(" m ".kind = 15 AND instr(" m ".raw_json, '[\"x\",\"') > 0)"
/* m is a kind-15 message naming the media row `media`. */
#define GH_STORE_MEDIA_NAMED_BY(m) \
  "(" GH_STORE_MEDIA_FILE(m) " AND " GH_STORE_MEDIA_X_OF(m) " = media.sha256)"

/* Keeps bytes (non-empty, at most GH_STORE_MAX_VALUE_SIZE and the cap) under
 * sha256 with mime (nullable, at most 127 bytes), marked used now, then
 * evicts least recently used rows beyond the cap. INVALID for a malformed
 * key or size; NOT_FOUND when no stored kind-15 message names sha256 (e.g.
 * it expired while downloading); CORRUPT on a read-only store; FULL when the
 * disk is. */
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
