#include "gh-agent-event-row.h"
#include "gh-agent-event.h"

struct _GhAgentEventRow {
  GtkBox parent_instance;
  GtkImage *icon;
  GtkLabel *primary;
  GtkLabel *detail;
  gchar *summary;
};

G_DEFINE_FINAL_TYPE(GhAgentEventRow, gh_agent_event_row, GTK_TYPE_BOX)

GtkWidget *
gh_agent_event_row_new(void)
{
  return g_object_new(GH_TYPE_AGENT_EVENT_ROW, NULL);
}

void
gh_agent_event_row_set_message(GhAgentEventRow *self, GhMessage *message)
{
  g_return_if_fail(GH_IS_AGENT_EVENT_ROW(self));
  g_autoptr(GhAgentEvent) event = message ? gh_agent_event_parse_message(message) : NULL;
  gtk_widget_set_visible(GTK_WIDGET(self), event != NULL);
  gtk_image_set_from_icon_name(self->icon, event ? event->icon_name : NULL);
  gtk_label_set_text(self->primary, event ? event->text : "");
  gtk_label_set_text(self->detail, event && event->detail ? event->detail : "");
  gtk_widget_set_visible(GTK_WIDGET(self->detail), event && event->detail);
  g_free(self->summary);
  self->summary = event ? g_strdup_printf("%s%s%s", event->text,
    event->detail ? ". " : "", event->detail ? event->detail : "") : g_strdup("");
  gtk_accessible_update_property(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 self->summary, -1);
}

const gchar *
gh_agent_event_row_get_summary(GhAgentEventRow *self)
{
  g_return_val_if_fail(GH_IS_AGENT_EVENT_ROW(self), NULL);
  return self->summary;
}

static void
gh_agent_event_row_finalize(GObject *object)
{
  g_free(GH_AGENT_EVENT_ROW(object)->summary);
  G_OBJECT_CLASS(gh_agent_event_row_parent_class)->finalize(object);
}

static void
gh_agent_event_row_class_init(GhAgentEventRowClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gh_agent_event_row_finalize;
}

static void
gh_agent_event_row_init(GhAgentEventRow *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 12);
  gtk_box_set_spacing(GTK_BOX(self), 6);
  gtk_widget_add_css_class(GTK_WIDGET(self), "groundhog-timeline-event");
  gtk_widget_add_css_class(GTK_WIDGET(self), "groundhog-agent-event");
  self->icon = GTK_IMAGE(gtk_image_new());
  gtk_image_set_pixel_size(self->icon, 12);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->icon));
  self->primary = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(self->primary, TRUE);
  gtk_label_set_selectable(self->primary, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->primary), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(self->primary), "dim-label");
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->primary));
  self->detail = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->detail), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(self->detail), "dim-label");
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->detail));
  self->summary = g_strdup("");
  gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}
