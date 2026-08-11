#include "gn-concord-message-item.h"

struct _GnConcordMessageItem {
  GObject parent_instance;
  gchar *rumor_id;
  gchar *author;
  gchar *content;
  gint64 created_at;
  int ms;
  gint64 order_key;
  int kind;
};

G_DEFINE_TYPE(GnConcordMessageItem, gn_concord_message_item, G_TYPE_OBJECT)

static void gn_concord_message_item_finalize(GObject *object) {
  GnConcordMessageItem *self = GN_CONCORD_MESSAGE_ITEM(object);
  g_free(self->rumor_id);
  g_free(self->author);
  g_free(self->content);
  G_OBJECT_CLASS(gn_concord_message_item_parent_class)->finalize(object);
}

static void gn_concord_message_item_class_init(
    GnConcordMessageItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gn_concord_message_item_finalize;
}

static void gn_concord_message_item_init(GnConcordMessageItem *self) {
  (void)self;
}

GnConcordMessageItem *gn_concord_message_item_new(const char *rumor_id,
                                                  const char *author,
                                                  const char *content,
                                                  gint64 created_at,
                                                  int ms,
                                                  gint64 order_key,
                                                  int kind) {
  GnConcordMessageItem *self = g_object_new(GN_TYPE_CONCORD_MESSAGE_ITEM, NULL);
  self->rumor_id = g_strdup(rumor_id);
  self->author = g_strdup(author);
  self->content = g_strdup(content);
  self->created_at = created_at;
  self->ms = ms;
  self->order_key = order_key;
  self->kind = kind;
  return self;
}

const char *gn_concord_message_item_get_rumor_id(GnConcordMessageItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_MESSAGE_ITEM(self), NULL);
  return self->rumor_id;
}
const char *gn_concord_message_item_get_author(GnConcordMessageItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_MESSAGE_ITEM(self), NULL);
  return self->author;
}
const char *gn_concord_message_item_get_content(GnConcordMessageItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_MESSAGE_ITEM(self), NULL);
  return self->content;
}
gint64 gn_concord_message_item_get_created_at(GnConcordMessageItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_MESSAGE_ITEM(self), 0);
  return self->created_at;
}
int gn_concord_message_item_get_ms(GnConcordMessageItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_MESSAGE_ITEM(self), 0);
  return self->ms;
}
gint64 gn_concord_message_item_get_order_key(GnConcordMessageItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_MESSAGE_ITEM(self), 0);
  return self->order_key;
}
int gn_concord_message_item_get_kind(GnConcordMessageItem *self) {
  g_return_val_if_fail(GN_IS_CONCORD_MESSAGE_ITEM(self), 0);
  return self->kind;
}
