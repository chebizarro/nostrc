#ifndef GH_NIP46_AUTH_URL_H
#define GH_NIP46_AUTH_URL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GH_TYPE_NIP46_AUTH_URL (gh_nip46_auth_url_get_type())
G_DECLARE_FINAL_TYPE(GhNip46AuthUrl, gh_nip46_auth_url, GH, NIP46_AUTH_URL, GObject)

/* A per-owner authorization prompt. An active window launches immediately;
 * otherwise its notification opens the same URL once when activated. Context
 * may be NULL to use the application's active window. Emits "launch-failed"
 * if GtkUriLauncher cannot open the link. */
GhNip46AuthUrl *gh_nip46_auth_url_new(GtkApplication *app, GtkWidget *context);
gboolean gh_nip46_auth_url_handle(GhNip46AuthUrl *self, const gchar *url,
                                   GCancellable *cancellable);
void gh_nip46_auth_url_clear(GhNip46AuthUrl *self);
/* Read-only UI test probe; never exposes the URL. */
gboolean gh_nip46_auth_url_has_pending(GhNip46AuthUrl *self);
typedef void (*GhNip46AuthNoticeFunc)(GNotification *notice, gpointer user_data);
/* Test seam: intercept notification delivery without registering a desktop app. */
void gh_nip46_auth_url_set_notice_hook_for_test(GhNip46AuthUrl *self,
                                                GhNip46AuthNoticeFunc hook,
                                                gpointer user_data);
void gh_nip46_auth_url_activate_for_test(GhNip46AuthUrl *self);

G_END_DECLS
#endif
