/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-message-item.c - GObject representing a NIP-29 group message
 */

#include "gn-nip29-message-item.h"

struct _GnNip29MessageItem
{
  GObject parent_instance;

  gchar  *id;
  gchar  *event_json;
  gint64  created_at;
  gint    kind;
  gchar  *pubkey;
  gchar  *content;
};

G_DEFINE_TYPE(GnNip29MessageItem, gn_nip29_message_item, G_TYPE_OBJECT)

static void
gn_nip29_message_item_finalize(GObject *object)
{
  GnNip29MessageItem *self = GN_NIP29_MESSAGE_ITEM(object);

  g_free(self->id);
  g_free(self->event_json);
  g_free(self->pubkey);
  g_free(self->content);

  G_OBJECT_CLASS(gn_nip29_message_item_parent_class)->finalize(object);
}

static void
gn_nip29_message_item_class_init(GnNip29MessageItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gn_nip29_message_item_finalize;
}

static void
gn_nip29_message_item_init(GnNip29MessageItem *self)
{
}

GnNip29MessageItem *
gn_nip29_message_item_new(const char *id,
                           const char *event_json,
                           gint64      created_at,
                           gint        kind,
                           const char *pubkey,
                           const char *content)
{
  GnNip29MessageItem *self = g_object_new(GN_TYPE_NIP29_MESSAGE_ITEM, NULL);

  self->id = g_strdup(id);
  self->event_json = g_strdup(event_json);
  self->created_at = created_at;
  self->kind = kind;
  self->pubkey = g_strdup(pubkey);
  self->content = g_strdup(content);

  return self;
}

const char *gn_nip29_message_item_get_id(GnNip29MessageItem *self)
{ return self->id; }

const char *gn_nip29_message_item_get_event_json(GnNip29MessageItem *self)
{ return self->event_json; }

gint64 gn_nip29_message_item_get_created_at(GnNip29MessageItem *self)
{ return self->created_at; }

gint gn_nip29_message_item_get_kind(GnNip29MessageItem *self)
{ return self->kind; }

const char *gn_nip29_message_item_get_pubkey(GnNip29MessageItem *self)
{ return self->pubkey; }

const char *gn_nip29_message_item_get_content(GnNip29MessageItem *self)
{ return self->content; }
