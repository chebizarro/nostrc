#ifndef GH_WEB_CONTENT_H
#define GH_WEB_CONTENT_H
#include <gtk/gtk.h>
#include "gh-net-http.h"
G_BEGIN_DECLS
typedef enum { GH_WEB_PREVIEW, GH_WEB_IMAGE, GH_WEB_PICTURE, GH_WEB_N_KINDS } GhWebKind;
const gchar *gh_web_content_setting(GhWebKind kind);
typedef struct {
  gchar *title;
  gchar *description;
  GdkTexture *texture;
} GhWebResult;
void gh_web_result_free(GhWebResult *result);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhWebResult, gh_web_result_free)
#define GH_TYPE_WEB_CONTENT (gh_web_content_get_type())
G_DECLARE_FINAL_TYPE(GhWebContent, gh_web_content, GH, WEB_CONTENT, GObject)
/* No request on construction. A supplied transport is borrowed (tests only).
 * Production uses public-address-only GhNetHttp: no redirects or subresources. */
GhWebContent *gh_web_content_new(GSettings *settings, const GhHttpTransport *transport,
                                 gpointer data);
void gh_web_content_load_async(GhWebContent *self, const gchar *uri, GhWebKind kind,
                                GCancellable *cancel, GAsyncReadyCallback callback,
                                gpointer data);
GhWebResult *gh_web_content_load_finish(GhWebContent *self, GAsyncResult *result, GError **error);
G_END_DECLS
#endif
