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

static void factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkWidget *lbl = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_list_item_set_child(item, lbl);
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
}

static void setup_default_factory(GnostrTimelineView *self) {
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), NULL);
  gtk_list_view_set_factory(GTK_LIST_VIEW(self->list_view), factory);
  g_object_unref(factory);
}

static void ensure_string_model(GnostrTimelineView *self) {
  if (self->string_model) return;
  self->string_model = gtk_string_list_new(NULL);
  self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->string_model)));
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
}

static void gnostr_timeline_view_class_init(GnostrTimelineViewClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, root_scroller);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, list_view);
}

static void gnostr_timeline_view_init(GnostrTimelineView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
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
  /* GtkStringList has only append/remove API; emulate prepend by inserting at 0 via internal API: use GListModel if needed */
  /* Simple approach: create a new list with the new item first, then copy old items (fast enough for prototype). */
  GtkStringList *old = self->string_model;
  GtkStringList *nl = gtk_string_list_new(NULL);
  gtk_string_list_append(nl, text ? text : "");
  guint n = g_list_model_get_n_items(G_LIST_MODEL(old));
  for (guint i=0;i<n;i++) {
    const char *s = gtk_string_list_get_string(old, i);
    gtk_string_list_append(nl, s);
  }
  /* Swap in */
  self->string_model = nl;
  if (self->selection_model) g_clear_object(&self->selection_model);
  self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->string_model)));
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
  g_clear_object(&old);
}
