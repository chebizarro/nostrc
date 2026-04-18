/*
 * gnostr-calendar-events-view.c - NIP-52 Calendar Events View
 *
 * Subscribes to kinds 31922/31923 via GnNostrEventModel, parses each event
 * with gnostr_nip52_calendar_event_parse(), caches the result, and displays
 * them in a GtkListView using GnostrCalendarEventCard via a signal factory.
 *
 * Pipeline: GnNostrEventModel → GtkFilterListModel → GtkSortListModel → ListView
 */

#define G_LOG_DOMAIN "gnostr-calendar-events-view"

#include "gnostr-calendar-events-view.h"
#include "gnostr-calendar-event-card.h"
#include "../model/gn-nostr-event-model.h"
#include "../model/gn-nostr-event-item.h"
#include "../util/nip52_calendar.h"
#include <nostr-gobject-1.0/gn-timeline-query.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <glib/gi18n.h>

/* This must match the compiled resource path for the blueprint template */
#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-calendar-events-view.ui"

/* Maximum events to query from NDB */
#define CALENDAR_QUERY_LIMIT 200

enum {
  SIGNAL_OPEN_PROFILE,
  SIGNAL_OPEN_EVENT,
  SIGNAL_RSVP_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _GnostrCalendarEventsView {
  GtkWidget parent_instance;

  /* Template children */
  GtkBox *root;
  GtkLabel *lbl_count;
  GtkToggleButton *btn_all;
  GtkToggleButton *btn_upcoming;
  GtkToggleButton *btn_ongoing;
  GtkToggleButton *btn_past;
  GtkStack *content_stack;
  GtkListView *events_list;
  GtkSpinner *loading_spinner;

  /* Model pipeline */
  GnNostrEventModel *event_model;       /* Source: kinds 31922 + 31923 */
  GtkFilterListModel *filter_model;
  GtkSortListModel *sort_model;
  GtkCustomFilter *custom_filter;
  GtkCustomSorter *custom_sorter;

  /* Parse cache: note_key (uint64) → GnostrNip52CalendarEvent* */
  GHashTable *parse_cache;

  /* State */
  GnostrCalendarEventsFilter active_filter;
  gboolean logged_in;
  gulong items_changed_id;
};

G_DEFINE_FINAL_TYPE(GnostrCalendarEventsView, gnostr_calendar_events_view, GTK_TYPE_WIDGET)

/* --- Parse cache helpers --- */

static void
parse_cache_value_free(gpointer data)
{
  gnostr_nip52_calendar_event_free(data);
}

static GnostrNip52CalendarEvent *
get_parsed_event(GnostrCalendarEventsView *self, GnNostrEventItem *item)
{
  uint64_t note_key = gn_nostr_event_item_get_note_key(item);
  GnostrNip52CalendarEvent *parsed = g_hash_table_lookup(self->parse_cache,
                                                          GUINT_TO_POINTER((guint)note_key));
  if (parsed)
    return parsed;

  /* Fetch JSON from NDB and parse */
  char *json = NULL;
  int json_len = 0;
  if (storage_ndb_get_note_json_by_key(note_key, &json, &json_len) != 0 || !json)
    return NULL;

  parsed = gnostr_nip52_calendar_event_parse(json);
  free(json);

  if (!parsed)
    return NULL;

  g_hash_table_insert(self->parse_cache,
                       GUINT_TO_POINTER((guint)note_key),
                       parsed);
  return parsed;
}

/* --- Filter function --- */

static gboolean
calendar_filter_func(gpointer item, gpointer user_data)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(user_data);
  GnNostrEventItem *event_item = GN_NOSTR_EVENT_ITEM(item);

  if (self->active_filter == GNOSTR_CALENDAR_FILTER_ALL)
    return TRUE;

  GnostrNip52CalendarEvent *parsed = get_parsed_event(self, event_item);
  if (!parsed)
    return FALSE;

  switch (self->active_filter) {
  case GNOSTR_CALENDAR_FILTER_UPCOMING:
    return gnostr_nip52_event_is_upcoming(parsed);
  case GNOSTR_CALENDAR_FILTER_ONGOING:
    return gnostr_nip52_event_is_ongoing(parsed);
  case GNOSTR_CALENDAR_FILTER_PAST:
    return gnostr_nip52_event_is_past(parsed);
  default:
    return TRUE;
  }
}

/* --- Sort function: upcoming events first (ascending start time) --- */

static int
calendar_sort_func(gconstpointer item_a, gconstpointer item_b, gpointer user_data)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(user_data);
  GnNostrEventItem *a = GN_NOSTR_EVENT_ITEM((gpointer)item_a);
  GnNostrEventItem *b = GN_NOSTR_EVENT_ITEM((gpointer)item_b);

  GnostrNip52CalendarEvent *pa = get_parsed_event(self, a);
  GnostrNip52CalendarEvent *pb = get_parsed_event(self, b);

  /* Unparseable events sort to the end */
  if (!pa && !pb) return 0;
  if (!pa) return 1;
  if (!pb) return -1;

  /* Sort by start time ascending (soonest first) */
  if (pa->start < pb->start) return -1;
  if (pa->start > pb->start) return 1;
  return 0;
}

/* --- Update UI state --- */

static void
update_count_and_state(GnostrCalendarEventsView *self)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->sort_model));

  if (n > 0) {
    g_autofree char *count_str = g_strdup_printf("%u", n);
    gtk_label_set_label(self->lbl_count, count_str);
    gtk_stack_set_visible_child_name(self->content_stack, "results");
  } else {
    gtk_label_set_label(self->lbl_count, "");
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
  }
}

/* --- items-changed on the source model → refresh view --- */

static void
on_items_changed(GListModel *model G_GNUC_UNUSED,
                 guint position G_GNUC_UNUSED,
                 guint removed G_GNUC_UNUSED,
                 guint added G_GNUC_UNUSED,
                 gpointer user_data)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(user_data);

  /* Hide loading once we have data */
  if (g_list_model_get_n_items(G_LIST_MODEL(self->event_model)) > 0) {
    gtk_spinner_set_spinning(self->loading_spinner, FALSE);
  }

  update_count_and_state(self);
}

/* --- Filter pill toggled --- */

static void
on_filter_toggled(GtkToggleButton *button, gpointer user_data)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(user_data);

  if (!gtk_toggle_button_get_active(button))
    return;

  GnostrCalendarEventsFilter new_filter;
  if (button == self->btn_upcoming)
    new_filter = GNOSTR_CALENDAR_FILTER_UPCOMING;
  else if (button == self->btn_ongoing)
    new_filter = GNOSTR_CALENDAR_FILTER_ONGOING;
  else if (button == self->btn_past)
    new_filter = GNOSTR_CALENDAR_FILTER_PAST;
  else
    new_filter = GNOSTR_CALENDAR_FILTER_ALL;

  if (self->active_filter == new_filter)
    return;

  self->active_filter = new_filter;
  gtk_filter_changed(GTK_FILTER(self->custom_filter), GTK_FILTER_CHANGE_DIFFERENT);
  update_count_and_state(self);
}

/* --- Factory callbacks --- */

static void
on_card_open_profile(GnostrCalendarEventCard *card G_GNUC_UNUSED,
                     const char *pubkey_hex,
                     gpointer user_data)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(user_data);
  g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
}

static void
on_card_open_event(GnostrCalendarEventCard *card G_GNUC_UNUSED,
                   const char *event_id_hex,
                   gpointer user_data)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(user_data);
  g_signal_emit(self, signals[SIGNAL_OPEN_EVENT], 0, event_id_hex);
}

static void
on_card_rsvp_requested(GnostrCalendarEventCard *card G_GNUC_UNUSED,
                       const char *event_id,
                       const char *d_tag,
                       const char *pubkey_hex,
                       gpointer user_data)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(user_data);
  g_signal_emit(self, signals[SIGNAL_RSVP_REQUESTED], 0, event_id, d_tag, pubkey_hex);
}

static void
factory_setup(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
              GtkListItem *list_item,
              gpointer user_data G_GNUC_UNUSED)
{
  GnostrCalendarEventCard *card = gnostr_calendar_event_card_new();
  gtk_list_item_set_child(list_item, GTK_WIDGET(card));
}

static void
factory_bind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
             GtkListItem *list_item,
             gpointer user_data)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(user_data);
  GnNostrEventItem *item = gtk_list_item_get_item(list_item);
  GnostrCalendarEventCard *card = GNOSTR_CALENDAR_EVENT_CARD(gtk_list_item_get_child(list_item));

  if (!item || !card)
    return;

  GnostrNip52CalendarEvent *parsed = get_parsed_event(self, item);
  if (parsed) {
    gnostr_calendar_event_card_set_event(card, parsed);
    gnostr_calendar_event_card_set_logged_in(card, self->logged_in);
  }

  /* Connect card signals */
  g_signal_connect(card, "open-profile", G_CALLBACK(on_card_open_profile), self);
  g_signal_connect(card, "open-event", G_CALLBACK(on_card_open_event), self);
  g_signal_connect(card, "rsvp-requested", G_CALLBACK(on_card_rsvp_requested), self);
}

static void
factory_unbind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
               GtkListItem *list_item,
               gpointer user_data G_GNUC_UNUSED)
{
  GnostrCalendarEventCard *card = GNOSTR_CALENDAR_EVENT_CARD(gtk_list_item_get_child(list_item));
  if (card) {
    g_signal_handlers_disconnect_by_data(card, user_data);
  }
}

/* --- Build subscription model --- */

static void
build_model(GnostrCalendarEventsView *self)
{
  /* Build query for calendar event kinds */
  GNostrTimelineQueryBuilder *builder = gnostr_timeline_query_builder_new();
  gnostr_timeline_query_builder_add_kind(builder, NIP52_KIND_DATE_BASED_EVENT);
  gnostr_timeline_query_builder_add_kind(builder, NIP52_KIND_TIME_BASED_EVENT);
  gnostr_timeline_query_builder_set_limit(builder, CALENDAR_QUERY_LIMIT);
  GNostrTimelineQuery *query = gnostr_timeline_query_builder_build(builder);
  /* build() consumes and frees the builder — do NOT call builder_free() */

  /* Source model */
  self->event_model = gn_nostr_event_model_new_with_query(query);
  gnostr_timeline_query_free(query);

  /* Filter */
  self->custom_filter = gtk_custom_filter_new(calendar_filter_func, self, NULL);
  self->filter_model = gtk_filter_list_model_new(
      G_LIST_MODEL(g_object_ref(self->event_model)),
      GTK_FILTER(g_object_ref(self->custom_filter)));

  /* Sort */
  self->custom_sorter = gtk_custom_sorter_new(calendar_sort_func, self, NULL);
  self->sort_model = gtk_sort_list_model_new(
      G_LIST_MODEL(g_object_ref(self->filter_model)),
      GTK_SORTER(g_object_ref(self->custom_sorter)));

  /* Factory */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup), self);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind), self);

  /* Selection model (no selection needed, just passthrough) */
  GtkNoSelection *selection = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(self->sort_model)));
  gtk_list_view_set_model(self->events_list, GTK_SELECTION_MODEL(selection));
  gtk_list_view_set_factory(self->events_list, GTK_LIST_ITEM_FACTORY(factory));
  g_object_unref(selection);
  g_object_unref(factory);

  /* Track items changes for UI state */
  self->items_changed_id = g_signal_connect(self->event_model, "items-changed",
                                             G_CALLBACK(on_items_changed), self);

  /* Start in loading state */
  gtk_spinner_set_spinning(self->loading_spinner, TRUE);
  gtk_stack_set_visible_child_name(self->content_stack, "loading");
}

/* --- GObject lifecycle --- */

static void
gnostr_calendar_events_view_dispose(GObject *obj)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(obj);

  if (self->event_model && self->items_changed_id) {
    g_signal_handler_disconnect(self->event_model, self->items_changed_id);
    self->items_changed_id = 0;
  }

  /* Unparent template root */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child)
    gtk_widget_unparent(child);

  g_clear_object(&self->sort_model);
  g_clear_object(&self->filter_model);
  g_clear_object(&self->custom_filter);
  g_clear_object(&self->custom_sorter);
  g_clear_object(&self->event_model);

  G_OBJECT_CLASS(gnostr_calendar_events_view_parent_class)->dispose(obj);
}

static void
gnostr_calendar_events_view_finalize(GObject *obj)
{
  GnostrCalendarEventsView *self = GNOSTR_CALENDAR_EVENTS_VIEW(obj);

  g_clear_pointer(&self->parse_cache, g_hash_table_unref);

  G_OBJECT_CLASS(gnostr_calendar_events_view_parent_class)->finalize(obj);
}

static void
gnostr_calendar_events_view_class_init(GnostrCalendarEventsViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_calendar_events_view_dispose;
  object_class->finalize = gnostr_calendar_events_view_finalize;

  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, GnostrCalendarEventsView, root);
  gtk_widget_class_bind_template_child(widget_class, GnostrCalendarEventsView, lbl_count);
  gtk_widget_class_bind_template_child(widget_class, GnostrCalendarEventsView, btn_all);
  gtk_widget_class_bind_template_child(widget_class, GnostrCalendarEventsView, btn_upcoming);
  gtk_widget_class_bind_template_child(widget_class, GnostrCalendarEventsView, btn_ongoing);
  gtk_widget_class_bind_template_child(widget_class, GnostrCalendarEventsView, btn_past);
  gtk_widget_class_bind_template_child(widget_class, GnostrCalendarEventsView, content_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrCalendarEventsView, events_list);
  gtk_widget_class_bind_template_child(widget_class, GnostrCalendarEventsView, loading_spinner);

  /* Signals */
  signals[SIGNAL_OPEN_PROFILE] =
    g_signal_new("open-profile",
                  G_TYPE_FROM_CLASS(klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_EVENT] =
    g_signal_new("open-event",
                  G_TYPE_FROM_CLASS(klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_RSVP_REQUESTED] =
    g_signal_new("rsvp-requested",
                  G_TYPE_FROM_CLASS(klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

static void
gnostr_calendar_events_view_init(GnostrCalendarEventsView *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  self->active_filter = GNOSTR_CALENDAR_FILTER_ALL;
  self->parse_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             NULL, parse_cache_value_free);

  /* Connect filter pill signals */
  g_signal_connect(self->btn_all, "toggled", G_CALLBACK(on_filter_toggled), self);
  g_signal_connect(self->btn_upcoming, "toggled", G_CALLBACK(on_filter_toggled), self);
  g_signal_connect(self->btn_ongoing, "toggled", G_CALLBACK(on_filter_toggled), self);
  g_signal_connect(self->btn_past, "toggled", G_CALLBACK(on_filter_toggled), self);

  /* Build the model pipeline and wire up the ListView */
  build_model(self);
}

/* --- Public API --- */

GnostrCalendarEventsView *
gnostr_calendar_events_view_new(void)
{
  return g_object_new(GNOSTR_TYPE_CALENDAR_EVENTS_VIEW, NULL);
}

void
gnostr_calendar_events_view_set_filter(GnostrCalendarEventsView *self,
                                        GnostrCalendarEventsFilter filter)
{
  g_return_if_fail(GNOSTR_IS_CALENDAR_EVENTS_VIEW(self));

  if (self->active_filter == filter)
    return;

  self->active_filter = filter;

  /* Sync toggle button state */
  switch (filter) {
  case GNOSTR_CALENDAR_FILTER_UPCOMING:
    gtk_toggle_button_set_active(self->btn_upcoming, TRUE);
    break;
  case GNOSTR_CALENDAR_FILTER_ONGOING:
    gtk_toggle_button_set_active(self->btn_ongoing, TRUE);
    break;
  case GNOSTR_CALENDAR_FILTER_PAST:
    gtk_toggle_button_set_active(self->btn_past, TRUE);
    break;
  default:
    gtk_toggle_button_set_active(self->btn_all, TRUE);
    break;
  }
}

GnostrCalendarEventsFilter
gnostr_calendar_events_view_get_filter(GnostrCalendarEventsView *self)
{
  g_return_val_if_fail(GNOSTR_IS_CALENDAR_EVENTS_VIEW(self), GNOSTR_CALENDAR_FILTER_ALL);
  return self->active_filter;
}

void
gnostr_calendar_events_view_set_logged_in(GnostrCalendarEventsView *self,
                                           gboolean logged_in)
{
  g_return_if_fail(GNOSTR_IS_CALENDAR_EVENTS_VIEW(self));
  self->logged_in = logged_in;
}

guint
gnostr_calendar_events_view_get_event_count(GnostrCalendarEventsView *self)
{
  g_return_val_if_fail(GNOSTR_IS_CALENDAR_EVENTS_VIEW(self), 0);
  return g_list_model_get_n_items(G_LIST_MODEL(self->sort_model));
}
