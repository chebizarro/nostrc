#define G_LOG_DOMAIN "gnostr-main-window-timeline"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include <nostr-gtk-1.0/gnostr-timeline-view.h>

#include "../model/gn-nostr-event-model.h"

#include <nostr-gobject-1.0/gn-timeline-query.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gtk-1.0/gn-timeline-tabs.h>

#include <string.h>

void
gnostr_main_window_on_event_model_need_profile_internal(GnNostrEventModel *model,
                                                         const char *pubkey_hex,
                                                         gpointer user_data)
{
  (void)model;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;
  if (strlen(pubkey_hex) != 64)
    return;

  gnostr_main_window_enqueue_profile_author_internal(self, pubkey_hex);
}

void
gnostr_main_window_on_timeline_scroll_value_changed_internal(GtkAdjustment *adj,
                                                             gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->event_model)
    return;
  if (gn_nostr_event_model_is_async_loading(self->event_model))
    return;

  gdouble value = gtk_adjustment_get_value(adj);
  gdouble upper = gtk_adjustment_get_upper(adj);
  gdouble page_size = gtk_adjustment_get_page_size(adj);
  gdouble lower = gtk_adjustment_get_lower(adj);

  guint n_items = g_list_model_get_n_items(G_LIST_MODEL(self->event_model));
  if (n_items > 0 && upper > lower) {
    gdouble row_height_estimate = (upper - lower) / (gdouble)n_items;
    if (row_height_estimate > 0) {
      guint visible_start = (guint)(value / row_height_estimate);
      guint visible_count = (guint)(page_size / row_height_estimate) + 2;
      guint visible_end = visible_start + visible_count;
      if (visible_end >= n_items)
        visible_end = n_items - 1;
      gn_nostr_event_model_set_visible_range(self->event_model, visible_start, visible_end);
    }
  }

  gboolean user_at_top = (value <= lower + 50.0);
  gn_nostr_event_model_set_user_at_top(self->event_model, user_at_top);

  guint batch = self->load_older_batch_size > 0 ? self->load_older_batch_size : 30;
  guint max_items = 200;

  gdouble top_threshold = lower + (page_size * 0.2);
  if (value <= top_threshold && upper > page_size) {
    gn_nostr_event_model_load_newer_async(self->event_model, batch, max_items);
    return;
  }

  gdouble bottom_threshold = upper - page_size - (page_size * 0.2);
  if (value >= bottom_threshold && upper > page_size)
    gn_nostr_event_model_load_older_async(self->event_model, batch, max_items);
}

void
gnostr_main_window_on_event_model_new_items_pending_internal(GnNostrEventModel *model,
                                                              guint count,
                                                              gpointer user_data)
{
  (void)model;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  g_debug("[NEW_NOTES] Pending count: %u", count);

  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
    gnostr_session_view_set_new_notes_count(self->session_view, count);
}

void
gnostr_main_window_on_timeline_tab_filter_changed_internal(NostrGtkTimelineView *view,
                                                            guint type,
                                                            const char *filter_value,
                                                            gpointer user_data)
{
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->event_model)
    return;

  g_debug("[TAB_FILTER] type=%u filter='%s'", type, filter_value ? filter_value : "(null)");

  GNostrTimelineQuery *query = NULL;

  switch ((GnTimelineTabType)type) {
    case GN_TIMELINE_TAB_GLOBAL:
      query = gnostr_timeline_query_new_global();
      break;

    case GN_TIMELINE_TAB_FOLLOWING:
      if (self->user_pubkey_hex && *self->user_pubkey_hex) {
        char **followed = storage_ndb_get_followed_pubkeys(self->user_pubkey_hex);
        if (followed) {
          gsize n_followed = 0;
          for (char **p = followed; *p; p++)
            n_followed++;

          if (n_followed > 0) {
            query = gnostr_timeline_query_new_for_authors((const char **)followed, n_followed);
            g_debug("[TAB_FILTER] Following tab: %zu followed pubkeys", n_followed);
          }
          g_strfreev(followed);
        }
      }
      if (!query) {
        query = gnostr_timeline_query_new_global();
        g_debug("[TAB_FILTER] Following tab: no contact list, showing global");
      }
      break;

    case GN_TIMELINE_TAB_HASHTAG:
      if (filter_value && *filter_value) {
        query = gnostr_timeline_query_new_for_hashtag(filter_value);
        g_debug("[TAB_FILTER] Created hashtag query for #%s", filter_value);
      } else {
        query = gnostr_timeline_query_new_global();
      }
      break;

    case GN_TIMELINE_TAB_AUTHOR:
      if (filter_value && *filter_value) {
        query = gnostr_timeline_query_new_for_author(filter_value);
        g_debug("[TAB_FILTER] Created author query for %s", filter_value);
      } else {
        query = gnostr_timeline_query_new_global();
      }
      break;

    case GN_TIMELINE_TAB_CUSTOM:
      query = gnostr_timeline_query_new_global();
      break;
  }

  if (query) {
    gn_nostr_event_model_set_timeline_query(self->event_model, query);
    gn_nostr_event_model_refresh_async(self->event_model);
    gnostr_timeline_query_free(query);
  }
}

static gboolean
scroll_to_top_idle(gpointer user_data)
{
  GnostrMainWindow *self = user_data;
  if (!self || !GNOSTR_IS_MAIN_WINDOW(self))
    return G_SOURCE_REMOVE;

  GtkWidget *timeline = self->session_view ? gnostr_session_view_get_timeline(self->session_view) : NULL;
  if (timeline && NOSTR_GTK_IS_TIMELINE_VIEW(timeline)) {
    GtkWidget *list_view = nostr_gtk_timeline_view_get_list_view(NOSTR_GTK_TIMELINE_VIEW(timeline));
    if (list_view && GTK_IS_LIST_VIEW(list_view))
      gtk_list_view_scroll_to(GTK_LIST_VIEW(list_view), 0, GTK_LIST_SCROLL_FOCUS, NULL);
  }

  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
    gnostr_session_view_set_new_notes_count(self->session_view, 0);

  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

void
gnostr_main_window_on_new_notes_clicked_internal(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  if (self->event_model && GN_IS_NOSTR_EVENT_MODEL(self->event_model))
    gn_nostr_event_model_set_user_at_top(self->event_model, TRUE);

  if (self->event_model && GN_IS_NOSTR_EVENT_MODEL(self->event_model))
    gn_nostr_event_model_flush_pending(self->event_model);

  g_idle_add_full(G_PRIORITY_DEFAULT, scroll_to_top_idle, g_object_ref(self), NULL);
}
