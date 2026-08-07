#include "gn-communikeys-targeted-item.h"
struct _GnCommunikeysTargetedItem {
  GObject parent_instance;
  gchar *id;
  gchar *identifier;
  gint64 created_at;
  gchar *author;
  gchar *reference;
  int original_kind;
  gchar *original_content;
  gchar *section_name;
  guint target_count;
};
G_DEFINE_TYPE(GnCommunikeysTargetedItem, gn_communikeys_targeted_item, G_TYPE_OBJECT)
static void gn_communikeys_targeted_item_finalize(GObject *object) {
  GnCommunikeysTargetedItem *self = GN_COMMUNIKEYS_TARGETED_ITEM(object);
  g_free(self->id);
  g_free(self->identifier);
  g_free(self->author);
  g_free(self->reference);
  g_free(self->original_content);
  g_free(self->section_name);
  G_OBJECT_CLASS(gn_communikeys_targeted_item_parent_class)->finalize(object);
}
static void gn_communikeys_targeted_item_class_init(GnCommunikeysTargetedItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gn_communikeys_targeted_item_finalize;
}
static void gn_communikeys_targeted_item_init(GnCommunikeysTargetedItem *self) { (void)self; }
GnCommunikeysTargetedItem *gn_communikeys_targeted_item_new(
    const char *id, const char *identifier, gint64 created_at,
    const char *author, const char *reference, int original_kind,
    const char *original_content, const char *section_name, guint target_count) {
  GnCommunikeysTargetedItem *self =
    g_object_new(GN_TYPE_COMMUNIKEYS_TARGETED_ITEM, NULL);
  self->id = g_strdup(id);
  self->identifier = g_strdup(identifier);
  self->created_at = created_at;
  self->author = g_strdup(author);
  self->reference = g_strdup(reference);
  self->original_kind = original_kind;
  self->original_content = g_strdup(original_content);
  self->section_name = g_strdup(section_name);
  self->target_count = target_count;
  return self;
}
const char *gn_communikeys_targeted_item_get_id(GnCommunikeysTargetedItem *self) { return self->id; }
const char *gn_communikeys_targeted_item_get_identifier(GnCommunikeysTargetedItem *self) { return self->identifier; }
gint64 gn_communikeys_targeted_item_get_created_at(GnCommunikeysTargetedItem *self) { return self->created_at; }
const char *gn_communikeys_targeted_item_get_author(GnCommunikeysTargetedItem *self) { return self->author; }
const char *gn_communikeys_targeted_item_get_reference(GnCommunikeysTargetedItem *self) { return self->reference; }
int gn_communikeys_targeted_item_get_original_kind(GnCommunikeysTargetedItem *self) { return self->original_kind; }
const char *gn_communikeys_targeted_item_get_original_content(GnCommunikeysTargetedItem *self) { return self->original_content; }
const char *gn_communikeys_targeted_item_get_section_name(GnCommunikeysTargetedItem *self) { return self->section_name; }
guint gn_communikeys_targeted_item_get_target_count(GnCommunikeysTargetedItem *self) { return self->target_count; }
