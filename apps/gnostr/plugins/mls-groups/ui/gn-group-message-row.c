/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-message-row.c - Group chat message bubble widget
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-message-row.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>
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

/* ── Profile name resolution ─────────────────────────────────────── */

/**
 * resolve_display_name:
 * @plugin_context: (nullable): Plugin context for nostrdb queries
 * @pubkey_hex: 64-char hex pubkey of the sender
 *
 * Attempt to resolve a display name from the local nostrdb cache.
 * Queries for a kind:0 (profile metadata) event authored by the pubkey.
 *
 * Returns: (transfer full) (nullable): Display name, or %NULL if not found.
 */
static gchar *
resolve_display_name(GnostrPluginContext *plugin_context,
                     const gchar        *pubkey_hex)
{
  if (plugin_context == NULL || pubkey_hex == NULL)
    return NULL;

  /* Build a NIP-01 filter for kind:0 from this author */
  g_autofree gchar *filter = g_strdup_printf(
    "{\"kinds\":[0],\"authors\":[\"%s\"],\"limit\":1}", pubkey_hex);

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) results =
    gnostr_plugin_context_query_events(plugin_context, filter, &error);

  if (results == NULL || results->len == 0)
    return NULL;

  const gchar *event_json = g_ptr_array_index(results, 0);
  if (event_json == NULL)
    return NULL;

  /* Parse the content field which contains the profile JSON */
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL))
    return NULL;

  JsonObject *event_obj = json_node_get_object(json_parser_get_root(parser));
  const gchar *content = json_object_get_string_member_with_default(
    event_obj, "content", NULL);
  if (content == NULL || *content == '\0')
    return NULL;

  /* Parse the profile content JSON for "display_name" or "name" */
  g_autoptr(JsonParser) profile_parser = json_parser_new();
  if (!json_parser_load_from_data(profile_parser, content, -1, NULL))
    return NULL;

  JsonObject *profile = json_node_get_object(
    json_parser_get_root(profile_parser));

  const gchar *display_name = json_object_get_string_member_with_default(
    profile, "display_name", NULL);
  if (display_name != NULL && *display_name != '\0')
    return g_strdup(display_name);

  const gchar *name = json_object_get_string_member_with_default(
    profile, "name", NULL);
  if (name != NULL && *name != '\0')
    return g_strdup(name);

  return NULL;
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
                           const gchar          *user_pubkey_hex,
                           GnostrPluginContext  *plugin_context)
{
  g_return_if_fail(GN_IS_GROUP_MESSAGE_ROW(self));
  g_return_if_fail(MARMOT_GOBJECT_IS_MESSAGE(message));

  g_set_object(&self->message, message);

  const gchar *sender_hex = marmot_gobject_message_get_pubkey(message);
  const gchar *content    = marmot_gobject_message_get_content(message);
  gint64       created_at = marmot_gobject_message_get_created_at(message);

  self->is_own = (user_pubkey_hex != NULL &&
                  g_strcmp0(sender_hex, user_pubkey_hex) == 0);

  /* Sender label: resolve display name from nostrdb profile cache */
  if (self->is_own)
    {
      gtk_label_set_text(self->sender_label, "You");
    }
  else
    {
      g_autofree gchar *display_name =
        resolve_display_name(plugin_context, sender_hex);

      if (display_name != NULL)
        {
          gtk_label_set_text(self->sender_label, display_name);
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
