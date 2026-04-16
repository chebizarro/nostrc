#define G_LOG_DOMAIN "gnostr-main-window-timeline"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include <nostr-gtk-1.0/gnostr-timeline-view.h>

#include "../model/gn-nostr-event-model.h"
#include "../model/gnostr-filter-set.h"
#include "../model/gnostr-filter-set-manager.h"
#include "../model/gnostr-filter-set-query.h"

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

    case GN_TIMELINE_TAB_CUSTOM: {
      /* Custom tabs carry a filter-set id in their filter_value.
       * Look it up in the default manager and convert to a runtime
       * query. If the set is missing (e.g. deleted after the tab was
       * opened) or the id is empty, fall back to the global feed so
       * the user never sees a dead tab.
       * nostrc-yg8j.4 */
      GnostrFilterSet *fs = NULL;
      if (filter_value && *filter_value) {
        GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
        if (mgr)
          fs = gnostr_filter_set_manager_get(mgr, filter_value);
      }
      if (fs) {
        query = gnostr_filter_set_to_timeline_query(fs);
        g_debug("[TAB_FILTER] Custom tab: filter-set '%s' (%s)",
                filter_value,
                gnostr_filter_set_get_name(fs) ?
                    gnostr_filter_set_get_name(fs) : "(unnamed)");
      } else {
        query = gnostr_timeline_query_new_global();
        g_debug("[TAB_FILTER] Custom tab: filter-set '%s' not found, "
                "falling back to global",
                filter_value ? filter_value : "(null)");
      }
      break;
    }
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

/* Called when the filter-set manager's list model changes (add/update/
 * remove). We only refresh the timeline if the currently-selected tab
 * is a CUSTOM tab AND the changed range actually contains that tab's
 * backing filter set. This keeps live-updating predefined sets (follow
 * list, mutes, bookmarks) from forcing a full requery every time they
 * tick. Everything else is handled lazily the next time the user clicks
 * a tab.
 * nostrc-yg8j.4 */
static void
on_filter_set_manager_items_changed(GListModel *model,
                                    guint position,
                                    guint removed,
                                    guint added,
                                    gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->session_view || !model)
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
  if (type != GN_TIMELINE_TAB_CUSTOM)
    return;

  const char *active_id = nostr_gtk_timeline_tabs_get_tab_filter_value(tabs, sel);
  if (!active_id || !*active_id)
    return;

  /* Removal of the active set: the removed item is gone from the model
   * by the time this handler runs, so fall back to refreshing — the
   * CUSTOM dispatch will see a NULL lookup and substitute the global
   * feed. Cheap and correct. */
  if (removed > 0 && added == 0) {
    gnostr_main_window_refresh_current_tab_filter_internal(self);
    return;
  }

  /* Otherwise only refresh if the [position, position + added) window
   * contains the id backing the active CUSTOM tab. */
  gboolean match = FALSE;
  for (guint i = 0; i < added; i++) {
    GnostrFilterSet *fs = g_list_model_get_item(model, position + i);
    if (!fs)
      continue;
    if (g_strcmp0(gnostr_filter_set_get_id(fs), active_id) == 0)
      match = TRUE;
    g_object_unref(fs);
    if (match)
      break;
  }

  if (match)
    gnostr_main_window_refresh_current_tab_filter_internal(self);
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

  /* Subscribe to filter-set manager changes so CUSTOM tabs refresh when
   * their underlying filter-set is edited or deleted. Using
   * g_signal_connect_object with G_CONNECT_DEFAULT ties the handler
   * lifetime to @self so the callback auto-disconnects when the window
   * is finalized. nostrc-yg8j.4 */
  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  if (mgr) {
    GListModel *model = gnostr_filter_set_manager_get_model(mgr);
    if (model) {
      g_signal_connect_object(model,
                              "items-changed",
                              G_CALLBACK(on_filter_set_manager_items_changed),
                              self,
                              G_CONNECT_DEFAULT);
    }
  }
}

/* Open (or focus) a custom tab backed by the filter set with the given
 * id. Safe to call before the manager has loaded the set — an unknown
 * id will surface via the CUSTOM dispatch fallback (global query +
 * g_debug) and later self-correct when the set appears via the manager's
 * items-changed signal.
 *
 * Returns silently on invalid input or when the timeline widget isn't
 * ready yet; the caller doesn't need to handle those cases specially.
 *
 * nostrc-yg8j.4 */
void
gnostr_main_window_open_filter_set_tab_internal(GnostrMainWindow *self,
                                                 const char *filter_set_id)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !filter_set_id || !*filter_set_id)
    return;
  if (!self->session_view)
    return;

  GtkWidget *timeline = gnostr_session_view_get_timeline(self->session_view);
  if (!timeline || !NOSTR_GTK_IS_TIMELINE_VIEW(timeline))
    return;

  NostrGtkTimelineView *tv = NOSTR_GTK_TIMELINE_VIEW(timeline);
  NostrGtkTimelineTabs *tabs = nostr_gtk_timeline_view_get_tabs(tv);
  if (!tabs)
    return;

  /* Prefer the filter-set name as the tab label; fall back to the id
   * when the set is unknown (should be rare — the switcher will
   * normally validate the id before calling us). */
  const char *label = filter_set_id;
  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  if (mgr) {
    GnostrFilterSet *fs = gnostr_filter_set_manager_get(mgr, filter_set_id);
    if (fs) {
      const char *name = gnostr_filter_set_get_name(fs);
      if (name && *name)
        label = name;
    }
  }

  /* Deduplicate: focus an existing tab with the same filter-set id
   * instead of opening a second one. */
  gint existing = nostr_gtk_timeline_tabs_find_tab_by_type_and_value(
      tabs, GN_TIMELINE_TAB_CUSTOM, filter_set_id);
  if (existing >= 0) {
    nostr_gtk_timeline_tabs_set_selected(tabs, (guint)existing);
  } else {
    guint idx = nostr_gtk_timeline_tabs_add_tab(tabs,
                                                GN_TIMELINE_TAB_CUSTOM,
                                                label,
                                                filter_set_id);
    nostr_gtk_timeline_tabs_set_closable(tabs, idx, TRUE);
    nostr_gtk_timeline_tabs_set_selected(tabs, idx);
  }

  /* Always surface the timeline page — if the caller was on Discover,
   * DMs, or another stack page the tab selection alone would be
   * invisible. Keeping this inside the internal helper means every
   * programmatic entry point (switcher sidebar, deep links, tests) gets
   * consistent navigation behaviour. */
  gnostr_session_view_show_page(self->session_view, "timeline");
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
