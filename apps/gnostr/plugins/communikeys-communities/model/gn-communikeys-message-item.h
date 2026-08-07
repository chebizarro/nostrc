#ifndef GN_COMMUNIKEYS_MESSAGE_ITEM_H
#define GN_COMMUNIKEYS_MESSAGE_ITEM_H
#include <glib-object.h>
G_BEGIN_DECLS
#define GN_TYPE_COMMUNIKEYS_MESSAGE_ITEM (gn_communikeys_message_item_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysMessageItem, gn_communikeys_message_item,
                     GN, COMMUNIKEYS_MESSAGE_ITEM, GObject)
GnCommunikeysMessageItem *gn_communikeys_message_item_new(
    const char *id, const char *event_json, gint64 created_at, int kind,
    const char *pubkey, const char *content, const char *section_name);
const char *gn_communikeys_message_item_get_id(GnCommunikeysMessageItem *self);
const char *gn_communikeys_message_item_get_event_json(GnCommunikeysMessageItem *self);
gint64 gn_communikeys_message_item_get_created_at(GnCommunikeysMessageItem *self);
int gn_communikeys_message_item_get_kind(GnCommunikeysMessageItem *self);
const char *gn_communikeys_message_item_get_pubkey(GnCommunikeysMessageItem *self);
const char *gn_communikeys_message_item_get_content(GnCommunikeysMessageItem *self);
const char *gn_communikeys_message_item_get_section_name(GnCommunikeysMessageItem *self);
G_END_DECLS
#endif
