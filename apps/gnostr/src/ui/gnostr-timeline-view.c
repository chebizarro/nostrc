#include "gnostr-timeline-view.h"

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-timeline-view.ui"

struct _GnostrTimelineView {
  GtkWidget parent_instance;
  GtkWidget *root_scroller;
  GtkWidget *list_view;
};

G_DEFINE_TYPE(GnostrTimelineView, gnostr_timeline_view, GTK_TYPE_WIDGET)

static void gnostr_timeline_view_class_init(GnostrTimelineViewClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, root_scroller);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, list_view);
}

static void gnostr_timeline_view_init(GnostrTimelineView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
}

GtkWidget *gnostr_timeline_view_new(void) {
  return g_object_new(GNOSTR_TYPE_TIMELINE_VIEW, NULL);
}
