/* Production model and message objects; fixture service supplies decrypted events/pages. */
#include "../model/gn-group-message-model.h"
#include <nostr-kinds.h>

struct _GnMarmotService { GObject parent_instance; gboolean history; guint calls; };
G_DEFINE_TYPE(GnMarmotService, gn_marmot_service, G_TYPE_OBJECT)
static void gn_marmot_service_init(GnMarmotService *self) { (void)self; }
static void gn_marmot_service_class_init(GnMarmotServiceClass *klass) {
  g_signal_new("message-received", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}
MarmotGobjectClient *gn_marmot_service_get_client(GnMarmotService *self) {
  return self->history ? (MarmotGobjectClient *)self : NULL;
}
GPtrArray *marmot_gobject_client_get_messages(MarmotGobjectClient *client,
    const gchar *group, guint limit, guint offset, GError **error) {
  GnMarmotService *self = (GnMarmotService *)client;
  (void)error;
  g_assert_cmpstr(group, ==, "group");
  g_assert_cmpuint(limit, ==, 50);
  g_assert_cmpuint(offset, ==, self->calls++ * 50);
  GPtrArray *page = g_ptr_array_new_with_free_func(g_object_unref);
  guint count = offset < 100 ? 50 : 1;
  for (guint i = 0; i < count; i++) {
    guint kind = offset == 50 || i > 0 ? 7 : NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE;
    g_autofree gchar *id = g_strdup_printf("%u", offset + i);
    g_ptr_array_add(page, marmot_gobject_message_new_from_data(id, "author", "text", kind, i, group));
  }
  return page;
}
static void changed(GListModel *model, guint pos, guint removed, guint added, guint *count) {
  (void)model; (void)pos; g_assert_cmpuint(removed, ==, 0); g_assert_cmpuint(added, >, 0); (*count)++;
}
static void live(void) {
  g_autoptr(GnMarmotService) service = g_object_new(GN_TYPE_MARMOT_SERVICE, NULL);
  g_autoptr(GnGroupMessageModel) model = gn_group_message_model_new(service, "group");
  guint changes = 0;
  g_signal_connect(model, "items-changed", G_CALLBACK(changed), &changes);
  const guint kinds[] = {9, 7, 5, 1200, 1201, 1202};
  for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
    g_autofree gchar *json = g_strdup_printf("{\"pubkey\":\"author\",\"kind\":%u,\"content\":\"payload\"}", kinds[i]);
    g_signal_emit_by_name(service, "message-received", "group", json);
  }
  const char *invalid[] = {"{}", "{\"pubkey\":\"author\"}", "{\"pubkey\":\"author\",\"kind\":\"9\"}", "{\"pubkey\":\"author\",\"kind\":9.5}", "{\"kind\":{}}", "{\"kind\":null}", "{\"kind\":[]}"};
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++)
    g_signal_emit_by_name(service, "message-received", "group", invalid[i]);
  g_signal_emit_by_name(service, "message-received", "other-group", "{\"pubkey\":\"author\",\"kind\":9}");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 1);
  g_assert_cmpuint(changes, ==, 1);
  g_autoptr(MarmotGobjectMessage) message = g_list_model_get_item(G_LIST_MODEL(model), 0);
  g_assert_cmpuint(marmot_gobject_message_get_kind(message), ==, 9);
}
static void history(void) {
  g_autoptr(GnMarmotService) service = g_object_new(GN_TYPE_MARMOT_SERVICE, NULL);
  service->history = TRUE;
  g_autoptr(GnGroupMessageModel) model = gn_group_message_model_new(service, "group");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 1);
  guint changes = 0;
  g_signal_connect(model, "items-changed", G_CALLBACK(changed), &changes);
  gn_group_message_model_load_more(model); /* Entire non-chat page must not end pagination. */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 1);
  g_assert_cmpuint(changes, ==, 0);
  gn_group_message_model_load_more(model);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 2);
  g_assert_cmpuint(changes, ==, 1);
  gn_group_message_model_load_more(model);
  g_assert_cmpuint(service->calls, ==, 3);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/mls/message-model/live-chat-only", live);
  g_test_add_func("/mls/message-model/history-chat-only", history);
  return g_test_run();
}
