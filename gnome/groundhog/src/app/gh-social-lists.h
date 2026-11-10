#ifndef GH_SOCIAL_LISTS_H
#define GH_SOCIAL_LISTS_H
#include "gh-account-store.h"
#include "gh-store-social.h"
G_BEGIN_DECLS
#define GH_TYPE_SOCIAL_LISTS (gh_social_lists_get_type())
G_DECLARE_FINAL_TYPE(GhSocialLists, gh_social_lists, GH, SOCIAL_LISTS, GObject)
/* Owned by GhAppServices. Main-context only. Signals: changed (),
 * sync-state-changed (), plus document-changed (GNostrListDocument).
 * Local relay denial and attachment destinations apply before networking. */
GhSocialLists *gh_social_lists_new(GhAccountStore *account_store);
GhSocialLists *gh_social_lists_new_with_settings(GhAccountStore *account_store, GSettings *legacy_settings);
GStrv gh_social_lists_dup_servers(GhSocialLists *self, GError **error);
gboolean gh_social_lists_set_servers(GhSocialLists *self, const gchar *const *servers, GError **error);
/* Explicit binding seam for session stores and tests; account_store may be NULL
 * at construction. The caller must close this binding before closing store. */
gboolean gh_social_lists_bind_store(GhSocialLists *self, GhStore *store, guint64 generation,
                                     GError **error);
void gh_social_lists_close(GhSocialLists *self);
GNostrListService *gh_social_lists_get_service(GhSocialLists *self); /* borrowed */
guint64 gh_social_lists_get_generation(GhSocialLists *self);
const gchar *gh_social_lists_get_error(GhSocialLists *self);
gboolean gh_social_lists_get_sync_enabled(GhSocialLists *self);
gboolean gh_social_lists_set_sync_enabled(GhSocialLists *self, gboolean enabled, GError **error);
/* Registers a local identity with no sources or targets. Returning a new
 * snapshot does not save it until an edit commits. Max 256 loaded documents. */
GNostrListDocument *gh_social_lists_ensure(GhSocialLists *self, gint kind,
                                          const gchar *identifier, GError **error);
GNostrListDocument *gh_social_lists_apply(GhSocialLists *self, gint kind, const gchar *identifier,
                                         guint64 revision, GPtrArray *operations, GError **error);
/* Local portable JSON {kind, identifier, public, private}. Export includes
 * plaintext private items only on explicit request; no signature or I/O. */
gchar *gh_social_lists_export(GNostrListDocument *snapshot);
GNostrListDocument *gh_social_lists_import(GhSocialLists *self, const gchar *json,
                                          GError **error);
G_END_DECLS
#endif
