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
static gboolean pending_import(GObject *owner, guint *accounts, guint *items)
{ return gh_social_lists_get_pending_import(GH_SOCIAL_LISTS(owner), accounts, items); }
static const gchar *import_revision(GObject *owner)
{ return gh_social_lists_get_pending_import_id(GH_SOCIAL_LISTS(owner)); }
static gboolean confirm_import(GObject *owner, gboolean accept, GError **error)
{ return gh_social_lists_confirm_import(GH_SOCIAL_LISTS(owner), accept, error); }
static const gchar *sync_error(GObject *owner)
{ return gh_social_lists_get_error(GH_SOCIAL_LISTS(owner)); }
static GStrv public_targets(GObject *owner)
{ return gh_social_lists_dup_public_targets(GH_SOCIAL_LISTS(owner)); }
GhListsBackend gh_social_lists_backend(GhSocialLists *self)
{
  return (GhListsBackend){
    .owner = G_OBJECT(self), .generation = generation, .service = service,
    .get_sync = get_sync, .set_sync = set_sync, .sync_available = TRUE,
    .pending_import = pending_import, .import_revision = import_revision,
    .confirm_import = confirm_import, .sync_error = sync_error,
    .dup_public_targets = public_targets,
    .ensure = ensure, .apply = apply, .import = import, .export = gh_social_lists_export,
  };
}
