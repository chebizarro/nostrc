#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include <string.h>
#include <libsoup/soup.h>
#include <jansson.h>
#include "nostr-keys.h"
#include "json.h"
#include <nostr-gobject-1.0/gnostr-signer-bridge.h>

static void pump(void) { guint n = 0; while (g_main_context_pending(NULL) && n++ < 10000) g_main_context_iteration(NULL, FALSE); g_assert_cmpuint(n, <, 10000); }
static void changed(GNostrListService *service, GNostrListDocument *document, gpointer data) { (void)service; (void)document; (*(guint *)data)++; }
static void test_facade(void) {
  GNostrMuteList *s = gnostr_mute_list_get_default(); g_assert_true(s == gnostr_mute_list_get_default());
  GNostrListService *service = gnostr_mute_list_get_service(s); g_assert_nonnull(service); guint changes = 0;
  gulong handler = g_signal_connect(service, "document-changed", G_CALLBACK(changed), &changes);
  g_assert_true(gnostr_mute_list_load_from_json(s, "{\"kind\":10000,\"tags\":[[\"x-extension\",\"\",\"tail\"]]}"));
  gnostr_mute_list_add_word(s, "Spoiler", TRUE); gnostr_mute_list_add_hashtag(s, "#Nostr", FALSE);
  g_assert_true(gnostr_mute_list_contains_muted_word(s, "SPOILER ahead")); g_assert_true(gnostr_mute_list_is_hashtag_muted(s, "NOSTR"));
  pump(); g_assert_cmpuint(changes, >, 0);
  GNostrListDocument *doc = gnostr_list_service_lookup(service, 10000, "");
  const NostrListDocument *codec = gnostr_list_document_get_codec(doc);
  g_assert_cmpuint(nostr_nip51_document_tag_count(codec, FALSE), ==, 2);
  g_assert_cmpstr(nostr_nip51_tag_field(nostr_nip51_document_tag(codec, FALSE, 0), 2), ==, "tail");
  g_assert_cmpuint(nostr_nip51_document_tag_count(codec, TRUE), ==, 1); g_object_unref(doc);
  size_t n = 0; const char **words = gnostr_mute_list_get_words(s, &n); g_assert_cmpuint(n, ==, 1); const char *borrowed = words[0];
  gnostr_mute_list_remove_word(s, "spoiler"); g_assert_false(gnostr_mute_list_contains_muted_word(s, "spoiler")); pump(); g_assert_cmpstr(borrowed, ==, "spoiler"); g_free(words);
  g_signal_handler_disconnect(service, handler); g_object_unref(service); gnostr_mute_list_shutdown(); pump();
}
static gpointer worker(gpointer data) {
  GNostrMuteList *s = data;
  gnostr_mute_list_add_word(s, "thread", FALSE);
  g_assert_true(gnostr_mute_list_contains_muted_word(s, "THREAD"));
  gnostr_mute_list_remove_word(s, "thread"); gnostr_mute_list_add_hashtag(s, "thread-tag", TRUE); return NULL;
}
static void test_thread_and_shutdown(void) {
  GNostrMuteList *s = gnostr_mute_list_get_default(); GThread *thread = g_thread_new("facade-edit", worker, s); g_thread_join(thread); pump();
  GNostrListService *service = gnostr_mute_list_get_service(s); GNostrListDocument *doc = gnostr_list_service_lookup(service, 10000, "");
  g_assert_cmpuint(nostr_nip51_document_tag_count(gnostr_list_document_get_codec(doc), TRUE), ==, 1); g_object_unref(doc);
  gnostr_mute_list_add_word(s, "cancelled-before-dispatch", TRUE); gnostr_mute_list_shutdown(); pump();
  g_assert_cmpint(gnostr_list_service_get_state(service, 10000, ""), ==, GNOSTR_LIST_INACTIVE); g_object_unref(service);
}
static void saved(GNostrMuteList *s, gboolean success, const char *error, gpointer data) { (void)s; g_assert_false(success); g_assert_nonnull(error); *(gboolean *)data = TRUE; }
static void test_no_stub_success(void) {
  GNostrMuteList *s = gnostr_mute_list_get_default(); gnostr_mute_list_add_word(s, "private", TRUE); gboolean done = FALSE;
  gnostr_mute_list_save_async(s, saved, &done); pump(); g_assert_true(done); g_assert_true(gnostr_mute_list_is_dirty(s)); gnostr_mute_list_shutdown(); pump();
}
typedef struct { GPtrArray *connections; SoupServer *server; char *url, *event, *sk, *pk; GMainLoop *loop; gboolean completed, success, no_event, edit_on_req; GCancellable *cancel_on_req; guint publishes; } RelayFixture;
static RelayFixture *signing_fixture;
static void relay_message(SoupWebsocketConnection *connection, int type, GBytes *bytes, gpointer data) {
  RelayFixture *f = data; g_assert_cmpint(type, ==, SOUP_WEBSOCKET_DATA_TEXT);
  gsize length; const char *text = g_bytes_get_data(bytes, &length); json_t *message = json_loadb(text, length, 0, NULL);
  const char *command = json_string_value(json_array_get(message, 0));
  json_t *reply = NULL;
  if (!g_strcmp0(command, "REQ")) {
    const char *subscription = json_string_value(json_array_get(message, 1));
    json_t *filter = json_array_get(message, 2);
    g_assert_cmpstr(json_string_value(json_array_get(json_object_get(filter, "authors"), 0)), ==, f->pk);
    g_assert_cmpint(json_integer_value(json_array_get(json_object_get(filter, "kinds"), 0)), ==, 10000);
    if (f->cancel_on_req) { g_cancellable_cancel(f->cancel_on_req); json_decref(message); return; }
    if (f->edit_on_req) gnostr_mute_list_add_word(gnostr_mute_list_get_default(), "during-fetch", FALSE);
    if (!f->no_event) { reply = json_pack("[s,s,o]", "EVENT", subscription, json_loads(f->event, 0, NULL)); char *wire = json_dumps(reply, JSON_COMPACT); soup_websocket_connection_send_text(connection, wire); free(wire); json_decref(reply); }
    reply = json_pack("[s,s]", "EOSE", subscription);
  } else if (!g_strcmp0(command, "EVENT")) {
    char *json = json_dumps(json_array_get(message, 1), JSON_COMPACT); NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK); g_assert_true(nostr_event_check_signature(event));
    g_assert_cmpstr(nostr_event_get_pubkey(event), ==, f->pk); f->publishes++;
    char *id = nostr_event_get_id(event); reply = json_pack("[s,s,b,s]", "OK", id, 1, "accepted"); free(id); free(json); nostr_event_free(event);
  }
  if (reply) { char *wire = json_dumps(reply, JSON_COMPACT); soup_websocket_connection_send_text(connection, wire); free(wire); json_decref(reply); }
  json_decref(message);
}
static void websocket(SoupServer *server, SoupServerMessage *message, const char *path, SoupWebsocketConnection *connection, gpointer data) {
  (void)server; (void)message; (void)path; g_ptr_array_add(((RelayFixture *)data)->connections, g_object_ref(connection)); g_signal_connect(connection, "message", G_CALLBACK(relay_message), data);
}
static void fixture_init(RelayFixture *f) {
  memset(f, 0, sizeof(*f)); f->connections = g_ptr_array_new_with_free_func(g_object_unref); f->sk = nostr_key_generate_private(); f->pk = nostr_key_get_public(f->sk);
  NostrEvent *ev = nostr_event_new(); char *json = g_strdup_printf("{\"pubkey\":\"%s\",\"kind\":10000,\"created_at\":10,\"tags\":[[\"word\",\"remote\"]],\"content\":\"\"}", f->pk);
  g_assert_cmpint(nostr_event_deserialize_compact(ev, json, NULL), ==, 1); g_free(json); g_assert_cmpint(nostr_event_sign(ev, f->sk), ==, 0); f->event = nostr_event_serialize(ev); nostr_event_free(ev);
  f->server = soup_server_new(NULL, NULL); soup_server_add_websocket_handler(f->server, "/", NULL, NULL, websocket, f, NULL);
  GError *error = NULL; g_assert_true(soup_server_listen_local(f->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)); g_assert_no_error(error);
  GSList *uris = soup_server_get_uris(f->server); f->url = g_strdup_printf("ws://127.0.0.1:%d/", g_uri_get_port(uris->data)); g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  f->loop = g_main_loop_new(NULL, FALSE);
}
static gboolean deadline(gpointer data) { RelayFixture *f = data; g_main_loop_quit(f->loop); return G_SOURCE_REMOVE; }
static void fixture_wait(RelayFixture *f) {
  guint timeout = g_timeout_add_seconds(10, deadline, f); if (!f->completed) g_main_loop_run(f->loop);
  g_assert_true(f->completed); g_source_remove(timeout); f->completed = FALSE;
}
static void fixture_clear(RelayFixture *f) { gnostr_mute_list_shutdown(); pump(); soup_server_disconnect(f->server); for (guint i = 0; i < f->connections->len; i++) { SoupWebsocketConnection *c = f->connections->pdata[i]; g_signal_handlers_disconnect_by_data(c, f); if (soup_websocket_connection_get_state(c) == SOUP_WEBSOCKET_STATE_OPEN) soup_websocket_connection_close(c, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL); } g_ptr_array_unref(f->connections); g_object_unref(f->server); g_main_loop_unref(f->loop); g_free(f->url); g_free(f->event); g_free(f->sk); g_free(f->pk); }
static void fetch_done(GNostrMuteList *s, gboolean success, gpointer data) { (void)s; RelayFixture *f = data; f->completed = TRUE; f->success = success; g_main_loop_quit(f->loop); }
static gboolean signer_available(void) { return TRUE; }
static void signer_start(const char *json, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  GTask *task = g_task_new(NULL, cancel, callback, data); NostrEvent *event = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_sign(event, signing_fixture->sk), ==, 0); g_task_return_pointer(task, nostr_event_serialize(event), g_free); nostr_event_free(event); g_object_unref(task);
}
static gboolean signer_finish(GAsyncResult *result, char **json, GError **error) { *json = g_task_propagate_pointer(G_TASK(result), error); return *json != NULL; }
static void save_done(GNostrMuteList *s, gboolean success, const char *error, gpointer data) { (void)error; fetch_done(s, success, data); }
static void test_merge_transport(void) {
  for (guint strategy = GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS; strategy <= GNOSTR_MUTE_LIST_MERGE_LATEST; strategy++) {
    RelayFixture f; fixture_init(&f); GNostrMuteList *s = gnostr_mute_list_get_default();
    g_assert_true(gnostr_mute_list_load_from_json(s, "{\"kind\":10000,\"created_at\":20,\"tags\":[[\"word\",\"local\"]]}")); pump();
    const char *relays[] = {f.url, NULL};
    gnostr_mute_list_fetch_with_strategy_async(s, f.pk, relays, strategy, fetch_done, &f); fixture_wait(&f); g_assert_true(f.success);
    g_assert_cmpint(gnostr_mute_list_contains_muted_word(s, "local"), ==, strategy != GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS);
    g_assert_cmpint(gnostr_mute_list_contains_muted_word(s, "remote"), ==, strategy == GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS || strategy == GNOSTR_MUTE_LIST_MERGE_UNION);
    if (strategy == GNOSTR_MUTE_LIST_MERGE_UNION) {
      signing_fixture = &f; GnostrSignerBridge bridge = {.is_available = signer_available, .sign_event_async = signer_start, .sign_event_finish = signer_finish}; gnostr_signer_bridge_install(&bridge);
      gnostr_mute_list_add_word(s, "published", FALSE); gnostr_mute_list_save_async(s, save_done, &f); fixture_wait(&f); g_assert_true(f.success);
      g_assert_cmpuint(f.publishes, ==, 1); g_assert_false(gnostr_mute_list_is_dirty(s)); gnostr_signer_bridge_install(NULL); signing_fixture = NULL;
    }
    fixture_clear(&f);
  }
}
static void test_absence_and_edits_during_fetch(void) {
  for (guint mode = 0; mode < 3; mode++) {
    RelayFixture f; fixture_init(&f); GNostrMuteList *s = gnostr_mute_list_get_default();
    g_assert_true(gnostr_mute_list_load_from_json(s, "{\"kind\":10000,\"created_at\":20,\"tags\":[[\"word\",\"local\"]]}")); pump();
    f.no_event = mode == 0; f.edit_on_req = mode != 0;
    const char *relays[] = {f.url, NULL}; GNostrMuteListMergeStrategy strategy = mode == 2 ? GNOSTR_MUTE_LIST_MERGE_UNION : GNOSTR_MUTE_LIST_MERGE_LOCAL_WINS;
    if (mode == 0) strategy = GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS;
    gnostr_mute_list_fetch_with_strategy_async(s, f.pk, relays, strategy, fetch_done, &f); fixture_wait(&f);
    g_assert_cmpint(f.success, ==, mode != 0); g_assert_true(gnostr_mute_list_contains_muted_word(s, "local"));
    if (mode) g_assert_true(gnostr_mute_list_contains_muted_word(s, "during-fetch"));
    fixture_clear(&f);
  }
}
static void fetch_cancel_done(GObject *source, GAsyncResult *res, gpointer data) {
  (void)source; RelayFixture *f = data; GError *error = NULL;
  g_assert_false(gnostr_mute_list_fetch_gtask_finish(gnostr_mute_list_get_default(), res, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error); f->completed = TRUE; g_main_loop_quit(f->loop);
}
static void test_fetch_cancel_invalid_plan(void) {
  RelayFixture f; fixture_init(&f); GNostrMuteList *s = gnostr_mute_list_get_default(); gnostr_mute_list_add_word(s, "keep", FALSE); pump();
  const char *duplicates[] = {f.url, f.url, NULL}; gnostr_mute_list_fetch_async(s, f.pk, duplicates, fetch_done, &f); g_assert_true(f.completed); g_assert_false(f.success);
  gnostr_mute_list_add_word(s, "after-invalid-plan", FALSE); pump(); g_assert_true(gnostr_mute_list_contains_muted_word(s, "after-invalid-plan")); f.completed = FALSE;
  const char *relays[] = {f.url, NULL}; GCancellable *cancel = g_cancellable_new(); f.cancel_on_req = cancel;
  gnostr_mute_list_fetch_gtask_async(s, f.pk, relays, cancel, fetch_cancel_done, &f); fixture_wait(&f);
  g_assert_true(gnostr_mute_list_contains_muted_word(s, "keep")); g_assert_false(gnostr_mute_list_contains_muted_word(s, "remote"));
  g_object_unref(cancel); fixture_clear(&f);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL); g_test_add_func("/mute-compat/facade", test_facade); g_test_add_func("/mute-compat/thread-shutdown", test_thread_and_shutdown); g_test_add_func("/mute-compat/no-stub-success", test_no_stub_success); g_test_add_func("/mute-compat/merge-transport", test_merge_transport); g_test_add_func("/mute-compat/absence-edits-during-fetch", test_absence_and_edits_during_fetch); g_test_add_func("/mute-compat/cancel-invalid-plan", test_fetch_cancel_invalid_plan); return g_test_run();
}
