/* events-page.c - Recent Events page showing signed event history
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "events-page.h"
#include "app-resources.h"

/* --- EventItem GObject --- */

struct _EventItem {
  GObject parent_instance;

  char   *event_id;
  guint32 event_kind;
  gint64  timestamp;
};

G_DEFINE_TYPE(EventItem, event_item, G_TYPE_OBJECT)

enum {
  PROP_EVENT_0,
  PROP_EVENT_ID,
  PROP_EVENT_KIND,
  PROP_EVENT_TIMESTAMP,
  N_EVENT_PROPS
};

static GParamSpec *event_props[N_EVENT_PROPS];

static void
event_item_finalize(GObject *object)
{
  EventItem *self = EVENT_ITEM(object);
  g_free(self->event_id);
  G_OBJECT_CLASS(event_item_parent_class)->finalize(object);
}

static void
event_item_get_property(GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  EventItem *self = EVENT_ITEM(object);

  switch (prop_id) {
    case PROP_EVENT_ID:
      g_value_set_string(value, self->event_id);
      break;
    case PROP_EVENT_KIND:
      g_value_set_uint(value, self->event_kind);
      break;
    case PROP_EVENT_TIMESTAMP:
      g_value_set_int64(value, self->timestamp);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
event_item_set_property(GObject      *object,
                        guint         prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  EventItem *self = EVENT_ITEM(object);

  switch (prop_id) {
    case PROP_EVENT_ID:
      g_free(self->event_id);
      self->event_id = g_value_dup_string(value);
      break;
    case PROP_EVENT_KIND:
      self->event_kind = g_value_get_uint(value);
      break;
    case PROP_EVENT_TIMESTAMP:
      self->timestamp = g_value_get_int64(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
event_item_class_init(EventItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->finalize = event_item_finalize;
  object_class->get_property = event_item_get_property;
  object_class->set_property = event_item_set_property;

  event_props[PROP_EVENT_ID] =
    g_param_spec_string("event-id", NULL, NULL, NULL,
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  event_props[PROP_EVENT_KIND] =
    g_param_spec_uint("event-kind", NULL, NULL, 0, G_MAXUINT32, 0,
                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  event_props[PROP_EVENT_TIMESTAMP] =
    g_param_spec_int64("timestamp", NULL, NULL, G_MININT64, G_MAXINT64, 0,
                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_EVENT_PROPS, event_props);
}

static void
event_item_init(EventItem *self)
{
  self->event_id = NULL;
  self->event_kind = 0;
  self->timestamp = 0;
}

EventItem *
event_item_new(const char *event_id,
               guint32     event_kind,
               gint64      timestamp)
{
  return g_object_new(TYPE_EVENT_ITEM,
                      "event-id", event_id,
                      "event-kind", event_kind,
                      "timestamp", timestamp,
                      NULL);
}

const char *
event_item_get_event_id(EventItem *self)
{
  g_return_val_if_fail(EVENT_IS_ITEM(self), NULL);
  return self->event_id;
}

guint32
event_item_get_event_kind(EventItem *self)
{
  g_return_val_if_fail(EVENT_IS_ITEM(self), 0);
  return self->event_kind;
}

gint64
event_item_get_timestamp(EventItem *self)
{
  g_return_val_if_fail(EVENT_IS_ITEM(self), 0);
  return self->timestamp;
}

/* --- EventsPage widget --- */

struct _EventsPage {
  AdwBin parent_instance;

  /* Template children */
  GtkStack      *stack;
  GtkListView   *list_view;
  AdwStatusPage *empty_state;
  GtkButton     *btn_view_full_log;
  GtkButton     *btn_approve;

  /* Data model */
  GListStore          *event_store;
  GtkSingleSelection  *selection_model;
};

G_DEFINE_TYPE(EventsPage, events_page, ADW_TYPE_BIN)

/* Signal IDs */
enum {
  SIGNAL_EVENT_ACTIVATED,
  SIGNAL_VIEW_FULL_LOG,
  SIGNAL_APPROVE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/**
 * get_event_kind_name:
 * @kind: the event kind number
 *
 * Returns a human-readable name for the event kind.
 */
static const char *
get_event_kind_name(guint32 kind)
{
  switch (kind) {
    case 0:     return "Metadata";
    case 1:     return "Text Note";
    case 2:     return "Recommend Server";
    case 3:     return "Contact List";
    case 4:     return "Encrypted DM";
    case 5:     return "Delete";
    case 6:     return "Repost";
    case 7:     return "Reaction";
    case 8:     return "Badge Award";
    case 9:     return "Group Chat";
    case 10:    return "Group Chat Threaded";
    case 11:    return "Group Thread";
    case 12:    return "Group Thread Reply";
    case 13:    return "Seal";
    case 14:    return "Direct Message";
    case 16:    return "Generic Repost";
    case 17:    return "Reaction to Website";
    case 40:    return "Channel Creation";
    case 41:    return "Channel Metadata";
    case 42:    return "Channel Message";
    case 43:    return "Channel Hide Message";
    case 44:    return "Channel Mute User";
    case 1021:  return "Bid";
    case 1022:  return "Bid Confirmation";
    case 1040:  return "OpenTimestamps";
    case 1059:  return "Gift Wrap";
    case 1063:  return "File Metadata";
    case 1311:  return "Live Chat";
    case 1617:  return "Patches";
    case 1621:  return "Issues";
    case 1622:  return "Replies";
    case 1971:  return "Problem Tracker";
    case 1984:  return "Report";
    case 1985:  return "Label";
    case 4550:  return "Community Post";
    case 5000:  return "Job Request";
    case 6000:  return "Job Result";
    case 7000:  return "Job Feedback";
    case 9041:  return "Zap Goal";
    case 9734:  return "Zap Request";
    case 9735:  return "Zap";
    case 10000: return "Mute List";
    case 10001: return "Pin List";
    case 10002: return "Relay List";
    case 10003: return "Bookmark List";
    case 10004: return "Community List";
    case 10005: return "Public Chat List";
    case 10006: return "Blocked Relay List";
    case 10007: return "Search Relay List";
    case 10009: return "User Groups";
    case 10015: return "Interest List";
    case 10030: return "User Emoji List";
    case 10050: return "DM Relay List";
    case 10096: return "File Storage Server List";
    case 13194: return "Wallet Info";
    case 21000: return "Lightning Pub RPC";
    case 22242: return "Client Auth";
    case 23194: return "Wallet Request";
    case 23195: return "Wallet Response";
    case 24133: return "Nostr Connect";
    case 27235: return "HTTP Auth";
    case 30000: return "Profile Badges";
    case 30001: return "Categorized Bookmarks";
    case 30002: return "Relay Sets";
    case 30003: return "Bookmark Sets";
    case 30004: return "Curations";
    case 30005: return "Video Sets";
    case 30008: return "Badge Definition";
    case 30009: return "Badge Award";
    case 30015: return "Interest Set";
    case 30017: return "Stall Definition";
    case 30018: return "Product Definition";
    case 30019: return "Marketplace UI";
    case 30020: return "Product Sold";
    case 30023: return "Long-form Content";
    case 30024: return "Draft Long-form";
    case 30030: return "Emoji Set";
    case 30063: return "Release Artifact";
    case 30078: return "App Specific Data";
    case 30311: return "Live Event";
    case 30315: return "User Status";
    case 30402: return "Classified Listing";
    case 30403: return "Draft Classified";
    case 30617: return "Repository Announcement";
    case 30618: return "Repository State";
    case 30818: return "Wiki";
    case 30819: return "Redirects";
    case 31890: return "Handler Recommendation";
    case 31922: return "Date Calendar";
    case 31923: return "Time Calendar";
    case 31924: return "Calendar";
    case 31925: return "RSVP";
    case 31989: return "Handler Metadata";
    case 31990: return "Relay Discovery";
    case 34235: return "Video Event";
    case 34236: return "Short Video";
    case 34237: return "Video View";
    case 34550: return "Community";
    default:
      if (kind >= 5000 && kind < 6000)
        return "Job Request";
      if (kind >= 6000 && kind < 7000)
        return "Job Result";
      if (kind >= 7000 && kind < 8000)
        return "Job Feedback";
      if (kind >= 10000 && kind < 20000)
        return "Replaceable";
      if (kind >= 20000 && kind < 30000)
        return "Ephemeral";
      if (kind >= 30000 && kind < 40000)
        return "Parameterized Replaceable";
      return "Unknown";
  }
}

/**
 * get_event_kind_icon:
 * @kind: the event kind number
 *
 * Returns an icon name for the event kind.
 */
static const char *
get_event_kind_icon(guint32 kind)
{
  switch (kind) {
    case 0:     return "avatar-default-symbolic";        /* Metadata */
    case 1:     return "chat-bubble-text-symbolic";      /* Text Note */
    case 3:     return "contact-new-symbolic";           /* Contact List */
    case 4:     return "mail-send-symbolic";             /* Encrypted DM */
    case 5:     return "user-trash-symbolic";            /* Delete */
    case 6:     return "emblem-shared-symbolic";         /* Repost */
    case 7:     return "starred-symbolic";               /* Reaction */
    case 14:    return "mail-symbolic";                  /* Direct Message */
    case 1059:  return "mail-attachment-symbolic";       /* Gift Wrap */
    case 1063:  return "document-open-symbolic";         /* File Metadata */
    case 9734:  return "emblem-synchronizing-symbolic";  /* Zap Request */
    case 9735:  return "star-new-symbolic";              /* Zap */
    case 10002: return "network-server-symbolic";        /* Relay List */
    case 22242: return "system-lock-screen-symbolic";    /* Client Auth */
    case 24133: return "network-wireless-encrypted-symbolic"; /* Nostr Connect */
    case 27235: return "system-lock-screen-symbolic";    /* HTTP Auth */
    case 30023: return "accessories-text-editor-symbolic"; /* Long-form */
    default:    return "document-send-symbolic";         /* Default */
  }
}

/**
 * format_relative_time:
 * @timestamp: Unix timestamp
 *
 * Formats a timestamp as a relative time string (e.g., "2 minutes ago").
 */
static char *
format_relative_time(gint64 timestamp)
{
  GDateTime *event_time = g_date_time_new_from_unix_local(timestamp);
  if (!event_time)
    return g_strdup("Unknown time");

  GDateTime *now = g_date_time_new_now_local();
  GTimeSpan diff = g_date_time_difference(now, event_time);

  g_date_time_unref(now);
  g_date_time_unref(event_time);

  gint64 seconds = diff / G_TIME_SPAN_SECOND;
  gint64 minutes = diff / G_TIME_SPAN_MINUTE;
  gint64 hours = diff / G_TIME_SPAN_HOUR;
  gint64 days = diff / G_TIME_SPAN_DAY;

  if (seconds < 60)
    return g_strdup("Just now");
  else if (minutes < 60)
    return g_strdup_printf("%ld minute%s ago", (long)minutes, minutes == 1 ? "" : "s");
  else if (hours < 24)
    return g_strdup_printf("%ld hour%s ago", (long)hours, hours == 1 ? "" : "s");
  else if (days < 7)
    return g_strdup_printf("%ld day%s ago", (long)days, days == 1 ? "" : "s");
  else {
    GDateTime *dt = g_date_time_new_from_unix_local(timestamp);
    char *formatted = g_date_time_format(dt, "%b %d, %Y");
    g_date_time_unref(dt);
    return formatted;
  }
}

/**
 * truncate_event_id:
 * @event_id: the full event ID
 *
 * Returns a truncated version of the event ID for display.
 */
static char *
truncate_event_id(const char *event_id)
{
  if (!event_id || strlen(event_id) < 16)
    return g_strdup(event_id ? event_id : "");

  return g_strdup_printf("%.8s...%.8s",
                         event_id,
                         event_id + strlen(event_id) - 8);
}

/* List item factory callbacks */

static void
setup_event_row(GtkListItemFactory *factory,
                GtkListItem        *list_item,
                gpointer            user_data)
{
  (void)factory;
  (void)user_data;

  /* Create the row widget structure */
  GtkBox *row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
  gtk_widget_set_margin_start(GTK_WIDGET(row), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(row), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(row), 8);
  gtk_widget_set_margin_bottom(GTK_WIDGET(row), 8);

  /* Icon */
  GtkImage *icon = GTK_IMAGE(gtk_image_new());
  gtk_image_set_icon_size(icon, GTK_ICON_SIZE_LARGE);
  gtk_widget_add_css_class(GTK_WIDGET(icon), "dim-label");
  gtk_box_append(row, GTK_WIDGET(icon));

  /* Text content box */
  GtkBox *text_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2));
  gtk_widget_set_hexpand(GTK_WIDGET(text_box), TRUE);
  gtk_widget_set_valign(GTK_WIDGET(text_box), GTK_ALIGN_CENTER);

  /* Event type label */
  GtkLabel *type_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(type_label, 0);
  gtk_widget_add_css_class(GTK_WIDGET(type_label), "heading");
  gtk_box_append(text_box, GTK_WIDGET(type_label));

  /* Subtitle box for ID and time */
  GtkBox *subtitle_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));

  /* Event ID label */
  GtkLabel *id_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(id_label, 0);
  gtk_widget_add_css_class(GTK_WIDGET(id_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(id_label), "caption");
  gtk_box_append(subtitle_box, GTK_WIDGET(id_label));

  /* Separator */
  GtkLabel *sep_label = GTK_LABEL(gtk_label_new("-"));
  gtk_widget_add_css_class(GTK_WIDGET(sep_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(sep_label), "caption");
  gtk_box_append(subtitle_box, GTK_WIDGET(sep_label));

  /* Timestamp label */
  GtkLabel *time_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(time_label, 0);
  gtk_widget_add_css_class(GTK_WIDGET(time_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(time_label), "caption");
  gtk_box_append(subtitle_box, GTK_WIDGET(time_label));

  gtk_box_append(text_box, GTK_WIDGET(subtitle_box));
  gtk_box_append(row, GTK_WIDGET(text_box));

  /* Disclosure indicator */
  GtkImage *chevron = GTK_IMAGE(gtk_image_new_from_icon_name("go-next-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(chevron), "dim-label");
  gtk_box_append(row, GTK_WIDGET(chevron));

  gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
bind_event_row(GtkListItemFactory *factory,
               GtkListItem        *list_item,
               gpointer            user_data)
{
  (void)factory;
  (void)user_data;

  EventItem *item = EVENT_ITEM(gtk_list_item_get_item(list_item));
  GtkBox *row = GTK_BOX(gtk_list_item_get_child(list_item));

  /* Get child widgets */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(row));
  GtkImage *icon = GTK_IMAGE(child);

  child = gtk_widget_get_next_sibling(child);
  GtkBox *text_box = GTK_BOX(child);

  GtkWidget *text_child = gtk_widget_get_first_child(GTK_WIDGET(text_box));
  GtkLabel *type_label = GTK_LABEL(text_child);

  text_child = gtk_widget_get_next_sibling(text_child);
  GtkBox *subtitle_box = GTK_BOX(text_child);

  GtkWidget *sub_child = gtk_widget_get_first_child(GTK_WIDGET(subtitle_box));
  GtkLabel *id_label = GTK_LABEL(sub_child);

  sub_child = gtk_widget_get_next_sibling(sub_child);
  sub_child = gtk_widget_get_next_sibling(sub_child); /* Skip separator */
  GtkLabel *time_label = GTK_LABEL(sub_child);

  /* Set values */
  guint32 kind = event_item_get_event_kind(item);
  gtk_image_set_from_icon_name(icon, get_event_kind_icon(kind));
  gtk_label_set_text(type_label, get_event_kind_name(kind));

  char *truncated_id = truncate_event_id(event_item_get_event_id(item));
  gtk_label_set_text(id_label, truncated_id);
  g_free(truncated_id);

  char *time_str = format_relative_time(event_item_get_timestamp(item));
  gtk_label_set_text(time_label, time_str);
  g_free(time_str);
}

static void
unbind_event_row(GtkListItemFactory *factory,
                 GtkListItem        *list_item,
                 gpointer            user_data)
{
  (void)factory;
  (void)list_item;
  (void)user_data;
  /* No cleanup needed */
}

static void
teardown_event_row(GtkListItemFactory *factory,
                   GtkListItem        *list_item,
                   gpointer            user_data)
{
  (void)factory;
  (void)user_data;
  gtk_list_item_set_child(list_item, NULL);
}

/* Row activation handler */

static void
on_list_view_activate(GtkListView *list_view,
                      guint        position,
                      gpointer     user_data)
{
  (void)list_view;

  EventsPage *self = EVENTS_PAGE(user_data);
  EventItem *item = g_list_model_get_item(G_LIST_MODEL(self->event_store), position);

  if (item) {
    g_signal_emit(self, signals[SIGNAL_EVENT_ACTIVATED], 0,
                  event_item_get_event_id(item),
                  event_item_get_event_kind(item),
                  event_item_get_timestamp(item));
    g_object_unref(item);
  }
}

/* Button click handlers */

static void
on_view_full_log(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  EventsPage *self = EVENTS_PAGE(user_data);
  g_signal_emit(self, signals[SIGNAL_VIEW_FULL_LOG], 0);
}

static void
on_approve(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  EventsPage *self = EVENTS_PAGE(user_data);
  g_signal_emit(self, signals[SIGNAL_APPROVE], 0);
}

/* Update stack visibility based on item count */

static void
update_stack_visible_child(EventsPage *self)
{
  guint n_items = g_list_model_get_n_items(G_LIST_MODEL(self->event_store));

  if (n_items == 0) {
    gtk_stack_set_visible_child_name(self->stack, "empty");
  } else {
    gtk_stack_set_visible_child_name(self->stack, "list");
  }
}

static void
on_items_changed(GListModel *model,
                 guint       position,
                 guint       removed,
                 guint       added,
                 gpointer    user_data)
{
  (void)model;
  (void)position;
  (void)removed;
  (void)added;

  EventsPage *self = EVENTS_PAGE(user_data);
  update_stack_visible_child(self);
}

/* GObject lifecycle */

static void
events_page_dispose(GObject *object)
{
  EventsPage *self = EVENTS_PAGE(object);

  g_clear_object(&self->event_store);
  g_clear_object(&self->selection_model);

  gtk_widget_dispose_template(GTK_WIDGET(object), TYPE_EVENTS_PAGE);
  G_OBJECT_CLASS(events_page_parent_class)->dispose(object);
}

static void
events_page_class_init(EventsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = events_page_dispose;

  /* Load template from resource */
  gtk_widget_class_set_template_from_resource(widget_class,
      APP_RESOURCE_PATH "/ui/events-page.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, EventsPage, stack);
  gtk_widget_class_bind_template_child(widget_class, EventsPage, list_view);
  gtk_widget_class_bind_template_child(widget_class, EventsPage, empty_state);
  gtk_widget_class_bind_template_child(widget_class, EventsPage, btn_view_full_log);
  gtk_widget_class_bind_template_child(widget_class, EventsPage, btn_approve);

  /**
   * EventsPage::event-activated:
   * @self: the #EventsPage instance
   * @event_id: the activated event's ID
   * @event_kind: the event kind number
   * @timestamp: the event timestamp
   *
   * Emitted when the user clicks on an event row to view details.
   */
  signals[SIGNAL_EVENT_ACTIVATED] = g_signal_new(
      "event-activated",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL,
      NULL,
      G_TYPE_NONE, 3,
      G_TYPE_STRING,
      G_TYPE_UINT,
      G_TYPE_INT64);

  /**
   * EventsPage::view-full-log:
   * @self: the #EventsPage instance
   *
   * Emitted when the user clicks the "View Full Event Log" link.
   */
  signals[SIGNAL_VIEW_FULL_LOG] = g_signal_new(
      "view-full-log",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL,
      NULL,
      G_TYPE_NONE, 0);

  /**
   * EventsPage::approve:
   * @self: the #EventsPage instance
   *
   * Emitted when the user clicks the "Approve" button.
   */
  signals[SIGNAL_APPROVE] = g_signal_new(
      "approve",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL,
      NULL,
      G_TYPE_NONE, 0);
}

static void
events_page_init(EventsPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Create the event store (in-memory GListStore) */
  self->event_store = g_list_store_new(TYPE_EVENT_ITEM);

  /* Create selection model */
  self->selection_model = gtk_single_selection_new(G_LIST_MODEL(self->event_store));
  gtk_single_selection_set_autoselect(self->selection_model, FALSE);
  gtk_single_selection_set_can_unselect(self->selection_model, TRUE);

  /* Create list item factory */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(setup_event_row), self);
  g_signal_connect(factory, "bind", G_CALLBACK(bind_event_row), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(unbind_event_row), self);
  g_signal_connect(factory, "teardown", G_CALLBACK(teardown_event_row), self);

  /* Set up the list view */
  gtk_list_view_set_model(self->list_view, GTK_SELECTION_MODEL(self->selection_model));
  gtk_list_view_set_factory(self->list_view, factory);
  g_object_unref(factory);

  /* Connect activation signal */
  g_signal_connect(self->list_view, "activate",
                   G_CALLBACK(on_list_view_activate), self);

  /* Connect button signals */
  if (self->btn_view_full_log)
    g_signal_connect(self->btn_view_full_log, "clicked",
                     G_CALLBACK(on_view_full_log), self);
  if (self->btn_approve)
    g_signal_connect(self->btn_approve, "clicked",
                     G_CALLBACK(on_approve), self);

  /* Monitor item count for empty state */
  g_signal_connect(self->event_store, "items-changed",
                   G_CALLBACK(on_items_changed), self);

  /* Initial state */
  update_stack_visible_child(self);
}

EventsPage *
events_page_new(void)
{
  return g_object_new(TYPE_EVENTS_PAGE, NULL);
}

void
events_page_add_event(EventsPage *self,
                      const char *event_id,
                      guint32     event_kind,
                      gint64      timestamp)
{
  g_return_if_fail(EVENTS_IS_PAGE(self));
  g_return_if_fail(event_id != NULL);

  EventItem *item = event_item_new(event_id, event_kind, timestamp);

  /* Insert at the beginning (most recent first) */
  g_list_store_insert(self->event_store, 0, item);
  g_object_unref(item);
}

void
events_page_clear(EventsPage *self)
{
  g_return_if_fail(EVENTS_IS_PAGE(self));

  g_list_store_remove_all(self->event_store);
}

GListStore *
events_page_get_event_store(EventsPage *self)
{
  g_return_val_if_fail(EVENTS_IS_PAGE(self), NULL);

  return self->event_store;
}
