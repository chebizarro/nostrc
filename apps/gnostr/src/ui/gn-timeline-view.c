/**
 * GnTimelineView - Timeline display widget
 *
 * A GTK widget that wraps GtkListView with efficient factory and scroll
 * handling for displaying timeline content.
 *
 * Part of the Timeline Architecture Refactor (nostrc-e03f)
 */

#define G_LOG_DOMAIN "gn-timeline-view"

#include "gn-timeline-view.h"
#include "note_card_row.h"
#include "../model/gn-nostr-event-item.h"
#include <adwaita.h>

struct _GnTimelineView {
  GtkWidget parent_instance;

  /* Main widgets */
  GtkWidget *overlay;
  GtkWidget *scrolled_window;
  GtkWidget *list_view;
  GtkWidget *empty_page;
  GtkWidget *empty_label;

  /* New notes indicator */
  GtkWidget *new_notes_revealer;
  GtkWidget *new_notes_button;
  GtkWidget *new_notes_label;

  /* Loading indicator */
  GtkWidget *loading_revealer;
  GtkWidget *loading_spinner;

  /* Model */
  GnTimelineModel *model;
  GtkSelectionModel *selection_model;
  GtkListItemFactory *factory;

  /* Scroll state */
  gboolean user_at_top;
  guint scroll_check_id;
  gdouble last_scroll_position;

  /* Signals */
  gulong model_items_changed_id;
  gulong model_pending_id;
  gulong vadjustment_changed_id;
};

G_DEFINE_TYPE(GnTimelineView, gn_timeline_view, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_MODEL,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum {
  SIGNAL_ACTIVATE,
  SIGNAL_SHOW_PROFILE,
  SIGNAL_SHOW_THREAD,
  SIGNAL_NEED_PROFILE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_model_items_changed(GListModel *model, guint position, guint removed, guint added, gpointer user_data);
static void on_model_pending(GnTimelineModel *model, guint count, gpointer user_data);
static void on_new_notes_clicked(GtkButton *button, gpointer user_data);
static void on_vadjustment_changed(GtkAdjustment *adj, gpointer user_data);
static void factory_setup_cb(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data);
static void factory_bind_cb(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data);
static void factory_unbind_cb(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data);

/* ============== Factory Callbacks ============== */

static void factory_setup_cb(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  GnostrNoteCardRow *row = gnostr_note_card_row_new();
  gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void factory_bind_cb(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  GnTimelineView *self = GN_TIMELINE_VIEW(user_data);

  GtkWidget *child = gtk_list_item_get_child(list_item);
  if (!GNOSTR_IS_NOTE_CARD_ROW(child)) return;

  GnostrNoteCardRow *row = GNOSTR_NOTE_CARD_ROW(child);

  /*
   * CRITICAL: Prepare for bind FIRST - this resets the disposed flag and
   * creates fresh cancellables. Without this, widgets that were unbound
   * (disposed=TRUE) would fail to work properly when rebound.
   */
  gnostr_note_card_row_prepare_for_bind(row);

  GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(gtk_list_item_get_item(list_item));

  if (!GN_IS_NOSTR_EVENT_ITEM(item)) return;

  /* Extract data from item */
  gchar *id_hex = NULL, *pubkey = NULL, *content = NULL;
  gchar *root_id = NULL, *parent_id = NULL;
  gint64 created_at = 0;
  guint depth = 0;

  g_object_get(G_OBJECT(item),
               "event-id", &id_hex,
               "pubkey", &pubkey,
               "created-at", &created_at,
               "content", &content,
               "thread-root-id", &root_id,
               "parent-id", &parent_id,
               "reply-depth", &depth,
               NULL);

  /* Get profile info */
  gchar *display = NULL, *handle = NULL, *avatar_url = NULL, *nip05 = NULL;
  GObject *profile = NULL;
  g_object_get(G_OBJECT(item), "profile", &profile, NULL);
  if (profile) {
    g_object_get(profile,
                 "display-name", &display,
                 "name", &handle,
                 "picture-url", &avatar_url,
                 "nip05", &nip05,
                 NULL);
    g_object_unref(profile);
  }

  /* Fallback display name */
  gchar *display_fallback = NULL;
  if (!display && !handle && pubkey && strlen(pubkey) >= 8) {
    display_fallback = g_strdup_printf("%.8s...", pubkey);
  }

  /* Apply to row */
  gnostr_note_card_row_set_author(row, display ? display : display_fallback, handle, avatar_url);
  gnostr_note_card_row_set_timestamp(row, created_at, NULL);
  gnostr_note_card_row_set_content(row, content);
  gnostr_note_card_row_set_ids(row, id_hex, root_id, pubkey);
  gnostr_note_card_row_set_depth(row, depth);

  /* nostrc-5b8: Set thread info to show reply indicator for events with e-tags */
  gboolean is_reply = (parent_id != NULL && *parent_id != '\0');
  gnostr_note_card_row_set_thread_info(row, root_id, parent_id, NULL, is_reply);

  if (nip05 && pubkey) {
    gnostr_note_card_row_set_nip05(row, nip05, pubkey);
  }

  /* Request profile if needed */
  if (pubkey && !profile) {
    g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey);
  }

  /*
   * nostrc-0hp Phase 3: Apply reveal animation CSS class.
   * If the item is marked as "revealing", add the note-revealing CSS class
   * to trigger the fade/slide animation defined in gnostr.css.
   */
  if (gn_nostr_event_item_get_revealing(item)) {
    gtk_widget_add_css_class(child, "note-revealing");
    g_debug("[TIMELINE-VIEW] Applied note-revealing CSS class to item");
  } else {
    /* Ensure class is removed if item is rebound without revealing state */
    gtk_widget_remove_css_class(child, "note-revealing");
  }

  /* Cleanup */
  g_free(id_hex);
  g_free(pubkey);
  g_free(content);
  g_free(root_id);
  g_free(parent_id);
  g_free(display);
  g_free(handle);
  g_free(avatar_url);
  g_free(nip05);
  g_free(display_fallback);
}

static void factory_unbind_cb(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  GtkWidget *child = gtk_list_item_get_child(list_item);
  if (!GNOSTR_IS_NOTE_CARD_ROW(child)) return;

  GnostrNoteCardRow *row = GNOSTR_NOTE_CARD_ROW(child);

  /* nostrc-0hp Phase 3: Remove reveal animation CSS class on unbind */
  gtk_widget_remove_css_class(child, "note-revealing");

  /* Prepare for unbind - cancels async ops and clears resources */
  gnostr_note_card_row_prepare_for_unbind(row);
}

/* ============== Scroll Handling ============== */

static gboolean check_scroll_position(gpointer user_data) {
  GnTimelineView *self = GN_TIMELINE_VIEW(user_data);
  if (!GN_IS_TIMELINE_VIEW(self)) return G_SOURCE_REMOVE;

  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  if (!adj) return G_SOURCE_CONTINUE;

  gdouble value = gtk_adjustment_get_value(adj);
  gboolean at_top = (value < 50.0);  /* Within 50px of top */

  if (at_top != self->user_at_top) {
    self->user_at_top = at_top;
    if (self->model) {
      gn_timeline_model_set_user_at_top(self->model, at_top);
    }
  }

  self->last_scroll_position = value;
  return G_SOURCE_CONTINUE;
}

static void on_vadjustment_changed(GtkAdjustment *adj, gpointer user_data) {
  GnTimelineView *self = GN_TIMELINE_VIEW(user_data);
  if (!GN_IS_TIMELINE_VIEW(self)) return;

  gdouble value = gtk_adjustment_get_value(adj);
  gboolean at_top = (value < 50.0);

  if (at_top != self->user_at_top) {
    self->user_at_top = at_top;
    if (self->model) {
      gn_timeline_model_set_user_at_top(self->model, at_top);
    }

    /* Hide new notes indicator when scrolled to top */
    if (at_top) {
      gn_timeline_view_hide_new_notes_indicator(self);
    }
  }
}

/* ============== Model Signal Handlers ============== */

static void on_model_items_changed(GListModel *model, guint position, guint removed, guint added, gpointer user_data) {
  (void)model;
  (void)position;
  (void)removed;
  (void)added;
  GnTimelineView *self = GN_TIMELINE_VIEW(user_data);
  if (!GN_IS_TIMELINE_VIEW(self)) return;

  /* Update empty state */
  guint n_items = g_list_model_get_n_items(model);
  gboolean is_empty = (n_items == 0);

  gtk_widget_set_visible(self->empty_page, is_empty);
  gtk_widget_set_visible(self->scrolled_window, !is_empty);
}

static void on_model_pending(GnTimelineModel *model, guint count, gpointer user_data) {
  (void)model;
  GnTimelineView *self = GN_TIMELINE_VIEW(user_data);
  if (!GN_IS_TIMELINE_VIEW(self)) return;

  if (count > 0 && !self->user_at_top) {
    gn_timeline_view_show_new_notes_indicator(self, count);
  } else {
    gn_timeline_view_hide_new_notes_indicator(self);
  }
}

/**
 * on_reveal_complete_scroll_to_top:
 * @model: The timeline model (unused)
 * @user_data: The GnTimelineView
 *
 * Phase 3 (nostrc-0hp): Completion callback for animated reveal.
 * Scrolls to top only AFTER all items have been revealed.
 */
static void on_reveal_complete_scroll_to_top(gpointer model, gpointer user_data) {
  (void)model;
  GnTimelineView *self = GN_TIMELINE_VIEW(user_data);
  if (!GN_IS_TIMELINE_VIEW(self)) return;

  g_debug("[TIMELINE-VIEW] Reveal complete, scrolling to top");
  gn_timeline_view_scroll_to_top(self);
}

static void on_new_notes_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GnTimelineView *self = GN_TIMELINE_VIEW(user_data);
  if (!GN_IS_TIMELINE_VIEW(self)) return;

  /*
   * Phase 3 (nostrc-0hp): Use animated flush for smooth UX.
   *
   * Instead of calling flush_pending() which just clears the unseen count,
   * we call flush_pending_animated() which:
   * 1. Moves all pending items to a reveal queue
   * 2. Animates them in with 50ms stagger between batches
   * 3. Calls our completion callback to scroll to top AFTER reveal finishes
   *
   * This prevents the jarring "dump all items at once" behavior.
   */
  if (self->model) {
    gn_timeline_model_flush_pending_animated(
      self->model,
      on_reveal_complete_scroll_to_top,
      self
    );
  }

  /* Hide the indicator immediately - animation is starting */
  gn_timeline_view_hide_new_notes_indicator(self);
}

/* ============== Public API ============== */

GnTimelineView *gn_timeline_view_new(void) {
  return g_object_new(GN_TYPE_TIMELINE_VIEW, NULL);
}

GnTimelineView *gn_timeline_view_new_with_model(GnTimelineModel *model) {
  GnTimelineView *self = gn_timeline_view_new();
  gn_timeline_view_set_model(self, model);
  return self;
}

void gn_timeline_view_set_model(GnTimelineView *self, GnTimelineModel *model) {
  g_return_if_fail(GN_IS_TIMELINE_VIEW(self));

  if (self->model == model) return;

  /* Disconnect old model */
  if (self->model) {
    /* nostrc-0hp: Disconnect view widget for frame-aware batching */
    gn_timeline_model_set_view_widget(self->model, NULL);

    if (self->model_items_changed_id > 0) {
      g_signal_handler_disconnect(self->model, self->model_items_changed_id);
      self->model_items_changed_id = 0;
    }
    if (self->model_pending_id > 0) {
      g_signal_handler_disconnect(self->model, self->model_pending_id);
      self->model_pending_id = 0;
    }
    g_clear_object(&self->model);
  }

  /* Connect new model */
  if (model) {
    self->model = g_object_ref(model);

    self->model_items_changed_id = g_signal_connect(
      model, "items-changed",
      G_CALLBACK(on_model_items_changed), self);

    self->model_pending_id = g_signal_connect(
      model, "new-items-pending",
      G_CALLBACK(on_model_pending), self);

    /* Update selection model */
    if (self->selection_model) {
      g_object_unref(self->selection_model);
    }
    self->selection_model = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(model)));
    gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);

    /* Sync initial state */
    gn_timeline_model_set_user_at_top(model, self->user_at_top);

    /*
     * nostrc-0hp Phase 1: Enable frame-aware batching.
     * Use the list_view as the tick widget since it's the main
     * widget that will benefit from frame-synchronized updates.
     */
    gn_timeline_model_set_view_widget(model, self->list_view);

    /* Update empty state */
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(model));
    gtk_widget_set_visible(self->empty_page, n_items == 0);
    gtk_widget_set_visible(self->scrolled_window, n_items > 0);
  } else {
    gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), NULL);
    gtk_widget_set_visible(self->empty_page, TRUE);
    gtk_widget_set_visible(self->scrolled_window, FALSE);
  }

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MODEL]);
}

GnTimelineModel *gn_timeline_view_get_model(GnTimelineView *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_VIEW(self), NULL);
  return self->model;
}

void gn_timeline_view_scroll_to_top(GnTimelineView *self) {
  g_return_if_fail(GN_IS_TIMELINE_VIEW(self));

  gtk_list_view_scroll_to(GTK_LIST_VIEW(self->list_view), 0,
                          GTK_LIST_SCROLL_FOCUS, NULL);
}

void gn_timeline_view_scroll_to_position(GnTimelineView *self, guint position) {
  g_return_if_fail(GN_IS_TIMELINE_VIEW(self));

  gtk_list_view_scroll_to(GTK_LIST_VIEW(self->list_view), position,
                          GTK_LIST_SCROLL_FOCUS, NULL);
}

gboolean gn_timeline_view_is_at_top(GnTimelineView *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_VIEW(self), TRUE);
  return self->user_at_top;
}

void gn_timeline_view_show_new_notes_indicator(GnTimelineView *self, guint count) {
  g_return_if_fail(GN_IS_TIMELINE_VIEW(self));

  if (count == 0) {
    gn_timeline_view_hide_new_notes_indicator(self);
    return;
  }

  char *text = g_strdup_printf("%u new note%s", count, count == 1 ? "" : "s");
  gtk_label_set_text(GTK_LABEL(self->new_notes_label), text);
  g_free(text);

  gtk_revealer_set_reveal_child(GTK_REVEALER(self->new_notes_revealer), TRUE);
}

void gn_timeline_view_hide_new_notes_indicator(GnTimelineView *self) {
  g_return_if_fail(GN_IS_TIMELINE_VIEW(self));
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->new_notes_revealer), FALSE);
}

void gn_timeline_view_set_loading(GnTimelineView *self, gboolean loading) {
  g_return_if_fail(GN_IS_TIMELINE_VIEW(self));
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->loading_revealer), loading);
  if (loading) {
    gtk_spinner_start(GTK_SPINNER(self->loading_spinner));
  } else {
    gtk_spinner_stop(GTK_SPINNER(self->loading_spinner));
  }
}

void gn_timeline_view_set_empty_message(GnTimelineView *self, const char *message) {
  g_return_if_fail(GN_IS_TIMELINE_VIEW(self));
  gtk_label_set_text(GTK_LABEL(self->empty_label), message ? message : "No notes to display");
}

/* ============== GObject Lifecycle ============== */

static void gn_timeline_view_dispose(GObject *object) {
  GnTimelineView *self = GN_TIMELINE_VIEW(object);

  /* Stop scroll check */
  if (self->scroll_check_id > 0) {
    g_source_remove(self->scroll_check_id);
    self->scroll_check_id = 0;
  }

  /* Disconnect signals */
  if (self->model) {
    /* nostrc-0hp: Disconnect view widget for frame-aware batching */
    gn_timeline_model_set_view_widget(self->model, NULL);

    if (self->model_items_changed_id > 0) {
      g_signal_handler_disconnect(self->model, self->model_items_changed_id);
      self->model_items_changed_id = 0;
    }
    if (self->model_pending_id > 0) {
      g_signal_handler_disconnect(self->model, self->model_pending_id);
      self->model_pending_id = 0;
    }
  }

  /* Clear model */
  g_clear_object(&self->model);
  g_clear_object(&self->selection_model);
  g_clear_object(&self->factory);

  /* Unparent children */
  g_clear_pointer(&self->overlay, gtk_widget_unparent);

  G_OBJECT_CLASS(gn_timeline_view_parent_class)->dispose(object);
}

static void gn_timeline_view_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  GnTimelineView *self = GN_TIMELINE_VIEW(object);

  switch (prop_id) {
    case PROP_MODEL:
      g_value_set_object(value, self->model);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gn_timeline_view_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  GnTimelineView *self = GN_TIMELINE_VIEW(object);

  switch (prop_id) {
    case PROP_MODEL:
      gn_timeline_view_set_model(self, g_value_get_object(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gn_timeline_view_class_init(GnTimelineViewClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gn_timeline_view_dispose;
  object_class->get_property = gn_timeline_view_get_property;
  object_class->set_property = gn_timeline_view_set_property;

  properties[PROP_MODEL] =
    g_param_spec_object("model", "Model", "The timeline model",
                        GN_TYPE_TIMELINE_MODEL,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  signals[SIGNAL_ACTIVATE] =
    g_signal_new("activate",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, GN_TYPE_NOSTR_EVENT_ITEM);

  signals[SIGNAL_SHOW_PROFILE] =
    g_signal_new("show-profile",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_SHOW_THREAD] =
    g_signal_new("show-thread",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_NEED_PROFILE] =
    g_signal_new("need-profile",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  /* Layout manager */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void gn_timeline_view_init(GnTimelineView *self) {
  self->user_at_top = TRUE;

  /* Create overlay container */
  self->overlay = gtk_overlay_new();
  gtk_widget_set_parent(self->overlay, GTK_WIDGET(self));

  /* Create scrolled window */
  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(self->scrolled_window, TRUE);
  gtk_widget_set_hexpand(self->scrolled_window, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(self->overlay), self->scrolled_window);

  /* Create factory */
  self->factory = gtk_signal_list_item_factory_new();
  g_signal_connect(self->factory, "setup", G_CALLBACK(factory_setup_cb), self);
  g_signal_connect(self->factory, "bind", G_CALLBACK(factory_bind_cb), self);
  g_signal_connect(self->factory, "unbind", G_CALLBACK(factory_unbind_cb), self);

  /* Create list view */
  self->list_view = gtk_list_view_new(NULL, self->factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(self->list_view), FALSE);
  gtk_widget_add_css_class(self->list_view, "timeline-list");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window), self->list_view);

  /* Create empty page */
  self->empty_page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->empty_page), "mail-inbox-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(self->empty_page), "No Notes");
  adw_status_page_set_description(ADW_STATUS_PAGE(self->empty_page), "Notes will appear here");
  gtk_widget_set_visible(self->empty_page, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->empty_page);

  /* Create new notes revealer */
  self->new_notes_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(self->new_notes_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_widget_set_halign(self->new_notes_revealer, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->new_notes_revealer, GTK_ALIGN_START);
  gtk_widget_set_margin_top(self->new_notes_revealer, 12);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->new_notes_revealer);

  self->new_notes_button = gtk_button_new();
  gtk_widget_add_css_class(self->new_notes_button, "pill");
  gtk_widget_add_css_class(self->new_notes_button, "suggested-action");
  g_signal_connect(self->new_notes_button, "clicked", G_CALLBACK(on_new_notes_clicked), self);
  gtk_revealer_set_child(GTK_REVEALER(self->new_notes_revealer), self->new_notes_button);

  self->new_notes_label = gtk_label_new("New notes");
  gtk_button_set_child(GTK_BUTTON(self->new_notes_button), self->new_notes_label);

  /* Create loading revealer */
  self->loading_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(self->loading_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_widget_set_halign(self->loading_revealer, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->loading_revealer, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(self->loading_revealer, 12);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->loading_revealer);

  self->loading_spinner = gtk_spinner_new();
  gtk_revealer_set_child(GTK_REVEALER(self->loading_revealer), self->loading_spinner);

  /* Connect scroll adjustment */
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  self->vadjustment_changed_id = g_signal_connect(adj, "value-changed",
                                                   G_CALLBACK(on_vadjustment_changed), self);

  /* Start scroll position check */
  self->scroll_check_id = g_timeout_add(100, check_scroll_position, self);
}
