/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-composer.c - Group message composer widget
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-composer.h"

struct _GnGroupComposer
{
  GtkBox parent_instance;

  GtkTextView   *text_view;
  GtkButton     *send_button;
  GtkButton     *attach_button;   /* Phase 7: media attachment */
};

enum {
  SIGNAL_SEND_REQUESTED,
  SIGNAL_MEDIA_ATTACH_REQUESTED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnGroupComposer, gn_group_composer, GTK_TYPE_BOX)

/* ── Signal handlers ─────────────────────────────────────────────── */

static void
gn_group_composer_request_media_attach(GnGroupComposer *self)
{
  g_signal_emit(self, signals[SIGNAL_MEDIA_ATTACH_REQUESTED], 0);
}

static void
on_send_clicked(GtkButton *button, gpointer user_data)
{
  GnGroupComposer *self = GN_GROUP_COMPOSER(user_data);

  g_autofree gchar *text = gn_group_composer_get_text(self);
  if (text == NULL || *text == '\0')
    return;

  g_signal_emit(self, signals[SIGNAL_SEND_REQUESTED], 0, text);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode,
               GdkModifierType        state,
               gpointer               user_data)
{
  GnGroupComposer *self = GN_GROUP_COMPOSER(user_data);

  /* Enter sends; Shift+Enter inserts newline */
  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
    {
      if (!(state & GDK_SHIFT_MASK))
        {
          on_send_clicked(self->send_button, self);
          return GDK_EVENT_STOP;
        }
    }

  return GDK_EVENT_PROPAGATE;
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_group_composer_class_init(GnGroupComposerClass *klass)
{
  /**
   * GnGroupComposer::send-requested:
   * @composer: The composer
   * @text: The message text
   *
   * Emitted when the user presses Send or Enter.
   */
  signals[SIGNAL_SEND_REQUESTED] =
    g_signal_new("send-requested",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_STRING);

  signals[SIGNAL_MEDIA_ATTACH_REQUESTED] =
    g_signal_new("media-attach-requested",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 0);
}

static void
gn_group_composer_init(GnGroupComposer *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing(GTK_BOX(self), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 6);

  /* Scrolled text view */
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 40);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 120);
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, FALSE);
  gtk_widget_add_css_class(scroll, "card");

  self->text_view = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_wrap_mode(self->text_view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(self->text_view, 8);
  gtk_text_view_set_right_margin(self->text_view, 8);
  gtk_text_view_set_top_margin(self->text_view, 6);
  gtk_text_view_set_bottom_margin(self->text_view, 6);

  /* Accessibility */
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->text_view),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Message",
                                  -1);

  /* Key controller for Enter-to-send */
  GtkEventController *key_ctl = gtk_event_controller_key_new();
  g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self->text_view), key_ctl);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_WIDGET(self->text_view));
  gtk_box_append(GTK_BOX(self), scroll);

  /* Media attach button (hidden until Phase 7 is enabled) */
  self->attach_button = GTK_BUTTON(gtk_button_new_from_icon_name(
    "mail-attachment-symbolic"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->attach_button),
                               "Attach encrypted media");
  gtk_widget_set_valign(GTK_WIDGET(self->attach_button), GTK_ALIGN_END);
  gtk_widget_add_css_class(GTK_WIDGET(self->attach_button), "flat");
  gtk_widget_add_css_class(GTK_WIDGET(self->attach_button), "circular");
  gtk_widget_set_visible(GTK_WIDGET(self->attach_button), FALSE);
  g_signal_connect_swapped(self->attach_button, "clicked",
                            G_CALLBACK(gn_group_composer_request_media_attach),
                            self);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->attach_button));

  /* Send button */
  self->send_button = GTK_BUTTON(gtk_button_new_from_icon_name(
    "mail-send-symbolic"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->send_button), "Send message");
  gtk_widget_set_valign(GTK_WIDGET(self->send_button), GTK_ALIGN_END);
  gtk_widget_add_css_class(GTK_WIDGET(self->send_button), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(self->send_button), "circular");
  g_signal_connect(self->send_button, "clicked",
                   G_CALLBACK(on_send_clicked), self);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->send_button));
}

/* ── Public API ──────────────────────────────────────────────────── */

GnGroupComposer *
gn_group_composer_new(void)
{
  return g_object_new(GN_TYPE_GROUP_COMPOSER, NULL);
}

gchar *
gn_group_composer_get_text(GnGroupComposer *self)
{
  g_return_val_if_fail(GN_IS_GROUP_COMPOSER(self), NULL);

  GtkTextBuffer *buf = gtk_text_view_get_buffer(self->text_view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
}

void
gn_group_composer_clear(GnGroupComposer *self)
{
  g_return_if_fail(GN_IS_GROUP_COMPOSER(self));

  GtkTextBuffer *buf = gtk_text_view_get_buffer(self->text_view);
  gtk_text_buffer_set_text(buf, "", 0);
}

void
gn_group_composer_set_send_sensitive(GnGroupComposer *self,
                                      gboolean         sensitive)
{
  g_return_if_fail(GN_IS_GROUP_COMPOSER(self));
  gtk_widget_set_sensitive(GTK_WIDGET(self->send_button), sensitive);
}

void
gn_group_composer_set_media_enabled(GnGroupComposer *self,
                                     gboolean         enabled)
{
  g_return_if_fail(GN_IS_GROUP_COMPOSER(self));
  gtk_widget_set_visible(GTK_WIDGET(self->attach_button), enabled);
}
