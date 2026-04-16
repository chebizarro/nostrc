#define G_LOG_DOMAIN "gnostr-main-window-navigation"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-search-results-view.h"
#include <nostr-gtk-1.0/gnostr-timeline-view.h>
#include "gnostr-notifications-view.h"
#include "gnostr-classifieds-view.h"
#include "page-discover.h"
#include "gnostr-article-reader.h"

#include <nostr-gtk-1.0/gnostr-profile-pane.h>
#include <nostr-gtk-1.0/gnostr-thread-view.h>

#include "../util/utils.h"

#include <string.h>

static void
open_profile_panel(GnostrMainWindow *self, const char *pubkey_hex)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  g_return_if_fail(pubkey_hex != NULL);

  GtkWidget *profile_pane = self->session_view
    ? gnostr_session_view_get_profile_pane(self->session_view) : NULL;
  if (!profile_pane || !NOSTR_GTK_IS_PROFILE_PANE(profile_pane))
    return;

  gboolean sidebar_visible = gnostr_main_window_is_panel_visible_internal(self)
    && gnostr_session_view_is_showing_profile(self->session_view);
  const char *current = nostr_gtk_profile_pane_get_current_pubkey(NOSTR_GTK_PROFILE_PANE(profile_pane));
  if (sidebar_visible && current && strcmp(current, pubkey_hex) == 0) {
    gnostr_main_window_hide_panel_internal(self);
    return;
  }

  gnostr_main_window_show_profile_panel_internal(self);
  nostr_gtk_profile_pane_set_pubkey(NOSTR_GTK_PROFILE_PANE(profile_pane), pubkey_hex);
}

static void
open_article_panel(GnostrMainWindow *self, const char *event_id)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  g_return_if_fail(event_id != NULL);

  GtkWidget *reader = self->session_view
    ? gnostr_session_view_get_article_reader(self->session_view) : NULL;
  if (!reader || !GNOSTR_IS_ARTICLE_READER(reader))
    return;

  gnostr_article_reader_load_event(GNOSTR_ARTICLE_READER(reader), event_id);
  gnostr_main_window_show_article_panel_internal(self);
}

void
gnostr_main_window_on_profile_pane_close_requested_internal(NostrGtkProfilePane *pane,
                                                            gpointer user_data)
{
  (void)pane;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;
  gnostr_main_window_hide_panel_internal(self);
}

void
gnostr_main_window_on_discover_open_profile_internal(GnostrPageDiscover *page,
                                                     const char *pubkey_hex,
                                                     gpointer user_data)
{
  (void)page;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;
  open_profile_panel(self, pubkey_hex);
}

void
gnostr_main_window_on_search_open_note_internal(GnostrSearchResultsView *view,
                                                const char *event_id_hex,
                                                gpointer user_data)
{
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_id_hex)
    return;
  gnostr_main_window_view_thread(GTK_WIDGET(self), event_id_hex);
}

void
gnostr_main_window_on_search_open_profile_internal(GnostrSearchResultsView *view,
                                                   const char *pubkey_hex,
                                                   gpointer user_data)
{
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;
  open_profile_panel(self, pubkey_hex);
}

void
gnostr_main_window_on_search_search_hashtag_internal(GnostrSearchResultsView *view,
                                                     const char *hashtag,
                                                     gpointer user_data)
{
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !hashtag || !*hashtag)
    return;

  GtkWidget *timeline = self->session_view ? gnostr_session_view_get_timeline(self->session_view) : NULL;
  if (timeline && NOSTR_GTK_IS_TIMELINE_VIEW(timeline)) {
    nostr_gtk_timeline_view_add_hashtag_tab(NOSTR_GTK_TIMELINE_VIEW(timeline), hashtag);
    if (self->session_view)
      gnostr_session_view_show_page(self->session_view, "timeline");
    g_debug("[SEARCH] Navigated to hashtag #%s from search results", hashtag);
  }
}

void
gnostr_main_window_on_notification_open_note_internal(GnostrNotificationsView *view,
                                                      const char *note_id,
                                                      gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !note_id)
    return;

  gnostr_main_window_view_thread(GTK_WIDGET(self), note_id);
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(view), GTK_TYPE_POPOVER);
  if (popover)
    gtk_popover_popdown(GTK_POPOVER(popover));
}

void
gnostr_main_window_on_notification_open_profile_internal(GnostrNotificationsView *view,
                                                         const char *pubkey_hex,
                                                         gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;

  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(view), GTK_TYPE_POPOVER);
  if (popover)
    gtk_popover_popdown(GTK_POPOVER(popover));
}

/* nostrc-hz5v.4: DM notification click opens the conversation with the peer */
void
gnostr_main_window_on_notification_open_conversation_internal(GnostrNotificationsView *view,
                                                               const char *peer_pubkey,
                                                               gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !peer_pubkey)
    return;

  gnostr_main_window_navigate_to_dm_conversation_internal(self, peer_pubkey);
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(view), GTK_TYPE_POPOVER);
  if (popover)
    gtk_popover_popdown(GTK_POPOVER(popover));
}

void
gnostr_main_window_on_classifieds_open_profile_internal(GnostrClassifiedsView *view,
                                                        const char *pubkey_hex,
                                                        gpointer user_data)
{
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;
  open_profile_panel(self, pubkey_hex);
}

void
gnostr_main_window_on_classifieds_listing_clicked_internal(GnostrClassifiedsView *view,
                                                           const char *event_id,
                                                           const char *naddr,
                                                           gpointer user_data)
{
  (void)view;
  (void)naddr;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_id)
    return;

  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    nostr_gtk_thread_view_set_focus_event(NOSTR_GTK_THREAD_VIEW(thread_view), event_id, NULL);
    gnostr_main_window_show_thread_panel_internal(self);
  }
}

void
gnostr_main_window_on_discover_open_article_internal(GnostrPageDiscover *page,
                                                     const char *event_id,
                                                     gint kind,
                                                     gpointer user_data)
{
  (void)page;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_id)
    return;

  open_article_panel(self, event_id);
  g_debug("[ARTICLES] Open article in reader: kind=%d, id=%s", kind, event_id);
}

void
gnostr_main_window_open_profile(GtkWidget *window, const char *pubkey_hex)
{
  if (!window || !GTK_IS_APPLICATION_WINDOW(window))
    return;

  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
  if (!hex)
    return;

  open_profile_panel(self, hex);
}

void
gnostr_main_window_view_thread(GtkWidget *window, const char *root_event_id)
{
  gnostr_main_window_view_thread_with_json(window, root_event_id, NULL);
}

void
gnostr_main_window_view_thread_with_json(GtkWidget *window,
                                         const char *root_event_id,
                                         const char *event_json)
{
  if (!window || !GTK_IS_APPLICATION_WINDOW(window))
    return;

  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  if (!root_event_id || strlen(root_event_id) != 64) {
    g_warning("[THREAD] Invalid root event ID for thread view");
    return;
  }

  g_debug("[THREAD] View thread requested for root=%s (json=%s)",
          root_event_id, event_json ? "provided" : "NULL");

  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (!thread_view || !NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    g_warning("[THREAD] Thread view widget not available");
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Thread view not available");
    return;
  }

  nostr_gtk_thread_view_set_thread_root_with_json(NOSTR_GTK_THREAD_VIEW(thread_view), root_event_id, event_json);
  gnostr_main_window_show_thread_panel_internal(self);
}

void
gnostr_main_window_on_thread_view_close_requested_internal(NostrGtkThreadView *view,
                                                           gpointer user_data)
{
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  gnostr_main_window_hide_panel_internal(self);

  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view))
    nostr_gtk_thread_view_clear(NOSTR_GTK_THREAD_VIEW(thread_view));
}

void
gnostr_main_window_on_article_reader_close_requested_internal(GnostrArticleReader *reader,
                                                              gpointer user_data)
{
  (void)reader;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;
  gnostr_main_window_hide_panel_internal(self);
}

void
gnostr_main_window_on_article_reader_open_profile_internal(GnostrArticleReader *reader,
                                                           const char *pubkey_hex,
                                                           gpointer user_data)
{
  (void)reader;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  gnostr_main_window_hide_panel_internal(self);
  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
}

void
gnostr_main_window_on_article_reader_open_url_internal(GnostrArticleReader *reader,
                                                       const char *url,
                                                       gpointer user_data)
{
  (void)reader;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !url)
    return;

  g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(url);
  gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
}

void
gnostr_main_window_on_article_reader_share_internal(GnostrArticleReader *reader,
                                                    const char *naddr_uri,
                                                    gpointer user_data)
{
  (void)reader;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !naddr_uri)
    return;

  GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_set_text(clipboard, naddr_uri);
  gnostr_main_window_show_toast(GTK_WIDGET(self), "Article link copied to clipboard");
}

void
gnostr_main_window_on_article_reader_zap_internal(GnostrArticleReader *reader,
                                                  const char *event_id,
                                                  const char *pubkey_hex,
                                                  const char *lud16,
                                                  gpointer user_data)
{
  (void)reader;
  (void)event_id;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;

  if (!lud16 || !*lud16) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Author has no Lightning address set");
    return;
  }

  gnostr_main_window_show_toast(GTK_WIDGET(self), "Zap dialog coming soon!");
  g_debug("[ARTICLE-READER] Zap requested: pubkey=%s", pubkey_hex);
}

void
gnostr_main_window_on_thread_view_open_profile_internal(NostrGtkThreadView *view,
                                                        const char *pubkey_hex,
                                                        gpointer user_data)
{
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view))
    gnostr_main_window_on_thread_view_close_requested_internal(NOSTR_GTK_THREAD_VIEW(thread_view), self);

  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
}

void
gnostr_main_window_on_thread_view_need_profile_internal(NostrGtkThreadView *view,
                                                        const char *pubkey_hex,
                                                        gpointer user_data)
{
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;
  if (strlen(pubkey_hex) != 64)
    return;

  gnostr_main_window_enqueue_profile_author_internal(self, pubkey_hex);
}
