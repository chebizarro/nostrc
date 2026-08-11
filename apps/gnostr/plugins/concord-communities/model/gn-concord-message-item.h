#ifndef GN_CONCORD_MESSAGE_ITEM_H
#define GN_CONCORD_MESSAGE_ITEM_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GN_TYPE_CONCORD_MESSAGE_ITEM (gn_concord_message_item_get_type())
G_DECLARE_FINAL_TYPE(GnConcordMessageItem, gn_concord_message_item,
                     GN, CONCORD_MESSAGE_ITEM, GObject)

/* One decrypted Chat Plane rumor. `rumor_id` is the *inner* event's id, which
 * is what Concord compares — the outer wrap's id differs per re-wrap
 * (CORD-02 §5). `order_key` is created_at * 1000 + ms, the protocol's single
 * ordering basis (CORD-02 §4). */
GnConcordMessageItem *gn_concord_message_item_new(const char *rumor_id,
                                                  const char *author,
                                                  const char *content,
                                                  gint64 created_at,
                                                  int ms,
                                                  gint64 order_key,
                                                  int kind);

const char *gn_concord_message_item_get_rumor_id(GnConcordMessageItem *self);
const char *gn_concord_message_item_get_author(GnConcordMessageItem *self);
const char *gn_concord_message_item_get_content(GnConcordMessageItem *self);
gint64 gn_concord_message_item_get_created_at(GnConcordMessageItem *self);
int gn_concord_message_item_get_ms(GnConcordMessageItem *self);
gint64 gn_concord_message_item_get_order_key(GnConcordMessageItem *self);
int gn_concord_message_item_get_kind(GnConcordMessageItem *self);

G_END_DECLS
#endif
