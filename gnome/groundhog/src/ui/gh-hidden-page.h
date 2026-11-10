#ifndef GH_HIDDEN_PAGE_H
#define GH_HIDDEN_PAGE_H
#include "gh-lists-page.h"
G_BEGIN_DECLS
#define GH_TYPE_HIDDEN_PAGE (gh_hidden_page_get_type())
G_DECLARE_FINAL_TYPE(GhHiddenPage, gh_hidden_page, GH, HIDDEN_PAGE, AdwNavigationPage)
GhHiddenPage *gh_hidden_page_new(const GhListsBackend *backend);
/* Local, approved edits. The UI confirms irreversible future account drops. */
gboolean gh_hidden_page_add(GhHiddenPage *self, const gchar *tag, const gchar *value,
                            GError **error);
gboolean gh_hidden_page_remove(GhHiddenPage *self, guint position, GError **error);
G_END_DECLS
#endif
