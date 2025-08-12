#include "gnostr-timeline-view.h"
#include <string.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-timeline-view.ui"

struct _GnostrTimelineView {
  GtkWidget parent_instance;
  GtkWidget *root_scroller;
  GtkWidget *list_view;
  GtkSelectionModel *selection_model; /* owned */
  GtkStringList *string_model;        /* owned; convenience */
};

G_DEFINE_TYPE(GnostrTimelineView, gnostr_timeline_view, GTK_TYPE_WIDGET)

static void gnostr_timeline_view_dispose(GObject *obj) {
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(obj);
  g_debug("timeline_view dispose: list_view=%p string_model=%p", (void*)self->list_view, (void*)self->string_model);
  if (self->list_view && GTK_IS_LIST_VIEW(self->list_view))
    gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), NULL);
  g_clear_object(&self->selection_model);
  g_clear_object(&self->string_model);
  G_OBJECT_CLASS(gnostr_timeline_view_parent_class)->dispose(obj);
}

static void gnostr_timeline_view_finalize(GObject *obj) {
  gtk_widget_dispose_template(GTK_WIDGET(obj), GNOSTR_TYPE_TIMELINE_VIEW);
  G_OBJECT_CLASS(gnostr_timeline_view_parent_class)->finalize(obj);
}

static void factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkWidget *lbl = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_list_item_set_child(item, lbl);
  g_debug("factory setup: item=%p child=%p", (void*)item, (void*)lbl);
}

static void factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GObject *obj = gtk_list_item_get_item(item);
  const char *text = NULL;
  if (G_IS_OBJECT(obj)) {
    if (GTK_IS_STRING_OBJECT(obj)) {
      text = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
    }
  }
  GtkWidget *lbl = gtk_list_item_get_child(item);
  if (GTK_IS_LABEL(lbl)) gtk_label_set_text(GTK_LABEL(lbl), text ? text : "");
  g_debug("factory bind: item=%p text=%.40s", (void*)item, text ? text : "");
}

static void setup_default_factory(GnostrTimelineView *self) {
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), NULL);
  gtk_list_view_set_factory(GTK_LIST_VIEW(self->list_view), factory);
  g_object_unref(factory);
  g_debug("setup_default_factory: list_view=%p", (void*)self->list_view);
}

static void ensure_string_model(GnostrTimelineView *self) {
  if (self->string_model) return;
  self->string_model = gtk_string_list_new(NULL);
  self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->string_model)));
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
  g_debug("ensure_string_model: string_model=%p selection_model=%p", (void*)self->string_model, (void*)self->selection_model);
}

static void gnostr_timeline_view_class_init(GnostrTimelineViewClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = gnostr_timeline_view_dispose;
  gobj_class->finalize = gnostr_timeline_view_finalize;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, root_scroller);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, list_view);
}

static void gnostr_timeline_view_init(GnostrTimelineView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_debug("timeline_view init: self=%p root_scroller=%p list_view=%p", (void*)self, (void*)self->root_scroller, (void*)self->list_view);
  setup_default_factory(self);
}

GtkWidget *gnostr_timeline_view_new(void) {
  return g_object_new(GNOSTR_TYPE_TIMELINE_VIEW, NULL);
}

void gnostr_timeline_view_set_model(GnostrTimelineView *self, GtkSelectionModel *model) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  if (self->selection_model == model) return;
  if (self->selection_model) g_clear_object(&self->selection_model);
  if (self->string_model) g_clear_object(&self->string_model);
  self->selection_model = model ? g_object_ref(model) : NULL;
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
}

void gnostr_timeline_view_prepend_text(GnostrTimelineView *self, const char *text) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  ensure_string_model(self);
  /* Prepend safely using splice to avoid model/selection/view churn */
  const char *items[] = { text ? text : "", NULL };
  gtk_string_list_splice(self->string_model, 0, 0, items);
  /* Auto-scroll to top so the newly prepended row is visible */
  if (self->root_scroller && GTK_IS_SCROLLED_WINDOW(self->root_scroller)) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
    if (vadj) {
      gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
    }
  }
  g_debug("prepend_text: added=%.40s count=%u", text ? text : "", (unsigned)g_list_model_get_n_items(G_LIST_MODEL(self->string_model)));
}
