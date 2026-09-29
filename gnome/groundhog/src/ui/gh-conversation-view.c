#include "gh-conversation-view.h"
#include "gh-link-policy.h"
#include "gh-message-row.h"

#include <glib/gi18n.h>

/* Consecutive messages from one sender this close together form a run. */
#define RUN_GAP_SECONDS (5 * 60)
/* Within this many pixels of the bottom counts as at the bottom. Small, so
 * a slight touchpad scroll up is not undone when rows get measured. */
#define EDGE_SLACK 4.0
/* Settings keys (charter §7.11), owned by the schema. */
#define LINK_PREVIEWS_KEY "link-previews"
#define NETWORK_MODE_KEY "network-mode"

/* ---- GhTimelineItem --------------------------------------------------------------- */

struct _GhTimelineItem {
  GObject parent_instance;
  GhMessage *message;
  gint day;        /* local calendar day of created_at, as yyyymmdd */
  gchar *day_label;
  gboolean run_start;
  gboolean run_end;
  gboolean show_sender;
};

enum {
  ITEM_PROP_0,
  ITEM_PROP_MESSAGE,
  ITEM_PROP_RUN_START,
  ITEM_PROP_RUN_END,
  ITEM_PROP_SHOW_SENDER,
  ITEM_PROP_DAY_LABEL,
  ITEM_N_PROPS
};
static GParamSpec *item_props[ITEM_N_PROPS];

G_DEFINE_FINAL_TYPE(GhTimelineItem, gh_timeline_item, G_TYPE_OBJECT)

static gint
day_key(GDateTime *when)
{
  return g_date_time_get_year(when) * 10000 + g_date_time_get_month(when) * 100 +
         g_date_time_get_day_of_month(when);
}

/* Noon of the day, which (unlike midnight) exists on every day. */
static GDateTime *
local_noon(GDateTime *when)
{
  return g_date_time_new_local(g_date_time_get_year(when), g_date_time_get_month(when),
                               g_date_time_get_day_of_month(when), 12, 0, 0);
}

gchar *
gh_conversation_view_format_day(GDateTime *when, GDateTime *now)
{
  g_return_val_if_fail(when != NULL && now != NULL, NULL);
  g_autoptr(GDateTime) local_when = g_date_time_to_local(when);
  g_autoptr(GDateTime) local_now = g_date_time_to_local(now);
  g_autoptr(GDateTime) day = local_noon(local_when);
  g_autoptr(GDateTime) today = local_noon(local_now);
  gint64 days = g_date_time_difference(today, day) / G_TIME_SPAN_DAY;
  if (days == 0)
    return g_strdup(_("Today"));
  if (days == 1)
    return g_strdup(_("Yesterday"));
  if (days > 1 && days < 7)
    return g_date_time_format(local_when, "%A");
  if (g_date_time_get_year(local_when) == g_date_time_get_year(local_now))
    /* TRANSLATORS: a day separator for a date this year, as a
     * g_date_time_format() format, e.g. "Monday, 21 September". */
    return g_date_time_format(local_when, _("%A, %-d %B"));
  /* TRANSLATORS: a day separator for a date in another year, as a
   * g_date_time_format() format, e.g. "21 September 2025". */
  return g_date_time_format(local_when, _("%-d %B %Y"));
}

static GhTimelineItem *
timeline_item_new(GhMessage *message, GDateTime *now)
{
  GhTimelineItem *self = g_object_new(GH_TYPE_TIMELINE_ITEM, NULL);
  self->message = g_object_ref(message);
  g_autoptr(GDateTime) when =
    g_date_time_new_from_unix_local(gh_message_get_created_at(message));
  self->day = when ? day_key(when) : 0;
  self->day_label = when ? gh_conversation_view_format_day(when, now) : g_strdup("");
  self->run_start = TRUE;
  self->run_end = TRUE;
  return self;
}

static void
item_set_flag(GhTimelineItem *self, gboolean *flag, gboolean value, guint prop)
{
  if (*flag == !!value)
    return;
  *flag = !!value;
  g_object_notify_by_pspec(G_OBJECT(self), item_props[prop]);
}

static void
item_refresh_day(GhTimelineItem *self, GDateTime *now)
{
  g_autoptr(GDateTime) when =
    g_date_time_new_from_unix_local(gh_message_get_created_at(self->message));
  g_autofree gchar *label = when ? gh_conversation_view_format_day(when, now) : g_strdup("");
  if (g_strcmp0(label, self->day_label) == 0)
    return;
  g_free(self->day_label);
  self->day_label = g_steal_pointer(&label);
  g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_DAY_LABEL]);
}

GhMessage *
gh_timeline_item_get_message(GhTimelineItem *self)
{
  g_return_val_if_fail(GH_IS_TIMELINE_ITEM(self), NULL);
  return self->message;
}

gboolean
gh_timeline_item_get_run_start(GhTimelineItem *self)
{
  g_return_val_if_fail(GH_IS_TIMELINE_ITEM(self), FALSE);
  return self->run_start;
}

gboolean
gh_timeline_item_get_run_end(GhTimelineItem *self)
{
  g_return_val_if_fail(GH_IS_TIMELINE_ITEM(self), FALSE);
  return self->run_end;
}

gboolean
gh_timeline_item_get_show_sender(GhTimelineItem *self)
{
  g_return_val_if_fail(GH_IS_TIMELINE_ITEM(self), FALSE);
  return self->show_sender;
}

const gchar *
gh_timeline_item_get_day_label(GhTimelineItem *self)
{
  g_return_val_if_fail(GH_IS_TIMELINE_ITEM(self), NULL);
  return self->day_label;
}

static void
gh_timeline_item_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhTimelineItem *self = GH_TIMELINE_ITEM(object);
  switch (id) {
  case ITEM_PROP_MESSAGE:
    g_value_set_object(value, self->message);
    break;
  case ITEM_PROP_RUN_START:
    g_value_set_boolean(value, self->run_start);
    break;
  case ITEM_PROP_RUN_END:
    g_value_set_boolean(value, self->run_end);
    break;
  case ITEM_PROP_SHOW_SENDER:
    g_value_set_boolean(value, self->show_sender);
    break;
  case ITEM_PROP_DAY_LABEL:
    g_value_set_string(value, self->day_label);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_timeline_item_finalize(GObject *object)
{
  GhTimelineItem *self = GH_TIMELINE_ITEM(object);
  g_clear_object(&self->message);
  g_free(self->day_label);
  G_OBJECT_CLASS(gh_timeline_item_parent_class)->finalize(object);
}

static void
gh_timeline_item_class_init(GhTimelineItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_timeline_item_get_property;
  object_class->finalize = gh_timeline_item_finalize;
  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  item_props[ITEM_PROP_MESSAGE] = g_param_spec_object("message", NULL, NULL, GH_TYPE_MESSAGE,
                                                      ro);
  item_props[ITEM_PROP_RUN_START] = g_param_spec_boolean("run-start", NULL, NULL, TRUE, ro);
  item_props[ITEM_PROP_RUN_END] = g_param_spec_boolean("run-end", NULL, NULL, TRUE, ro);
  item_props[ITEM_PROP_SHOW_SENDER] = g_param_spec_boolean("show-sender", NULL, NULL, FALSE,
                                                           ro);
  item_props[ITEM_PROP_DAY_LABEL] = g_param_spec_string("day-label", NULL, NULL, "", ro);
  g_object_class_install_properties(object_class, ITEM_N_PROPS, item_props);
}

static void
gh_timeline_item_init(GhTimelineItem *self)
{
  (void)self;
}

/* ---- GhTimeline: the visible messages as timeline items, sectioned by day ----------- */

#define GH_TYPE_TIMELINE (gh_timeline_get_type())
G_DECLARE_FINAL_TYPE(GhTimeline, gh_timeline, GH, TIMELINE, GObject)

struct _GhTimeline {
  GObject parent_instance;
  GListModel *source; /* GhMessage in conversation order */
  GPtrArray *items;   /* GhTimelineItem, parallel to source */
  gboolean multi_party;
};

static void gh_timeline_list_model_init(GListModelInterface *iface);
static void gh_timeline_section_model_init(GtkSectionModelInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GhTimeline, gh_timeline, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gh_timeline_list_model_init)
  G_IMPLEMENT_INTERFACE(GTK_TYPE_SECTION_MODEL, gh_timeline_section_model_init))

static GType
gh_timeline_get_item_type(GListModel *model)
{
  (void)model;
  return GH_TYPE_TIMELINE_ITEM;
}

static guint
gh_timeline_get_n_items(GListModel *model)
{
  return GH_TIMELINE(model)->items->len;
}

static gpointer
gh_timeline_get_item(GListModel *model, guint position)
{
  GhTimeline *self = GH_TIMELINE(model);
  return position < self->items->len ? g_object_ref(g_ptr_array_index(self->items, position))
                                     : NULL;
}

static void
gh_timeline_list_model_init(GListModelInterface *iface)
{
  iface->get_item_type = gh_timeline_get_item_type;
  iface->get_n_items = gh_timeline_get_n_items;
  iface->get_item = gh_timeline_get_item;
}

static GhTimelineItem *
timeline_at(GhTimeline *self, guint position)
{
  return g_ptr_array_index(self->items, position);
}

/* A section is one local calendar day. */
static void
gh_timeline_get_section(GtkSectionModel *model, guint position, guint *out_start,
                        guint *out_end)
{
  GhTimeline *self = GH_TIMELINE(model);
  guint n = self->items->len;
  if (position >= n) {
    *out_start = n;
    *out_end = G_MAXUINT;
    return;
  }
  gint day = timeline_at(self, position)->day;
  guint start = position;
  while (start > 0 && timeline_at(self, start - 1)->day == day)
    start--;
  guint end = position + 1;
  while (end < n && timeline_at(self, end)->day == day)
    end++;
  *out_start = start;
  *out_end = end;
}

static void
gh_timeline_section_model_init(GtkSectionModelInterface *iface)
{
  iface->get_section = gh_timeline_get_section;
}

static gboolean
same_run(GhTimelineItem *a, GhTimelineItem *b)
{
  gint64 gap = gh_message_get_created_at(b->message) - gh_message_get_created_at(a->message);
  return a->day == b->day && gap >= 0 && gap <= RUN_GAP_SECONDS &&
         g_strcmp0(gh_message_get_sender(a->message), gh_message_get_sender(b->message)) == 0;
}

/* Recomputes the run flags of the items in [from, to). */
static void
timeline_refresh_runs(GhTimeline *self, guint from, guint to)
{
  guint n = self->items->len;
  for (guint i = from; i < MIN(to, n); i++) {
    GhTimelineItem *item = timeline_at(self, i);
    gboolean start = i == 0 || !same_run(timeline_at(self, i - 1), item);
    gboolean end = i + 1 == n || !same_run(item, timeline_at(self, i + 1));
    item_set_flag(item, &item->run_start, start, ITEM_PROP_RUN_START);
    item_set_flag(item, &item->run_end, end, ITEM_PROP_RUN_END);
    item_set_flag(item, &item->show_sender,
                  self->multi_party && start && !gh_message_is_self(item->message),
                  ITEM_PROP_SHOW_SENDER);
  }
}

static void
on_source_changed(GhTimeline *self, guint position, guint removed, guint added,
                  GListModel *source)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_ptr_array_remove_range(self->items, position, removed);
  for (guint i = 0; i < added; i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(source, position + i);
    g_ptr_array_insert(self->items, position + i, timeline_item_new(message, now));
  }
  timeline_refresh_runs(self, position > 0 ? position - 1 : 0, position + added + 1);
  g_list_model_items_changed(G_LIST_MODEL(self), position, removed, added);
}

static GhTimeline *
timeline_new(GListModel *source, gboolean multi_party)
{
  GhTimeline *self = g_object_new(GH_TYPE_TIMELINE, NULL);
  self->source = g_object_ref(source);
  self->multi_party = multi_party;
  g_signal_connect_object(source, "items-changed", G_CALLBACK(on_source_changed), self,
                          G_CONNECT_SWAPPED);
  on_source_changed(self, 0, 0, g_list_model_get_n_items(source), source);
  return self;
}

static void
timeline_refresh_days(GhTimeline *self)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  for (guint i = 0; i < self->items->len; i++)
    item_refresh_day(timeline_at(self, i), now);
}

static void
gh_timeline_dispose(GObject *object)
{
  GhTimeline *self = GH_TIMELINE(object);
  if (self->source)
    g_signal_handlers_disconnect_by_data(self->source, self);
  g_clear_object(&self->source);
  G_OBJECT_CLASS(gh_timeline_parent_class)->dispose(object);
}

static void
gh_timeline_finalize(GObject *object)
{
  g_ptr_array_unref(GH_TIMELINE(object)->items);
  G_OBJECT_CLASS(gh_timeline_parent_class)->finalize(object);
}

static void
gh_timeline_class_init(GhTimelineClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_timeline_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_timeline_finalize;
}

static void
gh_timeline_init(GhTimeline *self)
{
  self->items = g_ptr_array_new_with_free_func(g_object_unref);
}

/* ---- GhConversationView ------------------------------------------------------------- */

typedef struct {
  GhLinkPreviewState state;
  gchar *uri;
  gchar *title;
  gchar *description;
} Preview;

static void
preview_free(Preview *preview)
{
  g_free(preview->uri);
  g_free(preview->title);
  g_free(preview->description);
  g_free(preview);
}

typedef enum {
  OPEN_NONE,       /* nothing pending */
  OPEN_LATEST,     /* open at the newest message */
  OPEN_FIRST_UNREAD
} OpenScroll;

struct _GhConversationView {
  AdwBreakpointBin parent_instance;
  AdwBanner *banner;
  GtkScrolledWindow *scroller;
  GtkListView *message_list;
  GtkWidget *loading_box;
  GtkButton *jump_button;
  GtkLabel *jump_count;
  GtkWidget *locked_row;
  GtkLabel *locked_label;
  AdwBreakpoint *compact_breakpoint;
  AdwAlertDialog *link_dialog;
  AdwAlertDialog *preview_dialog;
  GtkCheckButton *preview_dont_ask;

  GhConversation *conversation;
  GtkFilterListModel *visible; /* the conversation without expired messages */
  GhTimeline *timeline;
  GSettings *settings;
  gboolean compact;

  /* Scrolling */
  gboolean sticky;       /* keep the newest message in view */
  gboolean pin_pending;  /* a scroll to the newest message is queued */
  guint pin_idle;
  OpenScroll open_scroll;
  guint open_idle;
  guint open_target;     /* the first unread message's position */
  guint new_below;
  gboolean loading_older;
  GhConversationViewLoadOlder load_older;
  gpointer load_older_data;
  GDestroyNotify load_older_destroy;

  /* Expiry and day changes */
  guint expiry_source;
  gint64 expiry_at;      /* when expiry_source fires (unix seconds); 0 none */
  guint midnight_source;

  /* Delivery details */
  GhConversationViewDeliveryReport report_func;
  gpointer report_data;
  GDestroyNotify report_destroy;

  /* Links and previews */
  gchar *pending_link;     /* awaiting confirmation */
  gchar *pending_preview;  /* rumor id awaiting consent */
  GHashTable *previews;    /* rumor id -> Preview */
  GCancellable *cancellable;
  GhLinkPreviewFetch fetch;
  GhLinkPreviewFinish finish;
  gpointer fetch_data;
  GDestroyNotify fetch_destroy;

  guint announcements[3];
  gchar *last_announcement;
};

enum { PROP_0, PROP_CONVERSATION, PROP_COMPACT, PROP_SETTINGS, N_PROPS };
static GParamSpec *props[N_PROPS];

enum { SIGNAL_RETRY_REQUESTED, SIGNAL_UNLOCK_REQUESTED, SIGNAL_OPEN_URI, SIGNAL_PREVIEW_CHANGED,
       N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhConversationView, gh_conversation_view, ADW_TYPE_BREAKPOINT_BIN)

static gint64
now_seconds(void)
{
  return g_get_real_time() / G_USEC_PER_SEC;
}

/* ---- announcements ------------------------------------------------------------------ */

/* Only while the window is active: notifications cover the rest (charter
 * §7.14). */
static void
announce(GhConversationView *self, const gchar *text, GtkAccessibleAnnouncementPriority priority)
{
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  if (!GTK_IS_WINDOW(root) || !gtk_window_is_active(GTK_WINDOW(root)) ||
      !gtk_widget_get_mapped(GTK_WIDGET(self)))
    return;
  gtk_accessible_announce(GTK_ACCESSIBLE(self), text, priority);
  if ((guint)priority < G_N_ELEMENTS(self->announcements))
    self->announcements[priority]++;
  g_free(self->last_announcement);
  self->last_announcement = g_strdup(text);
}

/* ---- scrolling ---------------------------------------------------------------------- */

static GtkAdjustment *
vadjustment(GhConversationView *self)
{
  return gtk_scrolled_window_get_vadjustment(self->scroller);
}

static gdouble
distance_to_bottom(GhConversationView *self)
{
  GtkAdjustment *adj = vadjustment(self);
  return gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) -
         gtk_adjustment_get_value(adj);
}

static guint
n_visible(GhConversationView *self)
{
  return self->timeline ? g_list_model_get_n_items(G_LIST_MODEL(self->timeline)) : 0;
}

static void
update_jump(GhConversationView *self)
{
  gboolean shown = !self->sticky && n_visible(self) > 0;
  gtk_widget_set_visible(GTK_WIDGET(self->jump_button), shown);
  gtk_widget_set_visible(GTK_WIDGET(self->jump_count), self->new_below > 0);
  g_autofree gchar *count = g_strdup_printf("%u", self->new_below);
  gtk_label_set_text(self->jump_count, count);
  g_autofree gchar *label =
    self->new_below > 0
      ? g_strdup_printf(g_dngettext(NULL, "Jump to Latest, %u new message",
                                    "Jump to Latest, %u new messages", self->new_below),
                        self->new_below)
      : g_strdup(_("Jump to Latest"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->jump_button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static void
set_sticky(GhConversationView *self, gboolean sticky)
{
  self->sticky = sticky;
  if (sticky)
    self->new_below = 0;
  update_jump(self);
}

static void
maybe_load_older(GhConversationView *self)
{
  if (!self->conversation || !self->load_older || self->loading_older ||
      !gh_conversation_get_has_older(self->conversation))
    return;
  GtkAdjustment *adj = vadjustment(self);
  gdouble page = gtk_adjustment_get_page_size(adj);
  if (page <= 0 || gtk_adjustment_get_value(adj) > page / 2)
    return;
  self->loading_older = TRUE;
  gtk_widget_set_visible(self->loading_box, TRUE);
  self->load_older(self, self->conversation, self->load_older_data);
}

static gboolean
pin_to_latest(gpointer data)
{
  GhConversationView *self = data;
  self->pin_idle = 0;
  GtkAdjustment *adj = vadjustment(self);
  gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
  self->pin_pending = FALSE;
  set_sticky(self, TRUE);
  return G_SOURCE_REMOVE;
}

static void
queue_pin(GhConversationView *self)
{
  self->pin_pending = TRUE;
  if (!self->pin_idle)
    self->pin_idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE, pin_to_latest, self, NULL);
}

/* The scroll a newly shown conversation opens with, once the list has a
 * size: the newest message, or the first unread one at the top. It runs
 * after layout (an idle): a scroll made while the list allocates is
 * overridden by the list's own anchor. */
static gboolean
run_open_scroll(gpointer data)
{
  GhConversationView *self = data;
  self->open_idle = 0;
  GtkAdjustment *adj = vadjustment(self);
  if (self->open_scroll == OPEN_NONE || gtk_adjustment_get_page_size(adj) <= 0)
    return G_SOURCE_REMOVE;
  /* open_scroll stays set until the end, so on_value_changed() ignores
   * these moves. */
  gdouble upper = gtk_adjustment_get_upper(adj);
  gdouble page = gtk_adjustment_get_page_size(adj);
  gtk_adjustment_set_value(adj, upper - page);
  if (self->open_scroll == OPEN_FIRST_UNREAD && self->open_target < n_visible(self)) {
    /* From the bottom, scrolling an earlier message into view puts it at
     * the top of the list. Whether the view then rests at the bottom is
     * known once it is laid out (on_value_changed()), unless it all fits. */
    gtk_list_view_scroll_to(self->message_list, self->open_target, GTK_LIST_SCROLL_NONE, NULL);
    self->open_scroll = OPEN_NONE;
    if (upper <= page + EDGE_SLACK) {
      set_sticky(self, TRUE);
    } else {
      self->sticky = FALSE;
      update_jump(self);
    }
  } else {
    self->open_scroll = OPEN_NONE;
    set_sticky(self, TRUE);
    queue_pin(self);
  }
  return G_SOURCE_REMOVE;
}

static void
queue_open_scroll(GhConversationView *self)
{
  if (self->open_scroll != OPEN_NONE && !self->open_idle &&
      gtk_adjustment_get_page_size(vadjustment(self)) > 0)
    self->open_idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE, run_open_scroll, self, NULL);
}

static void
on_adjustment_changed(GhConversationView *self)
{
  queue_open_scroll(self);
  if (self->sticky && self->open_scroll == OPEN_NONE)
    queue_pin(self);
  maybe_load_older(self);
}

static void
on_value_changed(GhConversationView *self)
{
  if (self->pin_pending || self->open_scroll != OPEN_NONE)
    return;
  gboolean at_bottom = distance_to_bottom(self) <= EDGE_SLACK;
  if (at_bottom != self->sticky || (at_bottom && self->new_below))
    set_sticky(self, at_bottom);
  maybe_load_older(self);
}

void
gh_conversation_view_scroll_to_latest(GhConversationView *self)
{
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(self));
  guint n = n_visible(self);
  if (n > 0)
    gtk_list_view_scroll_to(self->message_list, n - 1, GTK_LIST_SCROLL_NONE, NULL);
  set_sticky(self, TRUE);
  queue_pin(self);
}

gboolean
gh_conversation_view_get_at_latest(GhConversationView *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), FALSE);
  return self->sticky;
}

guint
gh_conversation_view_get_new_below(GhConversationView *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), 0);
  return self->new_below;
}

/* ---- expiry and day changes ---------------------------------------------------------- */

static gboolean
not_expired(gpointer item, gpointer data)
{
  (void)data;
  gint64 expires = gh_message_get_expires_at(GH_MESSAGE(item));
  return expires == 0 || expires > now_seconds();
}

static void schedule_expiry(GhConversationView *self);

static gboolean
on_expiry(gpointer data)
{
  GhConversationView *self = data;
  self->expiry_source = 0;
  self->expiry_at = 0;
  gtk_filter_changed(gtk_filter_list_model_get_filter(self->visible),
                     GTK_FILTER_CHANGE_MORE_STRICT);
  schedule_expiry(self);
  return G_SOURCE_REMOVE;
}

/* The one expiry timer fires at @at unless it already fires sooner. */
static void
expire_by(GhConversationView *self, gint64 at)
{
  if (at <= 0 || (self->expiry_source && self->expiry_at <= at))
    return;
  g_clear_handle_id(&self->expiry_source, g_source_remove);
  gint64 now = now_seconds();
  gint64 wait_ms = at > now ? (at - now) * 1000 : 0;
  self->expiry_at = at;
  self->expiry_source = g_timeout_add((guint)MIN(wait_ms, (gint64)G_MAXINT32), on_expiry, self);
}

/* One timer at the soonest expiry of a shown message (charter §3.7): the
 * message is hidden then, whether or not the store has purged it yet. A
 * full scan only when the timer fires or the conversation changes; new
 * messages only bring it forward (expire_by()). */
static void
schedule_expiry(GhConversationView *self)
{
  g_clear_handle_id(&self->expiry_source, g_source_remove);
  self->expiry_at = 0;
  if (!self->visible)
    return;
  GListModel *model = G_LIST_MODEL(self->visible);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i);
    expire_by(self, gh_message_get_expires_at(message));
  }
}

static void schedule_midnight(GhConversationView *self);

static gboolean
on_midnight(gpointer data)
{
  GhConversationView *self = data;
  self->midnight_source = 0;
  if (self->timeline)
    timeline_refresh_days(self->timeline);
  schedule_midnight(self);
  return G_SOURCE_REMOVE;
}

/* "Today" becomes "Yesterday" at the next local midnight. */
static void
schedule_midnight(GhConversationView *self)
{
  g_clear_handle_id(&self->midnight_source, g_source_remove);
  if (!self->timeline)
    return;
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autoptr(GDateTime) today = g_date_time_new_local(g_date_time_get_year(now),
                                                     g_date_time_get_month(now),
                                                     g_date_time_get_day_of_month(now), 0, 0, 0);
  g_autoptr(GDateTime) tomorrow = g_date_time_add_days(today, 1);
  gint64 seconds = g_date_time_difference(tomorrow, now) / G_TIME_SPAN_SECOND + 1;
  self->midnight_source = g_timeout_add_seconds((guint)CLAMP(seconds, 1, 86400 + 3600),
                                                on_midnight, self);
}

/* ---- messages of the shown conversation ---------------------------------------------- */

/* An expiry learned later (gh_message_set_expires_at()). */
static void
on_message_expires(GhConversationView *self, GParamSpec *pspec, GhMessage *message)
{
  (void)pspec;
  gtk_filter_changed(gtk_filter_list_model_get_filter(self->visible),
                     GTK_FILTER_CHANGE_MORE_STRICT);
  expire_by(self, gh_message_get_expires_at(message));
}

/* A send failure is announced assertively (charter §7.6, §7.14). */
static void
on_message_status(GhConversationView *self, GParamSpec *pspec, GhMessage *message)
{
  (void)pspec;
  GhMessageStatus status = gh_message_get_status(message);
  if (status == GH_MESSAGE_STATUS_NOT_SENT)
    announce(self, _("Message not sent"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
  else if (status == GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX)
    announce(self, gh_message_status_get_accessible_description(status),
             GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
}

static void
watch_message(GhConversationView *self, GhMessage *message)
{
  g_signal_connect_object(message, "notify::expires-at", G_CALLBACK(on_message_expires), self,
                          G_CONNECT_SWAPPED);
  if (gh_message_is_self(message))
    g_signal_connect_object(message, "notify::status", G_CALLBACK(on_message_status), self,
                            G_CONNECT_SWAPPED);
}

static void
unwatch_messages(GhConversationView *self)
{
  if (!self->conversation)
    return;
  GListModel *model = G_LIST_MODEL(self->conversation);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i);
    g_signal_handlers_disconnect_by_data(message, self);
  }
}

static void
on_conversation_changed(GhConversationView *self, guint position, guint removed, guint added,
                        GListModel *model)
{
  (void)removed;
  for (guint i = position; i < position + added; i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i);
    g_signal_handlers_disconnect_by_data(message, self);
    watch_message(self, message);
    expire_by(self, gh_message_get_expires_at(message));
  }
}

/* New messages at the end: stick to them, or count them for "Jump to
 * Latest", and announce a new incoming one. Older history arrives at the
 * start and moves nothing. */
static void
on_timeline_changed(GhConversationView *self, guint position, guint removed, guint added,
                    GListModel *timeline)
{
  if (added == 0 || position + added != g_list_model_get_n_items(timeline) ||
      self->open_scroll != OPEN_NONE)
    return;
  guint incoming = 0;
  g_autoptr(GhTimelineItem) last_incoming = NULL;
  for (guint i = position; i < position + added; i++) {
    g_autoptr(GhTimelineItem) item = g_list_model_get_item(timeline, i);
    if (!gh_message_is_self(item->message)) {
      incoming++;
      g_set_object(&last_incoming, item);
    }
  }
  /* An own message (a local echo) always brings the view to the end. */
  if (self->sticky || incoming < added) {
    set_sticky(self, TRUE);
    queue_pin(self);
  } else if (removed == 0) {
    self->new_below += incoming;
    update_jump(self);
  }
  if (last_incoming) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *text = gh_message_row_compose_summary(last_incoming->message, now);
    announce(self, text, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  }
}

/* Closes a dialog of this conversation if it is on screen. */
static void
close_dialog(AdwDialog *dialog)
{
  if (gtk_widget_get_root(GTK_WIDGET(dialog)))
    adw_dialog_force_close(dialog);
}

static gboolean
is_multi_party(GhConversation *conversation)
{
  const gchar *const *peers = gh_conversation_get_peers(conversation);
  return peers && peers[0] && peers[1];
}

static void
cancel_previews(GhConversationView *self)
{
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  g_hash_table_remove_all(self->previews);
  g_clear_pointer(&self->pending_preview, g_free);
  g_clear_pointer(&self->pending_link, g_free);
}

void
gh_conversation_view_set_conversation(GhConversationView *self, GhConversation *conversation)
{
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(self));
  g_return_if_fail(!conversation || GH_IS_CONVERSATION(conversation));
  if (self->conversation == conversation)
    return;

  unwatch_messages(self);
  if (self->conversation)
    g_signal_handlers_disconnect_by_data(self->conversation, self);
  if (self->timeline)
    g_signal_handlers_disconnect_by_data(self->timeline, self);
  gtk_list_view_set_model(self->message_list, NULL);
  g_clear_object(&self->timeline);
  g_clear_object(&self->visible);
  g_clear_handle_id(&self->expiry_source, g_source_remove);
  self->expiry_at = 0;
  g_clear_handle_id(&self->midnight_source, g_source_remove);
  g_clear_handle_id(&self->pin_idle, g_source_remove);
  g_clear_handle_id(&self->open_idle, g_source_remove);
  self->pin_pending = FALSE;
  cancel_previews(self);
  close_dialog(ADW_DIALOG(self->link_dialog));
  close_dialog(ADW_DIALOG(self->preview_dialog));
  if (self->loading_older) {
    self->loading_older = FALSE;
    gtk_widget_set_visible(self->loading_box, FALSE);
  }
  self->new_below = 0;
  self->open_scroll = OPEN_NONE;
  g_set_object(&self->conversation, conversation);

  if (conversation) {
    GtkCustomFilter *filter = gtk_custom_filter_new(not_expired, NULL, NULL);
    self->visible = gtk_filter_list_model_new(g_object_ref(G_LIST_MODEL(conversation)),
                                              GTK_FILTER(filter));
    self->timeline = timeline_new(G_LIST_MODEL(self->visible), is_multi_party(conversation));
    on_conversation_changed(self, 0, 0, g_list_model_get_n_items(G_LIST_MODEL(conversation)),
                            G_LIST_MODEL(conversation));
    g_signal_connect_object(conversation, "items-changed", G_CALLBACK(on_conversation_changed),
                            self, G_CONNECT_SWAPPED);
    g_signal_connect_object(self->timeline, "items-changed", G_CALLBACK(on_timeline_changed),
                            self, G_CONNECT_SWAPPED);
    g_autoptr(GtkNoSelection) selection =
      gtk_no_selection_new(g_object_ref(G_LIST_MODEL(self->timeline)));
    gtk_list_view_set_model(self->message_list, GTK_SELECTION_MODEL(selection));

    guint n = n_visible(self);
    guint unread = MIN(gh_conversation_get_unread_count(conversation), n);
    self->open_scroll = unread > 0 ? OPEN_FIRST_UNREAD : OPEN_LATEST;
    self->open_target = n - unread;
    self->new_below = unread;
    self->sticky = unread == 0;
    schedule_midnight(self);
    queue_open_scroll(self);
  } else {
    self->sticky = TRUE;
  }
  update_jump(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CONVERSATION]);
}

GhConversation *
gh_conversation_view_get_conversation(GhConversationView *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), NULL);
  return self->conversation;
}

GListModel *
gh_conversation_view_get_timeline(GhConversationView *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), NULL);
  return self->timeline ? G_LIST_MODEL(self->timeline) : NULL;
}

GtkListView *
gh_conversation_view_get_message_list(GhConversationView *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), NULL);
  return self->message_list;
}

/* ---- older history ------------------------------------------------------------------ */

void
gh_conversation_view_set_history_loader(GhConversationView *self,
                                        GhConversationViewLoadOlder load_older,
                                        gpointer user_data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(self));
  if (self->load_older_destroy)
    self->load_older_destroy(self->load_older_data);
  self->load_older = load_older;
  self->load_older_data = user_data;
  self->load_older_destroy = destroy;
}

void
gh_conversation_view_finish_loading_older(GhConversationView *self)
{
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(self));
  if (!self->loading_older)
    return;
  self->loading_older = FALSE;
  gtk_widget_set_visible(self->loading_box, FALSE);
}

gboolean
gh_conversation_view_get_loading_older(GhConversationView *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), FALSE);
  return self->loading_older;
}

/* ---- delivery details ---------------------------------------------------------------- */

void
gh_conversation_view_set_delivery_report_func(GhConversationView *self,
                                              GhConversationViewDeliveryReport func,
                                              gpointer user_data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(self));
  if (self->report_destroy)
    self->report_destroy(self->report_data);
  self->report_func = func;
  self->report_data = user_data;
  self->report_destroy = destroy;
}

GhDeliveryReport *
gh_conversation_view_dup_delivery_report(GhConversationView *self, GhMessage *message)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), NULL);
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  return self->report_func ? self->report_func(message, self->report_data) : NULL;
}

/* ---- links ---------------------------------------------------------------------------- */

static void
gh_conversation_view_real_open_uri(GhConversationView *self, const gchar *uri)
{
  g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(uri);
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  gtk_uri_launcher_launch(launcher, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL, NULL,
                          NULL);
}

static void
show_toast(GhConversationView *self, const gchar *title)
{
  GtkWidget *overlay = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_TOAST_OVERLAY);
  if (overlay)
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(overlay), adw_toast_new(title));
}

static gchar *
confirmation_body(GhLinkConfirmReasons reasons, const gchar *open_uri)
{
  GString *body = g_string_new(NULL);
  if (reasons & GH_LINK_CONFIRM_IDN)
    g_string_append(body, _("This address uses letters that can look like other letters, "
                            "so it may not be the website it seems to be. "));
  if (reasons & GH_LINK_CONFIRM_USER_INFO)
    g_string_append(body, _("The part before “@” is not the website. "));
  if (reasons & GH_LINK_CONFIRM_INSECURE)
    g_string_append(body, _("This link isn't secure: others on your network can see and "
                            "change what it loads. "));
  /* TRANSLATORS: followed by the full web address a link opens. */
  g_string_append_printf(body, _("It opens:\n\n%s"), open_uri);
  return g_string_free(body, FALSE);
}

static void
open_link(GhConversationView *self, const gchar *uri)
{
  GhLinkConfirmReasons reasons = GH_LINK_CONFIRM_NONE;
  g_autofree gchar *open_uri = NULL;
  switch (gh_link_policy_classify(uri, &reasons, &open_uri)) {
  case GH_LINK_ACTION_OPEN:
    g_signal_emit(self, signals[SIGNAL_OPEN_URI], 0, open_uri);
    break;
  case GH_LINK_ACTION_CONFIRM: {
    g_autofree gchar *body = confirmation_body(reasons, open_uri);
    adw_alert_dialog_set_body(self->link_dialog, body);
    g_free(self->pending_link);
    self->pending_link = g_steal_pointer(&open_uri);
    adw_dialog_present(ADW_DIALOG(self->link_dialog), GTK_WIDGET(self));
    break;
  }
  case GH_LINK_ACTION_NOSTR:
    /* Never fetched or handed to another app: the address is copied. */
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), uri);
    show_toast(self, _("Nostr address copied"));
    announce(self, _("Nostr address copied"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    break;
  case GH_LINK_ACTION_REFUSE:
    break;
  }
}

static void
on_link_response(GhConversationView *self, const gchar *response)
{
  g_autofree gchar *uri = g_steal_pointer(&self->pending_link);
  if (uri && g_strcmp0(response, "link-open") == 0)
    g_signal_emit(self, signals[SIGNAL_OPEN_URI], 0, uri);
}

/* ---- link previews (charter §2.1, D13) ------------------------------------------------- */

static Preview *
preview_for(GhConversationView *self, const gchar *rumor_id)
{
  return g_hash_table_lookup(self->previews, rumor_id);
}

static void
set_preview_state(GhConversationView *self, const gchar *rumor_id, GhLinkPreviewState state)
{
  Preview *preview = preview_for(self, rumor_id);
  if (!preview || preview->state == state)
    return;
  preview->state = state;
  g_signal_emit(self, signals[SIGNAL_PREVIEW_CHANGED], 0, rumor_id);
}

typedef struct {
  GWeakRef view;
  gchar *rumor_id;
} FetchClosure;

static void
on_preview_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  FetchClosure *closure = data;
  g_autoptr(GhConversationView) self = g_weak_ref_get(&closure->view);
  g_weak_ref_clear(&closure->view);
  g_autofree gchar *rumor_id = closure->rumor_id;
  g_free(closure);
  g_autofree gchar *title = NULL;
  g_autofree gchar *description = NULL;
  g_autoptr(GError) error = NULL;
  if (!self || !self->finish)
    return;
  gboolean ok = self->finish(result, &title, &description, &error, self->fetch_data);
  if (!ok && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  Preview *preview = preview_for(self, rumor_id);
  if (!preview)
    return;
  if (ok) {
    g_free(preview->title);
    g_free(preview->description);
    preview->title = g_steal_pointer(&title);
    preview->description = g_steal_pointer(&description);
  }
  set_preview_state(self, rumor_id, ok ? GH_LINK_PREVIEW_LOADED : GH_LINK_PREVIEW_FAILED);
}

static void
fetch_preview(GhConversationView *self, const gchar *rumor_id)
{
  Preview *preview = preview_for(self, rumor_id);
  if (!preview)
    return;
  if (!self->fetch || !self->finish) {
    /* No fetcher in this build: say so, and load nothing. */
    set_preview_state(self, rumor_id, GH_LINK_PREVIEW_UNAVAILABLE);
    return;
  }
  if (!self->cancellable)
    self->cancellable = g_cancellable_new();
  set_preview_state(self, rumor_id, GH_LINK_PREVIEW_LOADING);
  FetchClosure *closure = g_new0(FetchClosure, 1);
  g_weak_ref_init(&closure->view, self);
  closure->rumor_id = g_strdup(rumor_id);
  self->fetch(preview->uri, self->cancellable, on_preview_fetched, closure, self->fetch_data);
}

static gboolean
settings_has_key(GSettings *settings, const gchar *key)
{
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  return schema && g_settings_schema_has_key(schema, key);
}

static gboolean
previews_allowed(GhConversationView *self)
{
  return self->settings && settings_has_key(self->settings, LINK_PREVIEWS_KEY) &&
         g_settings_get_boolean(self->settings, LINK_PREVIEWS_KEY);
}

static gchar *
consent_body(GhConversationView *self, const gchar *host)
{
  g_autofree gchar *mode = self->settings && settings_has_key(self->settings, NETWORK_MODE_KEY)
                             ? g_settings_get_string(self->settings, NETWORK_MODE_KEY)
                             : NULL;
  if (g_strcmp0(mode, "tor") == 0)
    return g_strdup_printf(_("This connects to %s through Tor to load a preview of the link."),
                           host);
  return g_strdup_printf(_("This connects to %s from your IP address to load a preview of the "
                           "link, so that website learns your IP address."), host);
}

static void
show_preview(GhConversationView *self, const gchar *rumor_id)
{
  GhMessage *message = self->conversation
                         ? gh_conversation_lookup_message(self->conversation, rumor_id)
                         : NULL;
  if (!message)
    return;
  g_autofree gchar *uri = gh_link_policy_dup_preview_uri(gh_message_get_content(message));
  if (!uri)
    return;
  Preview *preview = preview_for(self, rumor_id);
  if (!preview) {
    preview = g_new0(Preview, 1);
    preview->uri = g_steal_pointer(&uri);
    g_hash_table_insert(self->previews, g_strdup(rumor_id), preview);
  }
  if (preview->state != GH_LINK_PREVIEW_NONE && preview->state != GH_LINK_PREVIEW_FAILED)
    return;
  if (previews_allowed(self)) {
    fetch_preview(self, rumor_id);
    return;
  }
  /* Consent first (charter §2.1, P9): who is contacted, and how. */
  g_autofree gchar *host = gh_link_policy_dup_host(preview->uri);
  g_autofree gchar *body = consent_body(self, host);
  adw_alert_dialog_set_body(self->preview_dialog, body);
  gtk_check_button_set_active(self->preview_dont_ask, FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->preview_dont_ask),
                         self->settings && settings_has_key(self->settings, LINK_PREVIEWS_KEY));
  g_free(self->pending_preview);
  self->pending_preview = g_strdup(rumor_id);
  set_preview_state(self, rumor_id, GH_LINK_PREVIEW_ASKING);
  adw_dialog_present(ADW_DIALOG(self->preview_dialog), GTK_WIDGET(self));
}

static void
on_preview_response(GhConversationView *self, const gchar *response)
{
  g_autofree gchar *rumor_id = g_steal_pointer(&self->pending_preview);
  if (!rumor_id)
    return;
  if (g_strcmp0(response, "preview-show") != 0) {
    set_preview_state(self, rumor_id, GH_LINK_PREVIEW_NONE);
    return;
  }
  if (gtk_check_button_get_active(self->preview_dont_ask) && self->settings &&
      settings_has_key(self->settings, LINK_PREVIEWS_KEY))
    g_settings_set_boolean(self->settings, LINK_PREVIEWS_KEY, TRUE);
  fetch_preview(self, rumor_id);
}

GhLinkPreviewState
gh_conversation_view_get_link_preview(GhConversationView *self, GhMessage *message,
                                      const gchar **title, const gchar **description)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), GH_LINK_PREVIEW_NONE);
  g_return_val_if_fail(GH_IS_MESSAGE(message), GH_LINK_PREVIEW_NONE);
  Preview *preview = preview_for(self, gh_message_get_rumor_id(message));
  if (title)
    *title = preview ? preview->title : NULL;
  if (description)
    *description = preview ? preview->description : NULL;
  return preview ? preview->state : GH_LINK_PREVIEW_NONE;
}

void
gh_conversation_view_set_link_preview_fetcher(GhConversationView *self, GhLinkPreviewFetch fetch,
                                              GhLinkPreviewFinish finish, gpointer user_data,
                                              GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(self));
  g_return_if_fail((fetch == NULL) == (finish == NULL));
  if (self->fetch_destroy)
    self->fetch_destroy(self->fetch_data);
  self->fetch = fetch;
  self->finish = finish;
  self->fetch_data = user_data;
  self->fetch_destroy = destroy;
}

/* ---- conversation states ---------------------------------------------------------------- */

void
gh_conversation_view_set_recipient_without_inbox(GhConversationView *self, const gchar *name)
{
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(self));
  if (name) {
    g_autofree gchar *title = g_strdup_printf(_("%s hasn't set up private messaging yet"), name);
    adw_banner_set_title(self->banner, title);
  }
  adw_banner_set_revealed(self->banner, name != NULL);
}

void
gh_conversation_view_set_locked_messages(GhConversationView *self, guint count)
{
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(self));
  if (count > 0) {
    g_autofree gchar *text = g_strdup_printf(
      g_dngettext(NULL, "Waiting for Nostr Signer to unlock %u message",
                  "Waiting for Nostr Signer to unlock %u messages", count), count);
    gtk_label_set_text(self->locked_label, text);
  }
  gtk_widget_set_visible(self->locked_row, count > 0);
}

guint
gh_conversation_view_get_announcements(GhConversationView *self,
                                       GtkAccessibleAnnouncementPriority priority)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), 0);
  return (guint)priority < G_N_ELEMENTS(self->announcements) ? self->announcements[priority] : 0;
}

const gchar *
gh_conversation_view_get_last_announcement(GhConversationView *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), NULL);
  return self->last_announcement;
}

/* ---- actions ----------------------------------------------------------------------------- */

static void
action_open_link(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  open_link(GH_CONVERSATION_VIEW(widget), g_variant_get_string(parameter, NULL));
}

static void
action_show_preview(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  show_preview(GH_CONVERSATION_VIEW(widget), g_variant_get_string(parameter, NULL));
}

static void
action_retry(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhConversationView *self = GH_CONVERSATION_VIEW(widget);
  (void)name;
  GhMessage *message =
    self->conversation
      ? gh_conversation_lookup_message(self->conversation, g_variant_get_string(parameter, NULL))
      : NULL;
  if (message && gh_message_is_self(message))
    g_signal_emit(self, signals[SIGNAL_RETRY_REQUESTED], 0, message);
}

static void
action_jump(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  gh_conversation_view_scroll_to_latest(GH_CONVERSATION_VIEW(widget));
}

static void
action_unlock(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  g_signal_emit(widget, signals[SIGNAL_UNLOCK_REQUESTED], 0);
}

/* ---- widget ------------------------------------------------------------------------------ */

static void
set_compact(GhConversationView *self, gboolean compact)
{
  if (self->compact == compact)
    return;
  self->compact = compact;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_COMPACT]);
}

static void
on_compact_apply(GhConversationView *self)
{
  set_compact(self, TRUE);
}

static void
on_compact_unapply(GhConversationView *self)
{
  set_compact(self, FALSE);
}

/* High contrast outlines the bubbles (charter §7.6; GTK 4.14 has no
 * prefers-contrast media query). */
static void
on_high_contrast(GhConversationView *self)
{
  if (adw_style_manager_get_high_contrast(adw_style_manager_get_default()))
    gtk_widget_add_css_class(GTK_WIDGET(self), "hc");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self), "hc");
}

static gboolean
gh_conversation_view_grab_focus(GtkWidget *widget)
{
  return gtk_widget_grab_focus(GTK_WIDGET(GH_CONVERSATION_VIEW(widget)->message_list));
}

GtkWidget *
gh_conversation_view_new(void)
{
  return g_object_new(GH_TYPE_CONVERSATION_VIEW, NULL);
}

void
gh_conversation_view_set_settings(GhConversationView *self, GSettings *settings)
{
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(self));
  g_return_if_fail(!settings || G_IS_SETTINGS(settings));
  if (g_set_object(&self->settings, settings))
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SETTINGS]);
}

gboolean
gh_conversation_view_get_compact(GhConversationView *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_VIEW(self), FALSE);
  return self->compact;
}

static void
gh_conversation_view_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhConversationView *self = GH_CONVERSATION_VIEW(object);
  switch (id) {
  case PROP_CONVERSATION:
    g_value_set_object(value, self->conversation);
    break;
  case PROP_COMPACT:
    g_value_set_boolean(value, self->compact);
    break;
  case PROP_SETTINGS:
    g_value_set_object(value, self->settings);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_conversation_view_set_property(GObject *object, guint id, const GValue *value,
                                  GParamSpec *pspec)
{
  GhConversationView *self = GH_CONVERSATION_VIEW(object);
  switch (id) {
  case PROP_CONVERSATION:
    gh_conversation_view_set_conversation(self, g_value_get_object(value));
    break;
  case PROP_SETTINGS:
    gh_conversation_view_set_settings(self, g_value_get_object(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_conversation_view_dispose(GObject *object)
{
  GhConversationView *self = GH_CONVERSATION_VIEW(object);
  if (self->message_list)
    gh_conversation_view_set_conversation(self, NULL);
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  g_clear_handle_id(&self->pin_idle, g_source_remove);
  g_clear_handle_id(&self->open_idle, g_source_remove);
  g_clear_handle_id(&self->expiry_source, g_source_remove);
  g_clear_handle_id(&self->midnight_source, g_source_remove);
  gh_conversation_view_set_history_loader(self, NULL, NULL, NULL);
  gh_conversation_view_set_delivery_report_func(self, NULL, NULL, NULL);
  gh_conversation_view_set_link_preview_fetcher(self, NULL, NULL, NULL, NULL);
  g_clear_object(&self->settings);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_CONVERSATION_VIEW);
  G_OBJECT_CLASS(gh_conversation_view_parent_class)->dispose(object);
}

static void
gh_conversation_view_finalize(GObject *object)
{
  GhConversationView *self = GH_CONVERSATION_VIEW(object);
  g_hash_table_unref(self->previews);
  g_free(self->pending_link);
  g_free(self->pending_preview);
  g_free(self->last_announcement);
  G_OBJECT_CLASS(gh_conversation_view_parent_class)->finalize(object);
}

static void
gh_conversation_view_class_init(GhConversationViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->get_property = gh_conversation_view_get_property;
  object_class->set_property = gh_conversation_view_set_property;
  object_class->dispose = gh_conversation_view_dispose;
  object_class->finalize = gh_conversation_view_finalize;
  widget_class->grab_focus = gh_conversation_view_grab_focus;

  props[PROP_CONVERSATION] = g_param_spec_object("conversation", NULL, NULL,
    GH_TYPE_CONVERSATION, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_COMPACT] = g_param_spec_boolean("compact", NULL, NULL, FALSE,
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_SETTINGS] = g_param_spec_object("settings", NULL, NULL, G_TYPE_SETTINGS,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  signals[SIGNAL_RETRY_REQUESTED] = g_signal_new("retry-requested", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, GH_TYPE_MESSAGE);
  signals[SIGNAL_UNLOCK_REQUESTED] = g_signal_new("unlock-requested", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_OPEN_URI] = g_signal_new_class_handler("open-uri", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, G_CALLBACK(gh_conversation_view_real_open_uri), NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_PREVIEW_CHANGED] = g_signal_new("preview-changed", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  /* The templates name these types; they resolve by name. */
  g_type_ensure(GH_TYPE_TIMELINE_ITEM);
  g_type_ensure(GH_TYPE_MESSAGE_ROW);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-conversation-view.ui");
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, banner);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, scroller);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, message_list);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, loading_box);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, jump_button);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, jump_count);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, locked_row);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, locked_label);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, compact_breakpoint);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, link_dialog);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, preview_dialog);
  gtk_widget_class_bind_template_child(widget_class, GhConversationView, preview_dont_ask);
  gtk_widget_class_bind_template_child_full(widget_class, "unlock_button", FALSE, 0);

  gtk_widget_class_install_action(widget_class, "conversation.open-link", "s", action_open_link);
  gtk_widget_class_install_action(widget_class, "conversation.show-preview", "s",
                                  action_show_preview);
  gtk_widget_class_install_action(widget_class, "conversation.retry-message", "s", action_retry);
  gtk_widget_class_install_action(widget_class, "conversation.jump-to-latest", NULL,
                                  action_jump);
  gtk_widget_class_install_action(widget_class, "conversation.unlock-messages", NULL,
                                  action_unlock);
}

static void
gh_conversation_view_init(GhConversationView *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->sticky = TRUE;
  self->previews = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)preview_free);
  g_autoptr(GtkListItemFactory) headers = gtk_builder_list_item_factory_new_from_resource(
    NULL, "/org/nostr/Groundhog/ui/gh-day-separator.ui");
  gtk_list_view_set_header_factory(self->message_list, headers);

  GtkAdjustment *adj = vadjustment(self);
  g_signal_connect_object(adj, "changed", G_CALLBACK(on_adjustment_changed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(adj, "value-changed", G_CALLBACK(on_value_changed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_swapped(self->compact_breakpoint, "apply", G_CALLBACK(on_compact_apply), self);
  g_signal_connect_swapped(self->compact_breakpoint, "unapply", G_CALLBACK(on_compact_unapply),
                           self);
  g_signal_connect_swapped(self->link_dialog, "response", G_CALLBACK(on_link_response), self);
  g_signal_connect_swapped(self->preview_dialog, "response", G_CALLBACK(on_preview_response),
                           self);
  g_signal_connect_object(adw_style_manager_get_default(), "notify::high-contrast",
                          G_CALLBACK(on_high_contrast), self, G_CONNECT_SWAPPED);
  on_high_contrast(self);
  update_jump(self);
}
