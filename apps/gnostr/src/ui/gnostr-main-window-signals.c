#define G_LOG_DOMAIN "gnostr-main-window-signals"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-search-results-view.h"
#include "gnostr-notifications-view.h"
#include "gnostr-classifieds-view.h"
#include "gnostr-repo-browser.h"
#include "page-discover.h"
#include "gnostr-article-reader.h"

#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gtk-1.0/gnostr-profile-pane.h>
#include <nostr-gtk-1.0/gnostr-thread-view.h>

void
gnostr_main_window_connect_session_view_signals_internal(GnostrMainWindow *self,
                                                         GCallback settings_cb,
                                                         GCallback relays_cb,
                                                         GCallback reconnect_cb,
                                                         GCallback login_cb,
                                                         GCallback logout_cb,
                                                         GCallback view_profile_cb,
                                                         GCallback account_switch_cb,
                                                         GCallback new_notes_cb,
                                                         GCallback compose_cb)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  if (!self->session_view)
    return;

  g_signal_connect(self->session_view, "settings-requested", settings_cb, self);
  g_signal_connect(self->session_view, "relays-requested", relays_cb, self);
  g_signal_connect(self->session_view, "reconnect-requested", reconnect_cb, self);
  g_signal_connect(self->session_view, "login-requested", login_cb, self);
  g_signal_connect(self->session_view, "logout-requested", logout_cb, self);
  g_signal_connect(self->session_view, "view-profile-requested", view_profile_cb, self);
  g_signal_connect(self->session_view, "account-switch-requested", account_switch_cb, self);
  g_signal_connect(self->session_view, "new-notes-clicked", new_notes_cb, self);
  g_signal_connect(self->session_view, "compose-requested", compose_cb, self);
}

void
gnostr_main_window_connect_child_view_signals_internal(GnostrMainWindow *self,
                                                       GCallback profile_close_cb,
                                                       GCallback profile_mute_cb,
                                                       GCallback profile_follow_cb,
                                                       GCallback profile_message_cb,
                                                       GCallback thread_close_cb,
                                                       GCallback thread_need_profile_cb,
                                                       GCallback thread_open_profile_cb,
                                                       GCallback article_close_cb,
                                                       GCallback article_open_profile_cb,
                                                       GCallback article_open_url_cb,
                                                       GCallback article_share_cb,
                                                       GCallback article_zap_cb,
                                                       GCallback repo_selected_cb,
                                                       GCallback clone_requested_cb,
                                                       GCallback repo_refresh_cb,
                                                       GCallback repo_need_profile_cb,
                                                       GCallback repo_open_profile_cb)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  GtkWidget *profile_pane = self->session_view ? gnostr_session_view_get_profile_pane(self->session_view) : NULL;
  if (profile_pane && NOSTR_GTK_IS_PROFILE_PANE(profile_pane)) {
    g_signal_connect(profile_pane, "close-requested", profile_close_cb, self);
    g_signal_connect(profile_pane, "mute-user-requested", profile_mute_cb, self);
    g_signal_connect(profile_pane, "follow-requested", profile_follow_cb, self);
    g_signal_connect(profile_pane, "message-requested", profile_message_cb, self);
  }

  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    g_signal_connect(thread_view, "close-requested", thread_close_cb, self);
    g_signal_connect(thread_view, "need-profile", thread_need_profile_cb, self);
    g_signal_connect(thread_view, "open-profile", thread_open_profile_cb, self);
  }

  GtkWidget *article_reader = self->session_view ? gnostr_session_view_get_article_reader(self->session_view) : NULL;
  if (article_reader && GNOSTR_IS_ARTICLE_READER(article_reader)) {
    g_signal_connect(article_reader, "close-requested", article_close_cb, self);
    g_signal_connect(article_reader, "open-profile", article_open_profile_cb, self);
    g_signal_connect(article_reader, "open-url", article_open_url_cb, self);
    g_signal_connect(article_reader, "share-article", article_share_cb, self);
    g_signal_connect(article_reader, "zap-requested", article_zap_cb, self);
  }

  GtkWidget *repo_browser = self->session_view ? gnostr_session_view_get_repo_browser(self->session_view) : NULL;
  if (repo_browser && GNOSTR_IS_REPO_BROWSER(repo_browser)) {
    g_signal_connect(repo_browser, "repo-selected", repo_selected_cb, self);
    g_signal_connect(repo_browser, "clone-requested", clone_requested_cb, self);
    g_signal_connect(repo_browser, "refresh-requested", repo_refresh_cb, self);
    g_signal_connect(repo_browser, "need-profile", repo_need_profile_cb, self);
    g_signal_connect(repo_browser, "open-profile", repo_open_profile_cb, self);
  }
}

void
gnostr_main_window_connect_page_signals_internal(GnostrMainWindow *self,
                                                 GCallback discover_open_profile_cb,
                                                 GCallback discover_copy_npub_cb,
                                                 GCallback discover_open_communities_cb,
                                                 GCallback discover_open_article_cb,
                                                 GCallback discover_zap_article_cb,
                                                 GCallback discover_search_hashtag_cb,
                                                 GCallback search_open_note_cb,
                                                 GCallback search_open_profile_cb,
                                                 GCallback search_hashtag_cb,
                                                 GCallback notification_open_note_cb,
                                                 GCallback notification_open_profile_cb,
                                                 GCallback classifieds_open_profile_cb,
                                                 GCallback classifieds_contact_seller_cb,
                                                 GCallback classifieds_listing_clicked_cb)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  GtkWidget *discover_page = self->session_view ? gnostr_session_view_get_discover_page(self->session_view) : NULL;
  if (discover_page && GNOSTR_IS_PAGE_DISCOVER(discover_page)) {
    g_signal_connect(discover_page, "open-profile", discover_open_profile_cb, self);
    g_signal_connect(discover_page, "copy-npub-requested", discover_copy_npub_cb, self);
    g_signal_connect(discover_page, "open-communities", discover_open_communities_cb, self);
    g_signal_connect(discover_page, "open-article", discover_open_article_cb, self);
    g_signal_connect(discover_page, "zap-article-requested", discover_zap_article_cb, self);
    g_signal_connect(discover_page, "search-hashtag", discover_search_hashtag_cb, self);
  }

  GtkWidget *search_view = self->session_view ? gnostr_session_view_get_search_results_view(self->session_view) : NULL;
  if (search_view && GNOSTR_IS_SEARCH_RESULTS_VIEW(search_view)) {
    g_signal_connect(search_view, "open-note", search_open_note_cb, self);
    g_signal_connect(search_view, "open-profile", search_open_profile_cb, self);
    g_signal_connect(search_view, "search-hashtag", search_hashtag_cb, self);
  }

  GtkWidget *notif_view = self->session_view ? gnostr_session_view_get_notifications_view(self->session_view) : NULL;
  if (notif_view && GNOSTR_IS_NOTIFICATIONS_VIEW(notif_view)) {
    g_signal_connect(notif_view, "open-note", notification_open_note_cb, self);
    g_signal_connect(notif_view, "open-profile", notification_open_profile_cb, self);
  }

  GtkWidget *classifieds_view = self->session_view ? gnostr_session_view_get_classifieds_view(self->session_view) : NULL;
  if (classifieds_view && GNOSTR_IS_CLASSIFIEDS_VIEW(classifieds_view)) {
    g_signal_connect(classifieds_view, "open-profile", classifieds_open_profile_cb, self);
    g_signal_connect(classifieds_view, "contact-seller", classifieds_contact_seller_cb, self);
    g_signal_connect(classifieds_view, "listing-clicked", classifieds_listing_clicked_cb, self);
  }
}

void
gnostr_main_window_connect_window_signals_internal(GnostrMainWindow *self,
                                                   GCallback close_request_cb,
                                                   GCallback key_pressed_cb)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  g_signal_connect(self, "close-request", close_request_cb, NULL);
  self->relay_change_handler_id = gnostr_relay_change_connect(gnostr_main_window_on_relay_config_changed_internal, self);

  if (!self->key_controller) {
    self->key_controller = gtk_event_controller_key_new();
    g_signal_connect(self->key_controller, "key-pressed", key_pressed_cb, self);
    gtk_widget_add_controller(GTK_WIDGET(self), self->key_controller);
  }
}
