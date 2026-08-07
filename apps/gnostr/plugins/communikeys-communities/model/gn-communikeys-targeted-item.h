#ifndef GN_COMMUNIKEYS_TARGETED_ITEM_H
#define GN_COMMUNIKEYS_TARGETED_ITEM_H
#include <glib-object.h>
G_BEGIN_DECLS
#define GN_TYPE_COMMUNIKEYS_TARGETED_ITEM (gn_communikeys_targeted_item_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysTargetedItem, gn_communikeys_targeted_item,
                     GN, COMMUNIKEYS_TARGETED_ITEM, GObject)
GnCommunikeysTargetedItem *gn_communikeys_targeted_item_new(
    const char *id, const char *identifier, gint64 created_at,
    const char *author, const char *reference, int original_kind,
    const char *original_content, const char *section_name, guint target_count);
const char *gn_communikeys_targeted_item_get_id(GnCommunikeysTargetedItem *self);
const char *gn_communikeys_targeted_item_get_identifier(GnCommunikeysTargetedItem *self);
gint64 gn_communikeys_targeted_item_get_created_at(GnCommunikeysTargetedItem *self);
const char *gn_communikeys_targeted_item_get_author(GnCommunikeysTargetedItem *self);
const char *gn_communikeys_targeted_item_get_reference(GnCommunikeysTargetedItem *self);
int gn_communikeys_targeted_item_get_original_kind(GnCommunikeysTargetedItem *self);
const char *gn_communikeys_targeted_item_get_original_content(GnCommunikeysTargetedItem *self);
const char *gn_communikeys_targeted_item_get_section_name(GnCommunikeysTargetedItem *self);
guint gn_communikeys_targeted_item_get_target_count(GnCommunikeysTargetedItem *self);
G_END_DECLS
#endif
