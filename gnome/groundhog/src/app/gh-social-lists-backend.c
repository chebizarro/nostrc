#include "gh-social-lists-backend.h"

static guint64 generation(GObject *owner) { return gh_social_lists_get_generation(GH_SOCIAL_LISTS(owner)); }
static GNostrListService *service(GObject *owner) { return gh_social_lists_get_service(GH_SOCIAL_LISTS(owner)); }
static gboolean get_sync(GObject *owner) { return gh_social_lists_get_sync_enabled(GH_SOCIAL_LISTS(owner)); }
static gboolean set_sync(GObject *owner, gboolean enabled, GError **error)
{
  return gh_social_lists_set_sync_enabled(GH_SOCIAL_LISTS(owner), enabled, error);
}
static GNostrListDocument *ensure(GObject *owner, gint kind, const gchar *id, GError **error)
{
  return gh_social_lists_ensure(GH_SOCIAL_LISTS(owner), kind, id, error);
}
static GNostrListDocument *apply(GObject *owner, gint kind, const gchar *id, guint64 revision,
                                GPtrArray *operations, GError **error)
{
  GNostrListDocument *result = gh_social_lists_apply(GH_SOCIAL_LISTS(owner), kind, id, revision, operations, error);
  if (!result && error && g_error_matches(*error, GH_STORE_ERROR, GH_STORE_ERROR_FULL)) {
    g_clear_error(error);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Account list storage limit reached");
  }
  return result;
}
static GNostrListDocument *import(GObject *owner, const gchar *json, GError **error)
{
  return gh_social_lists_import(GH_SOCIAL_LISTS(owner), json, error);
}
GhListsBackend gh_social_lists_backend(GhSocialLists *self)
{
  return (GhListsBackend){
    .owner = G_OBJECT(self), .generation = generation, .service = service,
    .get_sync = get_sync, .set_sync = set_sync, .sync_available = FALSE,
    .ensure = ensure, .apply = apply, .import = import, .export = gh_social_lists_export,
  };
}
