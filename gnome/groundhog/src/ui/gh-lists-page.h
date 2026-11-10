#ifndef GH_LISTS_PAGE_H
#define GH_LISTS_PAGE_H
#include <adwaita.h>
#include <nostr-gobject-1.0/gnostr-list-service.h>
G_BEGIN_DECLS
/* Copied backend; owner is referenced by each page. It emits changed when its
 * account binding or snapshots change. No UI object ever borrows a GhStore. */
typedef struct {
  GObject *owner;
  guint64 (*generation)(GObject *owner);
  GNostrListService *(*service)(GObject *owner); /* borrowed, synchronous only */
  GNostrListDocument *(*ensure)(GObject *owner, gint kind, const gchar *id, GError **error);
  GNostrListDocument *(*apply)(GObject *owner, gint kind, const gchar *id, guint64 revision,
                              GPtrArray *operations, GError **error);
  GNostrListDocument *(*import)(GObject *owner, const gchar *json, GError **error);
  gchar *(*export)(GNostrListDocument *snapshot);
  gboolean (*get_sync)(GObject *owner);
  gboolean (*set_sync)(GObject *owner, gboolean enabled, GError **error);
  gboolean sync_available;
  const gchar *(*visibility_error)(GObject *owner);
  gboolean (*retry_visibility)(GObject *owner, GError **error);
} GhListsBackend;

#define GH_TYPE_LISTS_PAGE (gh_lists_page_get_type())
G_DECLARE_FINAL_TYPE(GhListsPage, gh_lists_page, GH, LISTS_PAGE, AdwNavigationPage)
GhListsPage *gh_lists_page_new(const GhListsBackend *backend);
/* Only generic sets and command definitions/references are exposed in S3.
 * Feature-specific lists are deliberately hidden until their consumers land. */
gboolean gh_lists_page_kind_visible(gint kind);
G_END_DECLS
#endif
