#include "gn-concord-composer.h"

struct _GnConcordComposer {
  GtkBox parent_instance;
  GtkTextView *text_view;
  GtkButton *send_button;
};

enum { SEND_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnConcordComposer, gn_concord_composer, GTK_TYPE_BOX)

static void request_send(GnConcordComposer *self) {
  g_autofree gchar *text = gn_concord_composer_get_text(self);
  if (!text) return;
  g_strstrip(text);
  /* Empty content is "" on the wire, but it is never a NIP-44 plaintext —
   * refuse before the service has to. */
  if (*text) g_signal_emit(self, signals[SEND_REQUESTED], 0, text);
}

static void on_send(GtkButton *button, gpointer user_data) {
  (void)button;
  request_send(GN_CONCORD_COMPOSER(user_data));
}

static gboolean on_key(GtkEventControllerKey *controller, guint keyval,
                       guint keycode, GdkModifierType state,
                       gpointer user_data) {
  (void)controller;
  (void)keycode;
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
      !(state & GDK_SHIFT_MASK)) {
    request_send(GN_CONCORD_COMPOSER(user_data));
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static void gn_concord_composer_class_init(GnConcordComposerClass *klass) {
  signals[SEND_REQUESTED] = g_signal_new(
    "send-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
    NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gn_concord_composer_init(GnConcordComposer *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self),
                                 GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing(GTK_BOX(self), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 6);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 44);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 120);
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_add_css_class(scroll, "card");

  self->text_view = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_wrap_mode(self->text_view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(self->text_view, 8);
  gtk_text_view_set_right_margin(self->text_view, 8);
  gtk_text_view_set_top_margin(self->text_view, 6);
  gtk_text_view_set_bottom_margin(self->text_view, 6);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->text_view),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Encrypted channel message", -1);
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), self);
  gtk_widget_add_controller(GTK_WIDGET(self->text_view), keys);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                GTK_WIDGET(self->text_view));
  gtk_box_append(GTK_BOX(self), scroll);

  self->send_button =
    GTK_BUTTON(gtk_button_new_from_icon_name("mail-send-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(self->send_button), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(self->send_button), "circular");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->send_button),
                              "Send encrypted message (Enter)");
  gtk_widget_set_valign(GTK_WIDGET(self->send_button), GTK_ALIGN_END);
  g_signal_connect(self->send_button, "clicked", G_CALLBACK(on_send), self);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->send_button));
}

GnConcordComposer *gn_concord_composer_new(void) {
  return g_object_new(GN_TYPE_CONCORD_COMPOSER, NULL);
}

gchar *gn_concord_composer_get_text(GnConcordComposer *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMPOSER(self), NULL);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->text_view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

void gn_concord_composer_set_text(GnConcordComposer *self, const char *text) {
  g_return_if_fail(GN_IS_CONCORD_COMPOSER(self));
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->text_view),
                           text ? text : "", -1);
}

void gn_concord_composer_clear(GnConcordComposer *self) {
  gn_concord_composer_set_text(self, "");
}

void gn_concord_composer_set_send_sensitive(GnConcordComposer *self,
                                            gboolean sensitive) {
  g_return_if_fail(GN_IS_CONCORD_COMPOSER(self));
  gtk_widget_set_sensitive(GTK_WIDGET(self->send_button), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(self->text_view), sensitive);
}
