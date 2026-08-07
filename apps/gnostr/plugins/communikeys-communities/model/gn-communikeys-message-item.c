#include "gn-communikeys-message-item.h"
struct _GnCommunikeysMessageItem {
  GObject parent_instance;
  gchar *id;
  gchar *event_json;
  gint64 created_at;
  int kind;
  gchar *pubkey;
  gchar *content;
  gchar *section_name;
};
G_DEFINE_TYPE(GnCommunikeysMessageItem, gn_communikeys_message_item, G_TYPE_OBJECT)
static void gn_communikeys_message_item_finalize(GObject *object) {
  GnCommunikeysMessageItem *self = GN_COMMUNIKEYS_MESSAGE_ITEM(object);
  g_free(self->id);
  g_free(self->event_json);
  g_free(self->pubkey);
  g_free(self->content);
  g_free(self->section_name);
  G_OBJECT_CLASS(gn_communikeys_message_item_parent_class)->finalize(object);
}
static void gn_communikeys_message_item_class_init(GnCommunikeysMessageItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gn_communikeys_message_item_finalize;
}
static void gn_communikeys_message_item_init(GnCommunikeysMessageItem *self) {
  (void)self;
}
GnCommunikeysMessageItem *gn_communikeys_message_item_new(
    const char *id, const char *event_json, gint64 created_at, int kind,
    const char *pubkey, const char *content, const char *section_name) {
  GnCommunikeysMessageItem *self =
    g_object_new(GN_TYPE_COMMUNIKEYS_MESSAGE_ITEM, NULL);
  self->id = g_strdup(id);
  self->event_json = g_strdup(event_json);
  self->created_at = created_at;
  self->kind = kind;
  self->pubkey = g_strdup(pubkey);
  self->content = g_strdup(content);
  self->section_name = g_strdup(section_name);
  return self;
}
const char *gn_communikeys_message_item_get_id(GnCommunikeysMessageItem *self) { return self->id; }
const char *gn_communikeys_message_item_get_event_json(GnCommunikeysMessageItem *self) { return self->event_json; }
gint64 gn_communikeys_message_item_get_created_at(GnCommunikeysMessageItem *self) { return self->created_at; }
int gn_communikeys_message_item_get_kind(GnCommunikeysMessageItem *self) { return self->kind; }
const char *gn_communikeys_message_item_get_pubkey(GnCommunikeysMessageItem *self) { return self->pubkey; }
const char *gn_communikeys_message_item_get_content(GnCommunikeysMessageItem *self) { return self->content; }
const char *gn_communikeys_message_item_get_section_name(GnCommunikeysMessageItem *self) { return self->section_name; }
