/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-message-row.c - Row widget for a NIP-29 group message
 */

#include "gn-nip29-message-row.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>
#include <nostr-kinds.h>
#include <string.h>

/* Fallbacks for older kind headers. */
#ifndef NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE
#define NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE 9
#endif
#ifndef NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY
#define NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY 10
#endif
#ifndef NOSTR_KIND_SIMPLE_GROUP_THREAD
#define NOSTR_KIND_SIMPLE_GROUP_THREAD 11
#endif
#ifndef NOSTR_KIND_SIMPLE_GROUP_REPLY
#define NOSTR_KIND_SIMPLE_GROUP_REPLY 12
#endif

struct _GnNip29MessageRow
{
  GtkBox parent_instance;

  GtkLabel *sender_label;
  GtkLabel *content_label;
  GtkLabel *time_label;
  GtkLabel *kind_badge;

  GnNip29MessageItem *item;
  gboolean            is_own;
};

G_DEFINE_TYPE(GnNip29MessageRow, gn_nip29_message_row, GTK_TYPE_BOX)

/* ── Signal for profile/thread navigation hooks ──────────────────── */

enum {
  SIGNAL_PROFILE_ACTIVATED,
  SIGNAL_THREAD_ACTIVATED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void
gn_nip29_message_row_dispose(GObject *object)
{
  GnNip29MessageRow *self = GN_NIP29_MESSAGE_ROW(object);
  g_clear_object(&self->item);
  G_OBJECT_CLASS(gn_nip29_message_row_parent_class)->dispose(object);
}

static void
gn_nip29_message_row_class_init(GnNip29MessageRowClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gn_nip29_message_row_dispose;

  signals[SIGNAL_PROFILE_ACTIVATED] =
    g_signal_new("profile-activated",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_THREAD_ACTIVATED] =
    g_signal_new("thread-activated",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gn_nip29_message_row_init(GnNip29MessageRow *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 2);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 4);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 4);

  /* Header: sender + kind badge + time */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  self->sender_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->sender_label, 0);
  gtk_label_set_ellipsize(self->sender_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(GTK_WIDGET(self->sender_label), "heading");
  gtk_widget_add_css_class(GTK_WIDGET(self->sender_label), "caption");
  gtk_widget_set_hexpand(GTK_WIDGET(self->sender_label), TRUE);
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(self->sender_label));

  self->kind_badge = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->kind_badge), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->kind_badge), "caption");
  gtk_widget_set_visible(GTK_WIDGET(self->kind_badge), FALSE);
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(self->kind_badge));

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
    {
      g_autofree gchar *time = g_date_time_format(dt, "%H:%M");
      return g_strdup_printf("Yesterday %s", time);
    }
  else
    return g_date_time_format(dt, "%b %d, %H:%M");
}

static const char *
kind_badge_text(gint kind)
{
  switch (kind)
    {
    case NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY: return "reply";
    case NOSTR_KIND_SIMPLE_GROUP_THREAD:         return "thread";
    case NOSTR_KIND_SIMPLE_GROUP_REPLY:          return "reply";
    default:                                     return NULL;
    }
}

static gchar *
resolve_display_name(GnostrPluginContext *ctx, const char *pubkey_hex)
{
  if (ctx == NULL || pubkey_hex == NULL)
    return NULL;

  g_autofree gchar *filter = g_strdup_printf(
    "{\"kinds\":[0],\"authors\":[\"%s\"],\"limit\":1}", pubkey_hex);

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) results =
    gnostr_plugin_context_query_events(ctx, filter, &error);
  if (results == NULL || results->len == 0)
    return NULL;

  const gchar *event_json = g_ptr_array_index(results, 0);
  if (event_json == NULL)
    return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL))
    return NULL;

  JsonObject *event_obj = json_node_get_object(json_parser_get_root(parser));
  const gchar *content = json_object_get_string_member_with_default(
    event_obj, "content", NULL);
  if (content == NULL || *content == '\0')
    return NULL;

  g_autoptr(JsonParser) pp = json_parser_new();
  if (!json_parser_load_from_data(pp, content, -1, NULL))
    return NULL;

  JsonObject *profile = json_node_get_object(json_parser_get_root(pp));
  const gchar *dn = json_object_get_string_member_with_default(
    profile, "display_name", NULL);
  if (dn != NULL && *dn != '\0')
    return g_strdup(dn);

  const gchar *name = json_object_get_string_member_with_default(
    profile, "name", NULL);
  if (name != NULL && *name != '\0')
    return g_strdup(name);

  return NULL;
}

/* ── Public API ──────────────────────────────────────────────────── */

GnNip29MessageRow *
gn_nip29_message_row_new(void)
{
  return g_object_new(GN_TYPE_NIP29_MESSAGE_ROW, NULL);
}

void
gn_nip29_message_row_bind(GnNip29MessageRow   *self,
                           GnNip29MessageItem  *item,
                           const char          *user_pubkey_hex,
                           GnostrPluginContext *plugin_context)
{
  g_return_if_fail(GN_IS_NIP29_MESSAGE_ROW(self));
  g_return_if_fail(GN_IS_NIP29_MESSAGE_ITEM(item));

  g_set_object(&self->item, item);

  const char *pubkey = gn_nip29_message_item_get_pubkey(item);
  const char *content = gn_nip29_message_item_get_content(item);
  gint64 created_at = gn_nip29_message_item_get_created_at(item);
  gint kind = gn_nip29_message_item_get_kind(item);

  self->is_own = (user_pubkey_hex != NULL &&
                  g_strcmp0(pubkey, user_pubkey_hex) == 0);

  /* Sender */
  if (self->is_own)
    {
      gtk_label_set_text(self->sender_label, "You");
    }
  else
    {
      g_autofree gchar *display = resolve_display_name(plugin_context, pubkey);
      if (display != NULL)
        gtk_label_set_text(self->sender_label, display);
      else if (pubkey != NULL && strlen(pubkey) >= 8)
        {
          g_autofree gchar *short_pk = g_strdup_printf("%.8s…", pubkey);
          gtk_label_set_text(self->sender_label, short_pk);
        }
      else
        gtk_label_set_text(self->sender_label, "Unknown");
    }

  /* Content */
  gtk_label_set_text(self->content_label, content ? content : "");

  /* Timestamp */
  g_autofree gchar *ts = format_timestamp(created_at);
  gtk_label_set_text(self->time_label, ts);

  /* Kind badge for thread/reply types */
  const char *badge = kind_badge_text(kind);
  if (badge != NULL)
    {
      gtk_label_set_text(self->kind_badge, badge);
      gtk_widget_set_visible(GTK_WIDGET(self->kind_badge), TRUE);
    }
  else
    {
      gtk_widget_set_visible(GTK_WIDGET(self->kind_badge), FALSE);
    }

  /* Own-message styling */
  if (self->is_own)
    {
      gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_END);
      gtk_widget_add_css_class(GTK_WIDGET(self), "nip29-own-message");
    }
  else
    {
      gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_START);
      gtk_widget_remove_css_class(GTK_WIDGET(self), "nip29-own-message");
    }
}

void
gn_nip29_message_row_unbind(GnNip29MessageRow *self)
{
  g_return_if_fail(GN_IS_NIP29_MESSAGE_ROW(self));

  g_clear_object(&self->item);
  gtk_label_set_text(self->sender_label, NULL);
  gtk_label_set_text(self->content_label, NULL);
  gtk_label_set_text(self->time_label, NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->kind_badge), FALSE);
  gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_FILL);
  gtk_widget_remove_css_class(GTK_WIDGET(self), "nip29-own-message");
  self->is_own = FALSE;
}
