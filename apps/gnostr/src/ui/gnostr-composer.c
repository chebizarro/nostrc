#include "gnostr-composer.h"

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-composer.ui"

struct _GnostrComposer {
  GtkWidget parent_instance;
  GtkWidget *root;
  GtkWidget *text_view; /* bound as widget; cast to GtkTextView when used */
  GtkWidget *btn_post;
};

G_DEFINE_TYPE(GnostrComposer, gnostr_composer, GTK_TYPE_WIDGET)

enum {
  SIGNAL_POST_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

static void on_post_clicked(GnostrComposer *self, GtkButton *button) {
  (void)button;
  // Read text from GtkTextView and emit signal to controller
  if (!self || !GTK_IS_WIDGET(self)) {
    g_warning("composer instance invalid in on_post_clicked");
    return;
  }
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) {
    g_warning("composer text_view not ready (self=%p root=%p text_view=%p btn_post=%p)",
              (void*)self,
              (void*)self->root,
              (void*)self->text_view,
              (void*)self->btn_post);
    return;
  }
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  char *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
  if (signals[SIGNAL_POST_REQUESTED] > 0)
    g_signal_emit(self, signals[SIGNAL_POST_REQUESTED], 0, text);
  g_free(text);
}

static void gnostr_composer_class_init(GnostrComposerClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, root);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, text_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, btn_post);
  gtk_widget_class_bind_template_callback(widget_class, on_post_clicked);

  signals[SIGNAL_POST_REQUESTED] =
      g_signal_new("post-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, /* class offset */
                   NULL, NULL,
                   g_cclosure_marshal_VOID__STRING,
                   G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_composer_init(GnostrComposer *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_message("composer init: self=%p root=%p text_view=%p btn_post=%p",
            (void*)self,
            (void*)self->root,
            (void*)self->text_view,
            (void*)self->btn_post);
}

GtkWidget *gnostr_composer_new(void) {
  return g_object_new(GNOSTR_TYPE_COMPOSER, NULL);
}
