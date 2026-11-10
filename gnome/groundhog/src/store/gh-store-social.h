#ifndef GH_STORE_SOCIAL_H
#define GH_STORE_SOCIAL_H

#include "gh-store.h"
#include <nostr-gobject-1.0/gnostr-list-service.h>

G_BEGIN_DECLS

#define GH_STORE_SOCIAL_MAX_BYTES (32u * 1024u * 1024u)

typedef struct {
  gboolean sync_enabled;
  gboolean blossom_imported;
} GhStoreSocialPreferences;
typedef struct {
  gint kind;
  gchar *identifier;
} GhStoreSocialIdentity;
void gh_store_social_identity_free(GhStoreSocialIdentity *identity);
GPtrArray *gh_store_social_list(GhStore *store, GError **error);
gboolean gh_store_social_get_preferences(GhStore *store, GhStoreSocialPreferences *out,
                                          GError **error);
gboolean gh_store_social_set_sync_enabled(GhStore *store, gboolean enabled, GError **error);
/* Replay state returned by GNostrListService. Owned; NULL without error = absent. */
gchar *gh_store_social_load(GhStore *store, gint kind, const gchar *identifier, GError **error);
/* Top-level transaction only: state, pending operations, removals and derived
 * indexes commit before any caller installs the snapshot or emits a signal.
 * decoded_json retains the engine's versioned replay state losslessly. */
gboolean gh_store_social_commit(GhStore *store, GNostrListDocument *snapshot,
                                 const gchar *state_json, GError **error);

G_END_DECLS
#endif
