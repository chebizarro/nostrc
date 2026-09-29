#ifndef GH_STORE_DIRECTORY_H
#define GH_STORE_DIRECTORY_H

#include "gh-store.h"

G_BEGIN_DECLS

/* The contact directory's rows of the encrypted store (privacy charter §3.3
 * table `directory`, schema v1; G10): per contact pubkey and kind (0,
 * 10050), the newest signed event Groundhog admitted and when the directory
 * last asked for it. The store keeps the raw signed JSON so every restore
 * re-verifies it; it never decides what is newest (the caller does, NIP-01)
 * but refuses to replace a row with an older event. Store thread only. */

#define GH_STORE_DIRECTORY_MAX_EVENT (64 * 1024)

typedef struct {
  gchar *pubkey;     /* 64 lowercase hex */
  gint kind;
  gchar *event_id;   /* 64 lowercase hex */
  gint64 created_at;
  gchar *event_json; /* signed, at most GH_STORE_DIRECTORY_MAX_EVENT bytes */
  gint64 fetched_at; /* unix seconds of the last answered fetch */
} GhStoreDirectoryEntry;

void gh_store_directory_entry_free(GhStoreDirectoryEntry *entry);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhStoreDirectoryEntry, gh_store_directory_entry_free)

/* Inserts or replaces (pubkey, kind) unless the stored event is newer
 * (created_at, then the lower id wins); fetched_at is updated either way. */
gboolean gh_store_directory_put(GhStore *store, const GhStoreDirectoryEntry *entry,
                                GError **error);
/* Moves fetched_at of pubkey's row of kind (every kind with kind < 0)
 * forward: a lookup of it was answered, whether or not it found anything. */
gboolean gh_store_directory_touch(GhStore *store, const gchar *pubkey, gint kind,
                                  gint64 fetched_at, GError **error);
/* Every row, as GhStoreDirectoryEntry. */
GPtrArray *gh_store_directory_load(GhStore *store, GError **error);
/* Deletes the rows of pubkey: every kind with kind < 0. */
gboolean gh_store_directory_delete(GhStore *store, const gchar *pubkey, gint kind,
                                   GError **error);

G_END_DECLS
#endif
