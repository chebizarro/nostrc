#include "gn-communikeys-composer.h"

struct _GnCommunikeysComposer {
  GtkBox parent_instance;
  GtkDropDown *kind_dropdown;
  GtkTextView *text_view;
  GtkButton *send_button;
  int kinds[2];
  guint n_kinds;
};
enum { SEND_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];
G_DEFINE_TYPE(GnCommunikeysComposer, gn_communikeys_composer, GTK_TYPE_BOX)

static void request_send(GnCommunikeysComposer *self) {
  g_autofree gchar *text = gn_communikeys_composer_get_text(self);
  g_strstrip(text);
  if (text && *text && self->n_kinds > 0) {
    guint selected = gtk_drop_down_get_selected(self->kind_dropdown);
    if (selected >= self->n_kinds) selected = 0;
    g_signal_emit(self, signals[SEND_REQUESTED], 0,
                  self->kinds[selected], text);
  }
}
static void on_send(GtkButton *button, gpointer user_data) {
  (void)button;
  request_send(GN_COMMUNIKEYS_COMPOSER(user_data));
}
static gboolean on_key(GtkEventControllerKey *controller, guint keyval,
                       guint keycode, GdkModifierType state,
                       gpointer user_data) {
  (void)controller;
  (void)keycode;
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
      !(state & GDK_SHIFT_MASK)) {
    request_send(GN_COMMUNIKEYS_COMPOSER(user_data));
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}
static void gn_communikeys_composer_class_init(
    GnCommunikeysComposerClass *klass) {
  signals[SEND_REQUESTED] = g_signal_new(
    "send-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 2,
    G_TYPE_INT, G_TYPE_STRING);
}
static void gn_communikeys_composer_init(GnCommunikeysComposer *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self),
                                 GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing(GTK_BOX(self), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 6);

  self->kind_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->kind_dropdown),
                              "Publication kind");
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->kind_dropdown));
  gn_communikeys_composer_set_allowed_kinds(self, TRUE, TRUE);

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
  gtk_accessible_update_property(
    GTK_ACCESSIBLE(self->text_view), GTK_ACCESSIBLE_PROPERTY_LABEL,
    "Community message", -1);
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), self);
  gtk_widget_add_controller(GTK_WIDGET(self->text_view), keys);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                GTK_WIDGET(self->text_view));
  gtk_box_append(GTK_BOX(self), scroll);

  self->send_button = GTK_BUTTON(
    gtk_button_new_from_icon_name("mail-send-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(self->send_button), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(self->send_button), "circular");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->send_button),
                              "Send message (Enter)");
  gtk_widget_set_valign(GTK_WIDGET(self->send_button), GTK_ALIGN_END);
  g_signal_connect(self->send_button, "clicked", G_CALLBACK(on_send), self);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->send_button));
}
GnCommunikeysComposer *gn_communikeys_composer_new(void) {
  return g_object_new(GN_TYPE_COMMUNIKEYS_COMPOSER, NULL);
}
gchar *gn_communikeys_composer_get_text(GnCommunikeysComposer *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMPOSER(self), NULL);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->text_view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}
void gn_communikeys_composer_set_text(GnCommunikeysComposer *self,
                                      const char *text) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMPOSER(self));
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->text_view),
                           text ? text : "", -1);
}
void gn_communikeys_composer_clear(GnCommunikeysComposer *self) {
  gn_communikeys_composer_set_text(self, "");
}
void gn_communikeys_composer_set_allowed_kinds(
    GnCommunikeysComposer *self, gboolean allow_kind_9,
    gboolean allow_kind_11) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMPOSER(self));
  GtkStringList *labels = gtk_string_list_new(NULL);
  self->n_kinds = 0;
  if (allow_kind_9) {
    self->kinds[self->n_kinds++] = 9;
    gtk_string_list_append(labels, "Chat");
  }
  if (allow_kind_11) {
    self->kinds[self->n_kinds++] = 11;
    gtk_string_list_append(labels, "Thread");
  }
  gtk_drop_down_set_model(self->kind_dropdown, G_LIST_MODEL(labels));
  gtk_drop_down_set_selected(self->kind_dropdown, 0);
  gtk_widget_set_visible(GTK_WIDGET(self->kind_dropdown),
                         self->n_kinds > 1);
  g_object_unref(labels);
}

void gn_communikeys_composer_set_send_sensitive(
    GnCommunikeysComposer *self, gboolean sensitive) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMPOSER(self));
  gtk_widget_set_sensitive(GTK_WIDGET(self->send_button), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(self->text_view), sensitive);
}
