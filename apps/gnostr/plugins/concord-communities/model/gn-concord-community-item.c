#include "gn-concord-community-item.h"

struct _GnConcordCommunityItem {
  GObject parent_instance;
  gchar *community_id;
  gchar *owner;
  gchar *name;
  guint64 root_epoch;
  gboolean has_control_pk;
  GListStore *channels;
  GPtrArray *relays; /* char *, NULL-terminated for the const char *const * view */
};

G_DEFINE_TYPE(GnConcordCommunityItem, gn_concord_community_item, G_TYPE_OBJECT)

static void gn_concord_community_item_dispose(GObject *object) {
  GnConcordCommunityItem *self = GN_CONCORD_COMMUNITY_ITEM(object);
  g_clear_object(&self->channels);
  G_OBJECT_CLASS(gn_concord_community_item_parent_class)->dispose(object);
}

static void gn_concord_community_item_finalize(GObject *object) {
  GnConcordCommunityItem *self = GN_CONCORD_COMMUNITY_ITEM(object);
  g_free(self->community_id);
  g_free(self->owner);
  g_free(self->name);
  g_clear_pointer(&self->relays, g_ptr_array_unref);
  G_OBJECT_CLASS(gn_concord_community_item_parent_class)->finalize(object);
}

static void gn_concord_community_item_class_init(
    GnConcordCommunityItemClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gn_concord_community_item_dispose;
  object_class->finalize = gn_concord_community_item_finalize;
}

static void gn_concord_community_item_init(GnConcordCommunityItem *self) {
  self->channels = g_list_store_new(GN_TYPE_CONCORD_CHANNEL_ITEM);
  self->relays = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(self->relays, NULL);
}

GnConcordCommunityItem *gn_concord_community_item_new(const char *community_id,
                                                      const char *owner,
                                                      const char *name,
                                                      guint64 root_epoch,
                                                      gboolean has_control_pk) {
  GnConcordCommunityItem *self =
    g_object_new(GN_TYPE_CONCORD_COMMUNITY_ITEM, NULL);
  self->community_id = g_strdup(community_id);
  self->owner = g_strdup(owner);
  self->name = g_strdup(name);
  self->root_epoch = root_epoch;
  self->has_control_pk = has_control_pk;
  return self;
}

const char *gn_concord_community_item_get_community_id(
    GnConcordCommunityItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), NULL);
  return self->community_id;
}
const char *gn_concord_community_item_get_owner(GnConcordCommunityItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), NULL);
  return self->owner;
}
const char *gn_concord_community_item_get_name(GnConcordCommunityItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), NULL);
  return self->name;
}
guint64 gn_concord_community_item_get_root_epoch(GnConcordCommunityItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), 0);
  return self->root_epoch;
}
gboolean gn_concord_community_item_get_has_control_pk(
    GnConcordCommunityItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), FALSE);
  return self->has_control_pk;
}

GListModel *gn_concord_community_item_get_channels(
    GnConcordCommunityItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), NULL);
  return G_LIST_MODEL(self->channels);
}

void gn_concord_community_item_add_channel(GnConcordCommunityItem *self,
                                           GnConcordChannelItem *channel) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self));
  g_return_if_fail(GN_IS_CONCORD_CHANNEL_ITEM(channel));
  g_list_store_append(self->channels, channel);
}

GnConcordChannelItem *gn_concord_community_item_find_channel(
    GnConcordCommunityItem *self, const char *channel_id) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), NULL);
  if (!channel_id) return NULL;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->channels));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnConcordChannelItem) channel =
      g_list_model_get_item(G_LIST_MODEL(self->channels), i);
    if (g_strcmp0(gn_concord_channel_item_get_id(channel), channel_id) == 0)
      return g_steal_pointer(&channel);
  }
  return NULL;
}

guint gn_concord_community_item_get_channel_count(
    GnConcordCommunityItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), 0);
  return g_list_model_get_n_items(G_LIST_MODEL(self->channels));
}

void gn_concord_community_item_set_relays(GnConcordCommunityItem *self,
                                          const char *const *relays,
                                          guint n_relays) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self));
  g_ptr_array_set_size(self->relays, 0);
  for (guint i = 0; relays && i < n_relays; i++)
    if (relays[i]) g_ptr_array_add(self->relays, g_strdup(relays[i]));
  g_ptr_array_add(self->relays, NULL);
}

const char *const *gn_concord_community_item_get_relays(
    GnConcordCommunityItem *self, guint *n_relays) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), NULL);
  if (n_relays) *n_relays = self->relays->len ? self->relays->len - 1 : 0;
  return (const char *const *)self->relays->pdata;
}

const char *gn_concord_community_item_get_primary_relay(
    GnConcordCommunityItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(self), NULL);
  return self->relays->len > 1 ? g_ptr_array_index(self->relays, 0) : NULL;
}
