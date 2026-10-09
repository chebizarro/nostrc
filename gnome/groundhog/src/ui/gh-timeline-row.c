#include "gh-timeline-row.h"
#include "gh-conversation-row.h"
#include "gh-agent-event-row.h"

#include <glib/gi18n.h>

struct _GhTimelineRow {
  GtkWidget parent_instance;
  GhMessageRow *message_row;
  GtkWidget *event_box;
  GtkLabel *event_label;
  GhAgentEventRow *agent_row;
  GhTimelineItem *item;
  gchar *summary;
};

enum { PROP_0, PROP_ITEM, PROP_SUMMARY, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhTimelineRow, gh_timeline_row, GTK_TYPE_WIDGET)

/* A message reads as its row does; an event as its text and time. */
static void
update_summary(GhTimelineRow *self)
{
  const gchar *event = self->item ? gh_timeline_item_get_event_text(self->item) : NULL;
  g_autofree gchar *summary = NULL;
  if (self->item && gh_timeline_item_get_is_agent(self->item)) {
    summary = g_strdup(gh_agent_event_row_get_summary(self->agent_row));
  } else if (event) {
    g_autofree gchar *time =
      gh_conversation_row_format_time_of_day(gh_timeline_item_get_event_at(self->item));
    /* TRANSLATORS: a local timeline event ("You set messages to disappear
     * after 1 day") and its time of day, read out by a screen reader. */
    summary = *time ? g_strdup_printf(_("%s. %s"), event, time) : g_strdup(event);
  } else {
    summary = g_strdup(gh_message_row_get_summary(self->message_row));
  }
  if (!summary)
    summary = g_strdup("");
  if (g_strcmp0(summary, self->summary) == 0)
    return;
  g_free(self->summary);
  self->summary = g_steal_pointer(&summary);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SUMMARY]);
}

GtkWidget *
gh_timeline_row_new(void)
{
  return g_object_new(GH_TYPE_TIMELINE_ROW, NULL);
}

GhTimelineItem *
gh_timeline_row_get_item(GhTimelineRow *self)
{
  g_return_val_if_fail(GH_IS_TIMELINE_ROW(self), NULL);
  return self->item;
}

void
gh_timeline_row_set_item(GhTimelineRow *self, GhTimelineItem *item)
{
  g_return_if_fail(GH_IS_TIMELINE_ROW(self));
  g_return_if_fail(!item || GH_IS_TIMELINE_ITEM(item));
  if (!g_set_object(&self->item, item))
    return;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ITEM]);
  /* Template bindings have no ordering guarantee: the message setter clears
   * the old summary for recycled rows. Reapply the new item's summary after
   * the message binding has run, including on the initial bind. */
  gboolean agent = item && gh_timeline_item_get_is_agent(item);
  gh_message_row_set_message(self->message_row,
    item && !agent ? gh_timeline_item_get_message(item) : NULL);
  gh_message_row_set_reaction_summary(self->message_row,
    item && !agent ? gh_timeline_item_get_reaction_summary(item) : NULL);
  gh_agent_event_row_set_message(self->agent_row,
    agent ? gh_timeline_item_get_message(item) : NULL);
  update_summary(self);
}

const gchar *
gh_timeline_row_get_summary(GhTimelineRow *self)
{
  g_return_val_if_fail(GH_IS_TIMELINE_ROW(self), NULL);
  return self->summary;
}

GhMessageRow *
gh_timeline_row_get_message_row(GhTimelineRow *self)
{
  g_return_val_if_fail(GH_IS_TIMELINE_ROW(self), NULL);
  return self->message_row;
}

static void
gh_timeline_row_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhTimelineRow *self = GH_TIMELINE_ROW(object);
  switch (id) {
  case PROP_ITEM:
    g_value_set_object(value, self->item);
    break;
  case PROP_SUMMARY:
    g_value_set_string(value, self->summary);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_timeline_row_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  GhTimelineRow *self = GH_TIMELINE_ROW(object);
  switch (id) {
  case PROP_ITEM: {
    GObject *item = g_value_get_object(value);
    gh_timeline_row_set_item(self, GH_IS_TIMELINE_ITEM(item) ? GH_TIMELINE_ITEM(item) : NULL);
    break;
  }
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_timeline_row_dispose(GObject *object)
{
  GhTimelineRow *self = GH_TIMELINE_ROW(object);
  g_clear_object(&self->item);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_TIMELINE_ROW);
  /* A plain GtkWidget owns its template's unnamed children too. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(object))))
    gtk_widget_unparent(child);
  G_OBJECT_CLASS(gh_timeline_row_parent_class)->dispose(object);
}

static void
gh_timeline_row_finalize(GObject *object)
{
  g_free(GH_TIMELINE_ROW(object)->summary);
  G_OBJECT_CLASS(gh_timeline_row_parent_class)->finalize(object);
}

static void
gh_timeline_row_class_init(GhTimelineRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->get_property = gh_timeline_row_get_property;
  object_class->set_property = gh_timeline_row_set_property;
  object_class->dispose = gh_timeline_row_dispose;
  object_class->finalize = gh_timeline_row_finalize;
  /* A GObject, so a list item's "item" binds to it directly. */
  props[PROP_ITEM] = g_param_spec_object("item", NULL, NULL, G_TYPE_OBJECT,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_SUMMARY] = g_param_spec_string("summary", NULL, NULL, "",
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  g_type_ensure(GH_TYPE_MESSAGE_ROW);
  g_type_ensure(GH_TYPE_TIMELINE_ITEM);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-timeline-row.ui");
  gtk_widget_class_bind_template_child(widget_class, GhTimelineRow, message_row);
  gtk_widget_class_bind_template_child(widget_class, GhTimelineRow, event_box);
  gtk_widget_class_bind_template_child(widget_class, GhTimelineRow, event_label);
}

static void
gh_timeline_row_init(GhTimelineRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->agent_row = GH_AGENT_EVENT_ROW(gh_agent_event_row_new());
  gtk_widget_set_parent(GTK_WIDGET(self->agent_row), GTK_WIDGET(self));
  self->summary = g_strdup("");
  g_signal_connect_swapped(self->message_row, "notify::summary", G_CALLBACK(update_summary),
                           self);
}
