/* nss-ui.h — shared UI plumbing for org.nostr.Settings pages.
 * SPDX-License-Identifier: MIT
 *
 * Pages are thin: they call the GTK-free core (src/core/) on worker
 * threads and render the results. Remote or user-supplied text is always
 * shown with use-markup = FALSE.
 */
#ifndef NSS_UI_H
#define NSS_UI_H

#include <adwaita.h>
#if __has_include(<gio/gdesktopappinfo.h>)
#include <gio/gdesktopappinfo.h>
#define NSS_HAVE_DESKTOP_APPINFO 1
#endif
#include <libsoup/soup.h>

G_BEGIN_DECLS

/* Shared by the window and every page; refcounted because worker-thread
 * callbacks may finish after the window closed. @window is a weak pointer
 * (NULL once the window is gone). */
typedef struct {
  gatomicrefcount       ref;
  GDBusConnection      *bus;       /* session bus; NULL if unavailable */
  SoupSession          *soup;
  AdwPreferencesWindow *window;
} NssContext;

NssContext *nss_context_new(AdwPreferencesWindow *window);
NssContext *nss_context_ref(NssContext *ctx);
void        nss_context_unref(NssContext *ctx);

void nss_toast(NssContext *ctx, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

/* Run @func(@data) on a worker thread, then @done(@result, @data) on the
 * main thread unless @owner was destroyed meanwhile. @result is freed with
 * @result_free after @done; @data with @data_free. */
typedef gpointer (*NssWorkFunc)(gpointer data, GError **error);
typedef void     (*NssDoneFunc)(GtkWidget *owner, gpointer result, const GError *error,
                                gpointer data);
void nss_run(GtkWidget *owner, NssWorkFunc func, NssDoneFunc done, gpointer data,
             GDestroyNotify data_free, GDestroyNotify result_free);

/* Rows */
GtkWidget *nss_info_row(const gchar *title, const gchar *subtitle);  /* plain text */
void       nss_row_set_subtitle_plain(GtkWidget *row, const gchar *subtitle);
GtkWidget *nss_status_row(const gchar *title, const gchar *subtitle, const gchar *icon);
/* Remove every row added with nss_group_add_dynamic(). */
void       nss_group_add_dynamic(AdwPreferencesGroup *g, GtkWidget *row);
void       nss_group_clear_dynamic(AdwPreferencesGroup *g);
GtkWidget *nss_suffix_button(const gchar *icon, const gchar *tooltip);
/* TRUE when @desktop_id is installed (always FALSE without GDesktopAppInfo,
 * i.e. on non-freedesktop platforms). */
gboolean   nss_desktop_installed(const gchar *desktop_id);
void       nss_launch_desktop(NssContext *ctx, const gchar *desktop_id, const gchar *action);

/* ── The user's published lists (worker-thread helpers) ────────────────── */
typedef struct {
  gchar     *pubkey_hex;
  gchar    **signer_relays;      /* GetRelays (never NULL) */
  GPtrArray *relay_list;         /* NssRelayEntry*; NULL = none published */
  gchar     *relay_list_source;
  gchar    **blossom;            /* NULL = none published */
  gchar     *blossom_source;
} NssUserLists;

void          nss_user_lists_free(NssUserLists *l);
/* Signer pubkey + GetRelays, then the newest verified kind 10002 from the
 * signer relays and the session relay; with @want_blossom also kind 10063
 * from the write relays too. */
NssUserLists *nss_user_lists_load(GDBusConnection *bus, gboolean want_blossom,
                                  GError **error);

typedef struct {
  GDBusConnection *bus;
  gchar           *unsigned_json;
  gchar          **targets;
  gchar          **required;
} NssPublishJob;

void      nss_publish_job_free(gpointer job);
/* Worker thread. Returns an NssPublishReport whenever at least one relay
 * was tried (success = n_required > 0 && n_required_ok == n_required);
 * NULL + error when signing failed or there was nothing to publish to. */
gpointer  nss_publish_job_run(gpointer job, GError **error);
void      nss_publish_report_free(gpointer report);
/* "wss://a: accepted\nwss://b: unreachable" */
gchar    *nss_publish_report_text(const gpointer report);

/* Pages */
AdwPreferencesPage *nss_page_identity_new(NssContext *ctx);
AdwPreferencesPage *nss_page_relays_new(NssContext *ctx);
AdwPreferencesPage *nss_page_notifications_new(NssContext *ctx);
AdwPreferencesPage *nss_page_wallet_new(NssContext *ctx);
AdwPreferencesPage *nss_page_media_new(NssContext *ctx);
AdwPreferencesPage *nss_page_files_new(NssContext *ctx);

GtkWindow *nss_window_new(GtkApplication *app, const gchar *page);

G_END_DECLS

#endif /* NSS_UI_H */
