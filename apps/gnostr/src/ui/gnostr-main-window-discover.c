#define G_LOG_DOMAIN "gnostr-main-window-discover"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-timeline-view.h"
#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-conversation-view.h"
#include "page-discover.h"
#include "gnostr-classifieds-view.h"

#include <nostr-gobject-1.0/nostr_nip19.h>

#include <string.h>

void
gnostr_main_window_on_discover_copy_npub_internal(GnostrPageDiscover *page,
                                                  const char *pubkey_hex,
                                                  gpointer user_data)
{
  (void)page;
  (void)user_data;
  if (!pubkey_hex || strlen(pubkey_hex) != 64)
    return;

  g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_npub(pubkey_hex, NULL);
  if (!n19)
    return;

  const char *npub = gnostr_nip19_get_bech32(n19);
  if (!npub)
    return;

  GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_set_text(clipboard, npub);
}

void
gnostr_main_window_on_classifieds_contact_seller_internal(GnostrClassifiedsView *view,
                                                          const char *pubkey_hex,
                                                          const char *lud16,
                                                          gpointer user_data)
{
  (void)view;
  (void)lud16;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;

  GtkWidget *dm_inbox = (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
                          ? gnostr_session_view_get_dm_inbox(self->session_view)
                          : NULL;

  if (dm_inbox && GNOSTR_IS_DM_INBOX_VIEW(dm_inbox)) {
    GnostrDmConversation conv = {0};
    conv.peer_pubkey = g_strdup(pubkey_hex);
    conv.display_name = g_strdup("Seller");
    conv.last_timestamp = g_get_real_time() / 1000000;
    gnostr_dm_inbox_view_upsert_conversation(GNOSTR_DM_INBOX_VIEW(dm_inbox), &conv);
    g_free(conv.peer_pubkey);
    g_free(conv.display_name);

    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
      gnostr_session_view_show_page(self->session_view, "messages");
  }
}

void
gnostr_main_window_on_discover_open_communities_internal(GnostrPageDiscover *page,
                                                         gpointer user_data)
{
  (void)page;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  gnostr_main_window_show_toast(GTK_WIDGET(self), "Communities (NIP-72) - Coming soon!");
  g_debug("[COMMUNITIES] Open communities list requested");
}

void
gnostr_main_window_on_discover_zap_article_internal(GnostrPageDiscover *page,
                                                    const char *event_id,
                                                    const char *pubkey_hex,
                                                    const char *lud16,
                                                    gpointer user_data)
{
  (void)page;
  (void)event_id;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;

  if (!lud16 || !*lud16) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Author has no Lightning address set");
    return;
  }

  gnostr_main_window_show_toast(GTK_WIDGET(self), "Zap dialog coming soon!");
  g_debug("[ARTICLES] Zap article author requested: pubkey=%s, lud16=%s", pubkey_hex, lud16);
}

void
gnostr_main_window_on_discover_search_hashtag_internal(GnostrPageDiscover *page,
                                                       const char *hashtag,
                                                       gpointer user_data)
{
  (void)page;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !hashtag || !*hashtag)
    return;

  GtkWidget *timeline = self->session_view ? gnostr_session_view_get_timeline(self->session_view) : NULL;
  if (timeline && NOSTR_GTK_IS_TIMELINE_VIEW(timeline)) {
    nostr_gtk_timeline_view_add_hashtag_tab(NOSTR_GTK_TIMELINE_VIEW(timeline), hashtag);
    if (self->session_view)
      gnostr_session_view_show_page(self->session_view, "timeline");
    g_debug("[DISCOVER] Navigated to hashtag #%s from trending", hashtag);
  }
}
