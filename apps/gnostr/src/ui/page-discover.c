/**
 * GnostrPageDiscover - Discover page for browsing and searching profiles
 *
 * Two modes:
 * 1. Local: Browse all cached profiles from nostrdb with sorting/filtering
 * 2. Network: NIP-50 search to index relays
 *
 * Features:
 * - Virtualized GtkListView for performance with large profile counts
 * - Sort by: recently seen, alphabetical, following first
 * - Filter by search text (name, NIP-05, bio)
 * - Empty state when no profiles cached
 */

#define G_LOG_DOMAIN "gnostr-discover"

#include "page-discover.h"
#include "page-discover-private.h"
#include "gnostr-articles-view.h"
#include "../model/gn-profile-list-model.h"
#include "../util/trending-hashtags.h"

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/page-discover.ui"

G_DEFINE_TYPE(GnostrPageDiscover, gnostr_page_discover, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_PROFILE,
    SIGNAL_FOLLOW_REQUESTED,
    SIGNAL_UNFOLLOW_REQUESTED,
    SIGNAL_MUTE_REQUESTED,
    SIGNAL_COPY_NPUB_REQUESTED,
    SIGNAL_OPEN_COMMUNITIES,
    SIGNAL_WATCH_LIVE,
    SIGNAL_OPEN_ARTICLE,
    SIGNAL_ZAP_ARTICLE_REQUESTED,
    SIGNAL_SEARCH_HASHTAG,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* --- Communities Button Handler --- */

static void
on_communities_clicked(GtkButton *button, GnostrPageDiscover *self)
{
    (void)button;
    g_signal_emit(self, signals[SIGNAL_OPEN_COMMUNITIES], 0);
}

/* --- Articles View Signal Handlers --- */

static void
on_articles_open_article(GnostrArticlesView *view, const char *event_id, gint kind, GnostrPageDiscover *self)
{
    (void)view;
    g_signal_emit(self, signals[SIGNAL_OPEN_ARTICLE], 0, event_id, kind);
}

static void
on_articles_open_profile(GnostrArticlesView *view, const char *pubkey_hex, GnostrPageDiscover *self)
{
    (void)view;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
}

static void
on_articles_zap_requested(GnostrArticlesView *view, const char *event_id,
                           const char *pubkey_hex, const char *lud16,
                           GnostrPageDiscover *self)
{
    (void)view;
    g_signal_emit(self, signals[SIGNAL_ZAP_ARTICLE_REQUESTED], 0, event_id, pubkey_hex, lud16);
}


void
gnostr_page_discover_emit_open_profile_internal(GnostrPageDiscover *self, const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
}

void
gnostr_page_discover_emit_watch_live_internal(GnostrPageDiscover *self, const char *event_id_hex)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    g_signal_emit(self, signals[SIGNAL_WATCH_LIVE], 0, event_id_hex);
}

void
gnostr_page_discover_emit_follow_requested_internal(GnostrPageDiscover *self, const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    g_signal_emit(self, signals[SIGNAL_FOLLOW_REQUESTED], 0, pubkey_hex);
}

void
gnostr_page_discover_emit_unfollow_requested_internal(GnostrPageDiscover *self, const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    g_signal_emit(self, signals[SIGNAL_UNFOLLOW_REQUESTED], 0, pubkey_hex);
}

void
gnostr_page_discover_emit_mute_requested_internal(GnostrPageDiscover *self, const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    g_signal_emit(self, signals[SIGNAL_MUTE_REQUESTED], 0, pubkey_hex);
}

void
gnostr_page_discover_emit_copy_npub_requested_internal(GnostrPageDiscover *self, const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    g_signal_emit(self, signals[SIGNAL_COPY_NPUB_REQUESTED], 0, pubkey_hex);
}

static void
switch_to_people_mode(GnostrPageDiscover *self)
{
    self->is_live_mode = FALSE;
    self->is_articles_mode = FALSE;

    gtk_widget_set_visible(GTK_WIDGET(self->search_entry), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->filter_row), TRUE);

    gnostr_page_discover_people_present(self);
}

static void
switch_to_live_mode(GnostrPageDiscover *self)
{
    self->is_live_mode = TRUE;
    self->is_articles_mode = FALSE;

    gtk_widget_set_visible(GTK_WIDGET(self->search_entry), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->filter_row), FALSE);

    gnostr_page_discover_live_present(self);
}

static void
switch_to_articles_mode(GnostrPageDiscover *self)
{
    self->is_live_mode = FALSE;
    self->is_articles_mode = TRUE;

    gtk_widget_set_visible(GTK_WIDGET(self->search_entry), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->filter_row), FALSE);

    gtk_stack_set_visible_child_name(self->content_stack, "articles");

    if (!self->articles_loaded && self->articles_view) {
        self->articles_loaded = TRUE;
        gnostr_articles_view_load_articles(self->articles_view);
    }
}

static void
on_mode_toggled(GtkToggleButton *button, GnostrPageDiscover *self)
{
    if (gtk_toggle_button_get_active(button)) {
        if (button == self->btn_mode_people) {
            gtk_toggle_button_set_active(self->btn_mode_live, FALSE);
            if (self->btn_mode_articles)
                gtk_toggle_button_set_active(self->btn_mode_articles, FALSE);
            switch_to_people_mode(self);
        } else if (button == self->btn_mode_live) {
            gtk_toggle_button_set_active(self->btn_mode_people, FALSE);
            if (self->btn_mode_articles)
                gtk_toggle_button_set_active(self->btn_mode_articles, FALSE);
            switch_to_live_mode(self);
        } else if (button == self->btn_mode_articles) {
            gtk_toggle_button_set_active(self->btn_mode_people, FALSE);
            gtk_toggle_button_set_active(self->btn_mode_live, FALSE);
            switch_to_articles_mode(self);
        }
    } else {
        gboolean any_active = gtk_toggle_button_get_active(self->btn_mode_people) ||
                              gtk_toggle_button_get_active(self->btn_mode_live) ||
                              (self->btn_mode_articles && gtk_toggle_button_get_active(self->btn_mode_articles));
        if (!any_active) {
            gtk_toggle_button_set_active(button, TRUE);
        }
    }
}

static void
on_refresh_live_clicked(GtkButton *button, GnostrPageDiscover *self)
{
    (void)button;
    gnostr_page_discover_live_reload(self);
}

/* --- Trending Hashtags --- */

static void
on_trending_hashtag_clicked(GtkButton *button, gpointer user_data)
{
  g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(user_data));
  
  GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);
  const char *tag = g_object_get_data(G_OBJECT(button), "hashtag");
  if (tag && *tag) {
    g_debug("discover: trending hashtag clicked: #%s", tag);
    g_signal_emit(self, signals[SIGNAL_SEARCH_HASHTAG], 0, tag);
  }
}

static void
populate_trending_flow_box(GnostrPageDiscover *self, GPtrArray *hashtags)
{
  /* Clear existing children */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->trending_flow_box))) != NULL) {
    gtk_flow_box_remove(self->trending_flow_box, child);
  }

  if (!hashtags || hashtags->len == 0) {
    gtk_widget_set_visible(GTK_WIDGET(self->trending_section), FALSE);
    return;
  }

  for (guint i = 0; i < hashtags->len; i++) {
    GnostrTrendingHashtag *ht = g_ptr_array_index(hashtags, i);
    
    /* Validate pointer before dereferencing */
    if (!ht) {
      g_warning("discover: NULL hashtag at index %u, skipping", i);
      continue;
    }
    
    if (!ht->tag) {
      g_warning("discover: hashtag at index %u has NULL tag, skipping", i);
      continue;
    }

    /* Create a clickable chip: "#tag (count)" */
    g_autofree gchar *label_text = g_strdup_printf("#%s", ht->tag);
    GtkWidget *button = gtk_button_new_with_label(label_text);

    gtk_widget_add_css_class(button, "pill");
    gtk_widget_add_css_class(button, "flat");

    /* Store tag string on the button for click handler */
    g_object_set_data_full(G_OBJECT(button), "hashtag",
                           g_strdup(ht->tag), g_free);

    g_signal_connect(button, "clicked",
                     G_CALLBACK(on_trending_hashtag_clicked), self);

    gtk_flow_box_append(self->trending_flow_box, button);
  }

  gtk_widget_set_visible(GTK_WIDGET(self->trending_section), TRUE);
  g_debug("discover: populated %u trending hashtags", hashtags->len);
}

static void
on_trending_hashtags_ready(GPtrArray *hashtags, gpointer user_data)
{
  GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);
  if (!GTK_IS_WIDGET(self) || !self->trending_flow_box) {
    g_ptr_array_unref(hashtags);
    return;
  }

  /* Show feedback if trending hashtags failed to load */
  if (!hashtags || hashtags->len == 0) {
    g_debug("discover: trending hashtags unavailable (empty result)");
    /* Hide the section - this is expected when NDB has few events */
  }

  populate_trending_flow_box(self, hashtags);
  g_ptr_array_unref(hashtags);
}

static void
load_trending_hashtags(GnostrPageDiscover *self)
{
  if (self->trending_loaded) return;
  self->trending_loaded = TRUE;

  /* Create cancellable for this operation */
  self->trending_cancellable = g_cancellable_new();

  /* Compute in background: scan last 500 kind-1 events, return top 15.
   * `self` is a borrowed reference kept alive by the widget tree; no
   * destroy function is needed. */
  gnostr_compute_trending_hashtags_async(500, 15,
                                         on_trending_hashtags_ready, self,
                                         NULL,
                                         self->trending_cancellable);
}

/* --- GObject Implementation --- */

static void
gnostr_page_discover_dispose(GObject *object)
{
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(object);

    gnostr_page_discover_people_dispose(self);

    if (self->trending_cancellable) {
        g_cancellable_cancel(self->trending_cancellable);
        g_clear_object(&self->trending_cancellable);
    }

    gnostr_page_discover_live_dispose(self);


    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_page_discover_parent_class)->dispose(object);
}

static void
gnostr_page_discover_finalize(GObject *object)
{
    G_OBJECT_CLASS(gnostr_page_discover_parent_class)->finalize(object);
}

static void
gnostr_page_discover_class_init(GnostrPageDiscoverClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_page_discover_dispose;
    object_class->finalize = gnostr_page_discover_finalize;

    /* Ensure child widget types are registered before loading template */
    g_type_ensure(GNOSTR_TYPE_ARTICLES_VIEW);

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

    /* Bind template children - Profile search */
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, search_entry);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_local);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_network);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, sort_dropdown);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, lbl_profile_count);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, results_list);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, empty_state);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_communities);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, filter_row);

    /* Bind template children - Mode toggle */
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_mode_people);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_mode_live);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_mode_articles);

    /* Bind template children - Articles view */
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, articles_view);

    /* Bind template children - Trending Hashtags */
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, trending_section);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, trending_flow_box);

    /* Bind template children - Live Activities */
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, live_flow_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, scheduled_flow_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, live_now_section);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, scheduled_section);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, live_loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_refresh_live);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_refresh_live_empty);

    /* Signals */
    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_FOLLOW_REQUESTED] = g_signal_new(
        "follow-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_UNFOLLOW_REQUESTED] = g_signal_new(
        "unfollow-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_MUTE_REQUESTED] = g_signal_new(
        "mute-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_COPY_NPUB_REQUESTED] = g_signal_new(
        "copy-npub-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_OPEN_COMMUNITIES] = g_signal_new(
        "open-communities",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_WATCH_LIVE] = g_signal_new(
        "watch-live",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_OPEN_ARTICLE] = g_signal_new(
        "open-article",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

    signals[SIGNAL_ZAP_ARTICLE_REQUESTED] = g_signal_new(
        "zap-article-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

    signals[SIGNAL_SEARCH_HASHTAG] = g_signal_new(
        "search-hashtag",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "page-discover");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_page_discover_init(GnostrPageDiscover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->is_live_mode = FALSE;
    self->is_articles_mode = FALSE;
    self->live_loaded = FALSE;
    self->articles_loaded = FALSE;
    self->live_activities = NULL;
    self->scheduled_activities = NULL;
    self->live_cancellable = NULL;
    self->live_sub_id = 0;

    gnostr_page_discover_people_init(self);

    /* Connect communities button */
    if (self->btn_communities) {
        g_signal_connect(self->btn_communities, "clicked",
                         G_CALLBACK(on_communities_clicked), self);
    }

    /* Connect mode toggle signals */
    if (self->btn_mode_people) {
        g_signal_connect(self->btn_mode_people, "toggled",
                         G_CALLBACK(on_mode_toggled), self);
    }
    if (self->btn_mode_live) {
        g_signal_connect(self->btn_mode_live, "toggled",
                         G_CALLBACK(on_mode_toggled), self);
    }
    if (self->btn_mode_articles) {
        g_signal_connect(self->btn_mode_articles, "toggled",
                         G_CALLBACK(on_mode_toggled), self);
    }

    /* Connect articles view signals */
    if (self->articles_view) {
        g_signal_connect(self->articles_view, "open-article",
                         G_CALLBACK(on_articles_open_article), self);
        g_signal_connect(self->articles_view, "open-profile",
                         G_CALLBACK(on_articles_open_profile), self);
        g_signal_connect(self->articles_view, "zap-requested",
                         G_CALLBACK(on_articles_zap_requested), self);
    }

    /* Connect live refresh buttons */
    if (self->btn_refresh_live) {
        g_signal_connect(self->btn_refresh_live, "clicked",
                         G_CALLBACK(on_refresh_live_clicked), self);
    }
    if (self->btn_refresh_live_empty) {
        g_signal_connect(self->btn_refresh_live_empty, "clicked",
                         G_CALLBACK(on_refresh_live_clicked), self);
    }

    /* Default to People mode - call switch function directly since
     * btn_mode_people may already be active from UI template, which
     * means gtk_toggle_button_set_active won't emit the toggled signal */
    if (self->btn_mode_people) {
        gtk_toggle_button_set_active(self->btn_mode_people, TRUE);
    }
    switch_to_people_mode(self);

    /* Load trending hashtags asynchronously */
    load_trending_hashtags(self);
}

GnostrPageDiscover *
gnostr_page_discover_new(void)
{
    return g_object_new(GNOSTR_TYPE_PAGE_DISCOVER, NULL);
}

void
gnostr_page_discover_load_profiles(GnostrPageDiscover *self)
{
    gnostr_page_discover_people_load_profiles_internal(self);
}

void
gnostr_page_discover_set_loading(GnostrPageDiscover *self, gboolean is_loading)
{
    gnostr_page_discover_people_set_loading_internal(self, is_loading);
}

void
gnostr_page_discover_clear_results(GnostrPageDiscover *self)
{
    gnostr_page_discover_people_clear_results_internal(self);
}

const char *
gnostr_page_discover_get_search_text(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), NULL);

    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    return (text && *text) ? text : NULL;
}

void
gnostr_page_discover_set_following(GnostrPageDiscover *self, const char **pubkeys)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    gn_profile_list_model_set_following_set(self->profile_model, pubkeys);
}

void
gnostr_page_discover_set_muted(GnostrPageDiscover *self, const char **pubkeys)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    gn_profile_list_model_set_muted_set(self->profile_model, pubkeys);
}

void
gnostr_page_discover_set_blocked(GnostrPageDiscover *self, const char **pubkeys)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    gn_profile_list_model_set_blocked_set(self->profile_model, pubkeys);
}

void
gnostr_page_discover_refresh(GnostrPageDiscover *self)
{
    gnostr_page_discover_people_refresh_internal(self);
}

gboolean
gnostr_page_discover_is_network_search_enabled(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), FALSE);
    return gtk_toggle_button_get_active(self->btn_network);
}

gboolean
gnostr_page_discover_is_local_search_enabled(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), TRUE);
    return gtk_toggle_button_get_active(self->btn_local);
}

guint
gnostr_page_discover_get_result_count(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), 0);

    if (self->is_local_mode) {
        return g_list_model_get_n_items(G_LIST_MODEL(self->profile_model));
    } else {
        return g_list_model_get_n_items(G_LIST_MODEL(self->network_results_model));
    }
}


void
gnostr_page_discover_load_live_activities(GnostrPageDiscover *self)
{
    gnostr_page_discover_live_load_internal(self);
}

gboolean
gnostr_page_discover_is_live_mode(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), FALSE);
    return self->is_live_mode;
}

gboolean
gnostr_page_discover_is_articles_mode(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), FALSE);
    return self->is_articles_mode;
}
