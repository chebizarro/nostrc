#ifndef GH_LIST_EDITOR_PAGE_H
#define GH_LIST_EDITOR_PAGE_H
#include "gh-lists-page.h"
G_BEGIN_DECLS
#define GH_TYPE_LIST_EDITOR_PAGE (gh_list_editor_page_get_type())
G_DECLARE_FINAL_TYPE(GhListEditorPage, gh_list_editor_page, GH, LIST_EDITOR_PAGE, AdwNavigationPage)
GhListEditorPage *gh_list_editor_page_new(const GhListsBackend *backend, GNostrListDocument *snapshot);
/* Actual UI save path; operations are committed before updating the page. */
gboolean gh_list_editor_page_save_tag(GhListEditorPage *self, const gchar *tag_json,
                                      gboolean private_tag, GError **error);
GNostrListDocument *gh_list_editor_page_get_snapshot(GhListEditorPage *self); /* borrowed */
const gchar *gh_list_editor_page_get_status(GhListEditorPage *self);
G_END_DECLS
#endif
