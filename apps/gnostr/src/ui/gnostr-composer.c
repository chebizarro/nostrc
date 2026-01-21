#include "gnostr-composer.h"

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-composer.ui"

struct _GnostrComposer {
  GtkWidget parent_instance;
  GtkWidget *root;
  GtkWidget *text_view; /* bound as widget; cast to GtkTextView when used */
  GtkWidget *btn_post;
  GtkWidget *reply_indicator_box; /* container for reply indicator */
  GtkWidget *reply_indicator;     /* label showing "Replying to @user" */
  GtkWidget *btn_cancel_reply;    /* button to cancel reply mode */
  /* Reply context for NIP-10 threading */
  char *reply_to_id;       /* event ID being replied to (hex) */
  char *root_id;           /* thread root event ID (hex), may equal reply_to_id */
  char *reply_to_pubkey;   /* pubkey of author being replied to (hex) */
};

G_DEFINE_TYPE(GnostrComposer, gnostr_composer, GTK_TYPE_WIDGET)

enum {
  SIGNAL_POST_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

static void gnostr_composer_dispose(GObject *obj) {
  /* Dispose template children before chaining up so they are unparented first */
  gtk_widget_dispose_template(GTK_WIDGET(obj), GNOSTR_TYPE_COMPOSER);
  GnostrComposer *self = GNOSTR_COMPOSER(obj);
  self->root = NULL;
  self->text_view = NULL;
  self->btn_post = NULL;
  self->reply_indicator_box = NULL;
  self->reply_indicator = NULL;
  self->btn_cancel_reply = NULL;
  G_OBJECT_CLASS(gnostr_composer_parent_class)->dispose(obj);
}

static void gnostr_composer_finalize(GObject *obj) {
  GnostrComposer *self = GNOSTR_COMPOSER(obj);
  g_clear_pointer(&self->reply_to_id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->reply_to_pubkey, g_free);
  G_OBJECT_CLASS(gnostr_composer_parent_class)->finalize(obj);
}

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

static void on_cancel_reply_clicked(GnostrComposer *self, GtkButton *button) {
  (void)button;
  if (!GNOSTR_IS_COMPOSER(self)) return;
  gnostr_composer_clear_reply_context(self);
}

static void gnostr_composer_class_init(GnostrComposerClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = gnostr_composer_dispose;
  gobj_class->finalize = gnostr_composer_finalize;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, root);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, text_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, btn_post);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, reply_indicator_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, reply_indicator);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, btn_cancel_reply);
  gtk_widget_class_bind_template_callback(widget_class, on_post_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_cancel_reply_clicked);

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
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->text_view),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Composer", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_post),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Composer Post", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_cancel_reply),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Composer Cancel Reply", -1);
  g_message("composer init: self=%p root=%p text_view=%p btn_post=%p",
            (void*)self,
            (void*)self->root,
            (void*)self->text_view,
            (void*)self->btn_post);
}

GtkWidget *gnostr_composer_new(void) {
  return g_object_new(GNOSTR_TYPE_COMPOSER, NULL);
}

void gnostr_composer_clear(GnostrComposer *self) {
  g_return_if_fail(GNOSTR_IS_COMPOSER(self));
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) return;
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  gtk_text_buffer_set_text(buf, "", 0);
  /* Also clear reply context */
  gnostr_composer_clear_reply_context(self);
}

void gnostr_composer_set_reply_context(GnostrComposer *self,
                                       const char *reply_to_id,
                                       const char *root_id,
                                       const char *reply_to_pubkey,
                                       const char *reply_to_display_name) {
  g_return_if_fail(GNOSTR_IS_COMPOSER(self));

  /* Store reply context */
  g_free(self->reply_to_id);
  g_free(self->root_id);
  g_free(self->reply_to_pubkey);

  self->reply_to_id = g_strdup(reply_to_id);
  /* If no root_id provided, use reply_to_id as root (direct reply to root) */
  self->root_id = g_strdup(root_id ? root_id : reply_to_id);
  self->reply_to_pubkey = g_strdup(reply_to_pubkey);

  /* Update reply indicator if present */
  if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
    char *label = g_strdup_printf("Replying to %s",
                                   reply_to_display_name ? reply_to_display_name : "@user");
    gtk_label_set_text(GTK_LABEL(self->reply_indicator), label);
    g_free(label);
  }
  /* Show the reply indicator box */
  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, TRUE);
  }

  /* Update button label */
  if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_post), "Reply");
  }

  g_message("composer: set reply context id=%s root=%s pubkey=%s",
            reply_to_id ? reply_to_id : "(null)",
            self->root_id ? self->root_id : "(null)",
            reply_to_pubkey ? reply_to_pubkey : "(null)");
}

void gnostr_composer_clear_reply_context(GnostrComposer *self) {
  g_return_if_fail(GNOSTR_IS_COMPOSER(self));

  g_clear_pointer(&self->reply_to_id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->reply_to_pubkey, g_free);

  /* Hide reply indicator box */
  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, FALSE);
  }

  /* Reset button label */
  if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_post), "Post");
  }
}

gboolean gnostr_composer_is_reply(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), FALSE);
  return self->reply_to_id != NULL;
}

const char *gnostr_composer_get_reply_to_id(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), NULL);
  return self->reply_to_id;
}

const char *gnostr_composer_get_root_id(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), NULL);
  return self->root_id;
}

const char *gnostr_composer_get_reply_to_pubkey(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), NULL);
  return self->reply_to_pubkey;
}
