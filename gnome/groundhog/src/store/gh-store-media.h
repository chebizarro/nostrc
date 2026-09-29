#ifndef GH_STORE_MEDIA_H
#define GH_STORE_MEDIA_H

#include "gh-nip17-file.h"
#include "gh-store.h"

G_BEGIN_DECLS

/* The decrypted attachment cache (privacy charter §3.3 table `media`, §6
 * "Cache and save", G21): plaintext that the user chose to download, kept
 * only inside the encrypted store, never in $XDG_CACHE_HOME, $TMPDIR or any
 * other file (P3, AT-5), so a second "Download" of the same file needs no
 * network. The cache is an LRU capped at GH_STORE_MEDIA_CAP bytes: each put
 * evicts the least recently used rows until the total fits. The store's
 * secure_delete overwrites evicted pages.
 *
 * Keyed by the file, not its address (W17 review B1). A row is kept under
 * the file's identity (gh_store_media_file_id(): x, the AES key and the GCM
 * nonce), never under x alone. x is public, the Blossom address of the
 * ciphertext, so a message that only names someone else's x, with any key,
 * must not be served, keep alive or even detect that file's plaintext: it
 * misses the cache and is downloaded and verified like any other.
 *
 * Bound to its messages (nostrc-5x5b; charter §3.7, P3). A row exists only
 * while a stored kind-15 message carries exactly that x, key and nonce: a put
 * for a file no stored message carries is refused, and every store
 * operation that deletes messages (the expiry and retention purge, forget
 * conversation, and with it block and delete request, and outbox delete;
 * gh-store.h) deletes, in the same transaction, each row a deleted message
 * carried and no remaining message carries, before the WAL checkpoint that
 * operation already makes. A disappearing photo disappears from the cache
 * with its message. Forgetting the account crypto-shreds the whole store.
 *
 * Store thread only. */

#define GH_STORE_MEDIA_CAP (200 * 1024 * 1024)

/* Keeps bytes (non-empty, at most GH_STORE_MAX_VALUE_SIZE and the cap) as
 * file's plaintext with mime (nullable, at most 127 bytes), marked used now,
 * then evicts least recently used rows beyond the cap. INVALID for an
 * incomplete file or a bad size; NOT_FOUND when no stored kind-15 message
 * carries file's x, key and nonce (e.g. it expired while downloading);
 * CORRUPT on a read-only store; FULL when the disk is. */
gboolean gh_store_media_put(GhStore *store, const GhNip17File *file, const gchar *mime,
                            GBytes *bytes, GError **error);
/* The plaintext kept for file (its x, key and nonce; in memory that is
 * wiped when freed), marking it used now, and *out_mime (nullable). NULL
 * without an error when the cache has none. */
GBytes *gh_store_media_get(GhStore *store, const GhNip17File *file, gchar **out_mime,
                           GError **error);
/* Removes file's row, if any. */
gboolean gh_store_media_remove(GhStore *store, const GhNip17File *file, GError **error);
/* Evicts least recently used rows until at most cap bytes remain (0 empties
 * the cache). */
gboolean gh_store_media_prune(GhStore *store, gint64 cap, GError **error);
/* The bytes the cache holds in total. */
gboolean gh_store_media_get_total(GhStore *store, gint64 *out_bytes, GError **error);

G_END_DECLS
#endif
