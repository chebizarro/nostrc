/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-message-row.c - Group chat message bubble widget
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-message-row.h"
#include <glib/gi18n.h>

struct _GnGroupMessageRow
{
  GtkBox parent_instance;

  /* Widgets */
  GtkLabel *sender_label;
  GtkLabel *content_label;
  GtkLabel *time_label;

  /* Bound data */
  MarmotGobjectMessage *message;   /* strong ref, nullable */
  gboolean              is_own;    /* TRUE if from the local user */
};

G_DEFINE_TYPE(GnGroupMessageRow, gn_group_message_row, GTK_TYPE_BOX)

static void
gn_group_message_row_dispose(GObject *object)
{
  GnGroupMessageRow *self = GN_GROUP_MESSAGE_ROW(object);
  g_clear_object(&self->message);
  G_OBJECT_CLASS(gn_group_message_row_parent_class)->dispose(object);
}

static void
gn_group_message_row_class_init(GnGroupMessageRowClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_group_message_row_dispose;
}

static void
gn_group_message_row_init(GnGroupMessageRow *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 2);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 4);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 4);

  /* Header row: sender + time */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  self->sender_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->sender_label, 0);
  gtk_label_set_ellipsize(self->sender_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(GTK_WIDGET(self->sender_label), "heading");
  gtk_widget_add_css_class(GTK_WIDGET(self->sender_label), "caption");
  gtk_widget_set_hexpand(GTK_WIDGET(self->sender_label), TRUE);
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(self->sender_label));

  self->time_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->time_label, 1);
  gtk_widget_add_css_class(GTK_WIDGET(self->time_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->time_label), "caption");
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(self->time_label));

  gtk_box_append(GTK_BOX(self), header);

  /* Content */
  self->content_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->content_label, 0);
  gtk_label_set_wrap(self->content_label, TRUE);
  gtk_label_set_wrap_mode(self->content_label, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable(self->content_label, TRUE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->content_label));
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static gchar *
format_timestamp(gint64 created_at)
{
  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(created_at);
  if (dt == NULL)
    return g_strdup("??:??");

  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  gint days = g_date_time_get_day_of_year(now) - g_date_time_get_day_of_year(dt);
  gint years = g_date_time_get_year(now) - g_date_time_get_year(dt);

  if (years == 0 && days == 0)
    return g_date_time_format(dt, "%H:%M");
  else if (years == 0 && days == 1)
    return g_strdup_printf("Yesterday %s", g_date_time_format(dt, "%H:%M"));
  else
    return g_date_time_format(dt, "%b %d, %H:%M");
}

/* ── Public API ──────────────────────────────────────────────────── */

GnGroupMessageRow *
gn_group_message_row_new(void)
{
  return g_object_new(GN_TYPE_GROUP_MESSAGE_ROW, NULL);
}

void
gn_group_message_row_bind(GnGroupMessageRow    *self,
                           MarmotGobjectMessage *message,
                           const gchar          *user_pubkey_hex)
{
  g_return_if_fail(GN_IS_GROUP_MESSAGE_ROW(self));
  g_return_if_fail(MARMOT_GOBJECT_IS_MESSAGE(message));

  g_set_object(&self->message, message);

  const gchar *sender_hex = marmot_gobject_message_get_pubkey_hex(message);
  const gchar *content    = marmot_gobject_message_get_content(message);
  gint64       created_at = marmot_gobject_message_get_created_at(message);

  self->is_own = (user_pubkey_hex != NULL &&
                  g_strcmp0(sender_hex, user_pubkey_hex) == 0);

  /* Sender label: show truncated pubkey (TODO: resolve to display name) */
  if (self->is_own)
    {
      gtk_label_set_text(self->sender_label, "You");
    }
  else if (sender_hex && strlen(sender_hex) >= 16)
    {
      g_autofree gchar *short_pk = g_strndup(sender_hex, 8);
      g_autofree gchar *display = g_strdup_printf("%.8s…", short_pk);
      gtk_label_set_text(self->sender_label, display);
    }
  else
    {
      gtk_label_set_text(self->sender_label, "Unknown");
    }

  /* Content */
  gtk_label_set_text(self->content_label, content ? content : "");

  /* Timestamp */
  g_autofree gchar *ts = format_timestamp(created_at);
  gtk_label_set_text(self->time_label, ts);

  /* Style: own messages get a distinct alignment */
  if (self->is_own)
    {
      gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_END);
      gtk_widget_add_css_class(GTK_WIDGET(self), "mls-own-message");
    }
  else
    {
      gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_START);
      gtk_widget_remove_css_class(GTK_WIDGET(self), "mls-own-message");
    }
}

void
gn_group_message_row_unbind(GnGroupMessageRow *self)
{
  g_return_if_fail(GN_IS_GROUP_MESSAGE_ROW(self));

  g_clear_object(&self->message);
  gtk_label_set_text(self->sender_label, NULL);
  gtk_label_set_text(self->content_label, NULL);
  gtk_label_set_text(self->time_label, NULL);
  gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_FILL);
  gtk_widget_remove_css_class(GTK_WIDGET(self), "mls-own-message");
  self->is_own = FALSE;
}
