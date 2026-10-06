#pragma once
#include <gtk/gtk.h>
#include "gh-web-content.h"

G_BEGIN_DECLS

/* Profile pictures across the UI (owner decision, W32): consent is given
 * once per contact and kept in the account's encrypted store
 * (contacts.picture_allowed_at, schema v9; P8: local only), and
 * one download per URL serves every place that contact's picture is shown
 * (message rows, the sidebar, Conversation Info). Nothing is fetched for a
 * contact without consent; turning the load-profile-pictures preference
 * off revokes all of it (gh_picture_cache_revoke_all). */
#define GH_TYPE_PICTURE_CACHE (gh_picture_cache_get_type())
G_DECLARE_FINAL_TYPE(GhPictureCache, gh_picture_cache, GH, PICTURE_CACHE, GObject)

/* web may be NULL (no fetching: tests, no network) */
GhPictureCache *gh_picture_cache_new(GhWebContent *web);
/* Where consent is kept (the account's encrypted store, through the app):
 * list() returns every allowed pubkey, set() records or clears (0) one,
 * clear() revokes all. NULL backend between accounts: no consent, and the
 * cache is emptied. */
typedef struct {
  GStrv (*list)(gpointer data, GError **error);
  gboolean (*set)(gpointer data, const gchar *pubkey, gint64 allowed_at, GError **error);
  gboolean (*clear)(gpointer data, GError **error);
} GhPictureConsentBackend;
void gh_picture_cache_set_consent(GhPictureCache *self, const GhPictureConsentBackend *backend,
                                  gpointer data);
gboolean gh_picture_cache_is_allowed(GhPictureCache *self, const gchar *pubkey);
void gh_picture_cache_allow(GhPictureCache *self, const gchar *pubkey);
void gh_picture_cache_revoke_all(GhPictureCache *self);
/* The picture at uri for pubkey: the texture when loaded, else NULL. With
 * consent and a uri, a fetch starts (once per URL; a failed URL is not
 * retried until the next gh_picture_cache_allow for any contact) and
 * "picture-changed" (pubkey) follows. Borrowed. */
GdkTexture *gh_picture_cache_get(GhPictureCache *self, const gchar *pubkey, const gchar *uri);
/* The state of uri: TRUE while a fetch is under way. */
gboolean gh_picture_cache_is_loading(GhPictureCache *self, const gchar *uri);
gboolean gh_picture_cache_has_failed(GhPictureCache *self, const gchar *uri);

G_END_DECLS
