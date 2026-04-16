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

    case GN_TIMELINE_TAB_SEARCH:
      if (filter_value && *filter_value) {
        query = gnostr_timeline_query_new_for_search(filter_value);
        g_debug("[TAB_FILTER] Created search query for '%s'", filter_value);
      } else {
        query = gnostr_timeline_query_new_global();
      }
      break;

    case GN_TIMELINE_TAB_CUSTOM:
      /* Reserved for GnostrFilterSet wiring — nostrc-yg8j.4 */
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

/* Show the tab bar and install the persistent non-closable tabs (Global
 * and Following) on top of the default Global tab baked into the widget.
 *
 * Called from gnostr_main_window_init_widget_state_internal after the
 * session view's timeline widget has been created. Safe to call only
 * once — later calls are a no-op.
 *
 * Part of nostrc-e03f.4: Multiple Timeline Views Support. */
void
gnostr_main_window_setup_initial_tabs_internal(GnostrMainWindow *self)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->session_view)
    return;

  GtkWidget *timeline = gnostr_session_view_get_timeline(self->session_view);
  if (!timeline || !NOSTR_GTK_IS_TIMELINE_VIEW(timeline))
    return;

  NostrGtkTimelineView *tv = NOSTR_GTK_TIMELINE_VIEW(timeline);
  nostr_gtk_timeline_view_set_tabs_visible(tv, TRUE);

  NostrGtkTimelineTabs *tabs = nostr_gtk_timeline_view_get_tabs(tv);
  if (!tabs) return;

  /* Guard against duplicate setup. */
  if (nostr_gtk_timeline_tabs_find_tab_by_type(tabs, GN_TIMELINE_TAB_FOLLOWING) >= 0)
    return;

  /* Global is installed by the widget init at index 0. Add Following
   * alongside it at index 1, non-closable. Before login it falls back
   * to the global query; after login the dispatcher in
   * on_timeline_tab_filter_changed_internal picks up the followed authors
   * via storage_ndb_get_followed_pubkeys when the user activates it. */
  guint idx = nostr_gtk_timeline_tabs_add_tab(tabs,
                                              GN_TIMELINE_TAB_FOLLOWING,
                                              "Following",
                                              NULL);
  nostr_gtk_timeline_tabs_set_closable(tabs, idx, FALSE);
}

/* Re-trigger the currently-selected tab's filter dispatch. Used after
 * login to refresh the Following tab's author list without requiring
 * the user to re-click it. */
void
gnostr_main_window_refresh_current_tab_filter_internal(GnostrMainWindow *self)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->session_view)
    return;

  GtkWidget *timeline = gnostr_session_view_get_timeline(self->session_view);
  if (!timeline || !NOSTR_GTK_IS_TIMELINE_VIEW(timeline))
    return;

  NostrGtkTimelineView *tv = NOSTR_GTK_TIMELINE_VIEW(timeline);
  NostrGtkTimelineTabs *tabs = nostr_gtk_timeline_view_get_tabs(tv);
  if (!tabs || nostr_gtk_timeline_tabs_get_n_tabs(tabs) == 0)
    return;

  guint sel = nostr_gtk_timeline_tabs_get_selected(tabs);
  GnTimelineTabType type = nostr_gtk_timeline_tabs_get_tab_type(tabs, sel);
  const char *value = nostr_gtk_timeline_tabs_get_tab_filter_value(tabs, sel);

  gnostr_main_window_on_timeline_tab_filter_changed_internal(tv,
                                                              (guint)type,
                                                              value,
                                                              self);
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
