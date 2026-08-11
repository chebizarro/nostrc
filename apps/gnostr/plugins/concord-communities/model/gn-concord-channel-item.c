#include "gn-concord-channel-item.h"

#include <string.h>

struct _GnConcordChannelItem {
  GObject parent_instance;
  gchar *id;
  gchar *key;
  guint64 epoch;
  gchar *name;
  gboolean is_private;
  gchar *stream_pubkey;
};

G_DEFINE_TYPE(GnConcordChannelItem, gn_concord_channel_item, G_TYPE_OBJECT)

static void gn_concord_channel_item_finalize(GObject *object) {
  GnConcordChannelItem *self = GN_CONCORD_CHANNEL_ITEM(object);
  g_free(self->id);
  /* The Channel key is a membership credential (NIP-CAS-0008 Security §1):
   * scrub it rather than returning it to the allocator intact. */
  if (self->key) {
    memset(self->key, 0, strlen(self->key));
    g_free(self->key);
  }
  g_free(self->name);
  g_free(self->stream_pubkey);
  G_OBJECT_CLASS(gn_concord_channel_item_parent_class)->finalize(object);
}

static void gn_concord_channel_item_class_init(GnConcordChannelItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gn_concord_channel_item_finalize;
}

static void gn_concord_channel_item_init(GnConcordChannelItem *self) {
  (void)self;
}

GnConcordChannelItem *gn_concord_channel_item_new(const char *id_hex,
                                                  const char *key_hex,
                                                  guint64 epoch,
                                                  const char *name,
                                                  gboolean is_private) {
  GnConcordChannelItem *self = g_object_new(GN_TYPE_CONCORD_CHANNEL_ITEM, NULL);
  self->id = g_strdup(id_hex);
  self->key = g_strdup(key_hex);
  self->epoch = epoch;
  self->name = g_strdup(name);
  self->is_private = is_private;
  return self;
}

const char *gn_concord_channel_item_get_id(GnConcordChannelItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(self), NULL);
  return self->id;
}
const char *gn_concord_channel_item_get_key(GnConcordChannelItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(self), NULL);
  return self->key;
}
guint64 gn_concord_channel_item_get_epoch(GnConcordChannelItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(self), 0);
  return self->epoch;
}
const char *gn_concord_channel_item_get_name(GnConcordChannelItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(self), NULL);
  return self->name;
}
gboolean gn_concord_channel_item_get_is_private(GnConcordChannelItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(self), FALSE);
  return self->is_private;
}
void gn_concord_channel_item_set_name(GnConcordChannelItem *self,
                                      const char *name) {
  g_return_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(self));
  if (g_strcmp0(self->name, name) == 0) return;
  g_free(self->name);
  self->name = g_strdup(name);
}
void gn_concord_channel_item_set_is_private(GnConcordChannelItem *self,
                                            gboolean is_private) {
  g_return_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(self));
  self->is_private = is_private;
}
const char *gn_concord_channel_item_get_stream_pubkey(
    GnConcordChannelItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(self), NULL);
  return self->stream_pubkey;
}
void gn_concord_channel_item_set_stream_pubkey(GnConcordChannelItem *self,
                                               const char *pubkey_hex) {
  g_return_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(self));
  g_free(self->stream_pubkey);
  self->stream_pubkey = g_strdup(pubkey_hex);
}
