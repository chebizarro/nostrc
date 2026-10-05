#include "gh-about-dialog.h"
#include "gh-new-message-dialog.h"
#include "gh-issue-dialog.h"
#include "gh-conversation-list.h"
#include "gh-test-signer.h"
#include "gh-recipient.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <nip34.h>

#define OWNER "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400"
#define OWNER_URI "nostr:npub1ehhfg09mr8z34wz85ek46a6rww4f7c7jsujxhdvmpqnl5hnrwsqq2szjqv"

static GhTestBus application_bus;

void gh_test_about_flows_setup(void) { gh_test_bus_up_beside_gtk(&application_bus); }
void gh_test_about_flows_teardown(void) { gh_test_bus_down(&application_bus); }

static void drain(void) { while (g_main_context_iteration(NULL, FALSE)); }

static void
no_marmot(const gchar *pubkey, GCancellable *cancel, GAsyncReadyCallback callback,
           gpointer data, gpointer config_data)
{
  (void)pubkey; (void)cancel; (void)callback; (void)data; (void)config_data;
  g_error("A nostr: link must not create a Marmot DM or look up KeyPackages");
}

static void
activate_uri_app(GApplication *app, gpointer data)
{
  (void)app;
  gtk_window_present(GTK_WINDOW(data));
}

static gboolean
quit_app(gpointer data)
{
  g_application_quit(G_APPLICATION(data));
  return G_SOURCE_REMOVE;
}

static void
activate_empty(GApplication *app, gpointer data)
{
  (void)app;
  (void)data;
}

static void
message_uri(const char *uri, const char *expected, gboolean via_application)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  g_autofree gchar *account = gh_test_pub(1);
  gh_conversation_store_set_account(store, account, NULL, NULL, NULL);
  g_autoptr(GtkApplication) app = gtk_application_new("org.nostr.Groundhog.AboutTest",
    G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_OPEN);
  g_autoptr(GError) register_error = NULL;
  if (!g_application_register(G_APPLICATION(app), NULL, &register_error))
    g_error("g_application_register: %s", register_error ? register_error->message : "(no error)");
  gh_window_setup_application(app);
  GhWindow *window = gh_window_new(app);
  g_signal_connect(app, "activate", G_CALLBACK(activate_uri_app), window);
  gh_conversation_list_attach(window, store, NULL);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  GhNewMessageConfig config = { .conversations = store, .settings = settings,
                               .create_marmot_dm = no_marmot };
  gh_new_message_attach(window, &config);
  gtk_window_present(GTK_WINDOW(window));
  if (via_application) {
    g_autoptr(GFile) file = g_file_new_for_uri(uri);
    GFile *files[] = {file};
    g_application_open(G_APPLICATION(app), files, 1, "");
  } else {
    AdwDialog *about = gh_about_dialog_new();
    adw_dialog_present(about, GTK_WIDGET(window));
    gboolean handled = FALSE;
    g_signal_emit_by_name(about, "activate-link", uri, &handled);
    g_assert_true(handled);
  }
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(GH_IS_NEW_MESSAGE_DIALOG(dialog));
  GListModel *recipients = gh_new_message_dialog_get_recipients(GH_NEW_MESSAGE_DIALOG(dialog));
  g_assert_cmpuint(g_list_model_get_n_items(recipients), ==, 1);
  g_autoptr(GhNewMessageItem) item = g_list_model_get_item(recipients, 0);
  g_assert_cmpstr(gh_new_message_item_get_pubkey(item), ==, expected);
  /* Opening a link adds a chip, not a contact, accepted request or room. */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 0);
  gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.next", NULL);
  gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.start", NULL);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 1);
  g_autoptr(GhConversation) room = g_list_model_get_item(G_LIST_MODEL(store), 0);
  g_assert_cmpint(gh_conversation_get_backend(room), ==, GH_CONVERSATION_BACKEND_NIP17);
  gtk_window_destroy(GTK_WINDOW(window));
  drain();
  g_assert_null(gtk_application_get_windows(app));
  /* A manually registered GApplication retains its D-Bus export until run()
   * completes shutdown. Do not leak that export into the next test case. */
  g_signal_handlers_disconnect_by_func(app, G_CALLBACK(activate_uri_app), window);
  g_signal_connect(app, "activate", G_CALLBACK(activate_empty), NULL);
  g_idle_add(quit_app, app);
  char *argv[] = { (char *) "groundhog-about-test", NULL };
  g_assert_cmpint(g_application_run(G_APPLICATION(app), 1, argv), ==, 0);
  drain();
  g_assert_cmpuint(G_OBJECT(app)->ref_count, ==, 1);
}

static void test_npub(void) { message_uri(OWNER_URI, OWNER, FALSE); }

static void
test_nprofile(void)
{
  NostrProfilePointer *pointer = nostr_profile_pointer_new();
  pointer->public_key = strdup(OWNER);
  pointer->relays = calloc(1, sizeof(char *));
  pointer->relays[0] = strdup("ws://127.0.0.1:1");
  pointer->relays_count = 1;
  char *bech = NULL;
  g_assert_cmpint(nostr_nip19_encode_nprofile(pointer, &bech), ==, 0);
  nostr_profile_pointer_free(pointer);
  g_autofree gchar *uri = g_strconcat("nostr:", bech, NULL);
  free(bech);
  message_uri(uri, OWNER, TRUE);
}

static GPtrArray *
identities(gpointer data, GError **error)
{
  (void)data; (void)error;
  GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = gh_test_npub(1);
  info->label = g_strdup("Test signer");
  g_ptr_array_add(list, info);
  return list;
}

static gboolean active(gpointer data) {
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE;
}

static GtkWidget *
child(GhIssueDialog *dialog, const char *name)
{
  GObject *object = gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_ISSUE_DIALOG, name);
  g_assert_nonnull(object);
  return GTK_WIDGET(object);
}

typedef struct {
  guint connections;
  guint events;
  NostrEvent *event;
  SoupWebsocketConnection *connection;
} Relay;

static void
relay_message(SoupWebsocketConnection *connection, SoupWebsocketDataType type,
               GBytes *bytes, gpointer data)
{
  Relay *relay = data;
  g_assert_cmpint(type, ==, SOUP_WEBSOCKET_DATA_TEXT);
  gsize length = 0;
  const char *text = g_bytes_get_data(bytes, &length);
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, text, (gssize)length, NULL));
  JsonArray *frame = json_node_get_array(json_parser_get_root(parser));
  g_assert_cmpstr(json_array_get_string_element(frame, 0), ==, "EVENT");
  g_autofree char *json = json_to_string(json_array_get_element(frame, 1), FALSE);
  relay->events++;
  g_assert_null(relay->event);
  relay->event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(relay->event, json, NULL), ==, 1);
  g_assert_true(nostr_event_check_signature(relay->event));
  g_autofree char *ok = g_strdup_printf("[\"OK\",\"%s\",true,\"\"]", nostr_event_get_id(relay->event));
  soup_websocket_connection_send_text(connection, ok);
}

static void
relay_connected(SoupServer *server, SoupServerMessage *message, const char *path,
                  SoupWebsocketConnection *connection, gpointer data)
{
  (void)server; (void)message; (void)path;
  Relay *relay = data;
  relay->connections++;
  relay->connection = g_object_ref(connection);
  g_signal_connect(connection, "message", G_CALLBACK(relay_message), relay);
}

static gboolean
published(gpointer data)
{
  return g_strcmp0(gtk_label_get_text(GTK_LABEL(data)), "Issue published. It is public on Nostr.") == 0;
}

static void
tag_is(NostrEvent *event, const char *name, const char *value)
{
  const NostrTags *tags = nostr_event_get_tags(event);
  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), name) == 0 &&
        g_strcmp0(nostr_tag_get(tag, 1), value) == 0)
      return;
  }
  g_error("Missing %s=%s tag", name, value);
}

static gboolean held(gpointer data) { return ((GhTestSigner *)data)->held->len == 1; }

static void
test_issue_consent(gconstpointer data)
{
  int mode = GPOINTER_TO_INT(data);
  GhTestBus bus = {0};
  GhTestSigner signer = {0};
  gh_test_bus_up_beside_gtk(&bus);
  gh_test_signer_up(&bus, &signer);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_autofree char *npub = gh_test_npub(1);
  g_autofree char *pk = gh_test_pub(1);
  g_settings_set_string(settings, "current-npub", npub);
  g_settings_set_string(settings, "signer-method", "auto");
  g_autoptr(GhAccountController) accounts = gh_account_controller_new_full(settings, bus.client, identities, NULL);
  gh_test_spin_until(active, accounts);

  Relay relay = {0};
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  char *protocols[] = { "nostr", NULL };
  soup_server_add_websocket_handler(server, "/", NULL, protocols, relay_connected, &relay, NULL);
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string("127.0.0.1");
  g_autoptr(GSocketAddress) socket_address = g_inet_socket_address_new(address, 0);
  g_assert_true(soup_server_listen(server, socket_address, 0, NULL));
  GSList *uris = soup_server_get_uris(server);
  g_autofree char *url = g_strdup_printf("ws://127.0.0.1:%d", g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  const char *relays[] = {url, NULL};
  g_settings_set_strv(settings, "discovery-relays", relays);
  GtkWindow *window = GTK_WINDOW(adw_window_new());
  gtk_window_present(window);
  GhIssueDialog *dialog = gh_issue_dialog_new(accounts, settings);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  gtk_editable_set_text(GTK_EDITABLE(child(dialog, "title_row")), "<Test title>");
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(child(dialog, "body_view"))),
                           "The exact body.\nNo diagnostics.", -1);
  g_signal_emit_by_name(child(dialog, "review_button"), "clicked");
  AdwDialog *alert = adw_window_get_visible_dialog(ADW_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(alert));
  GtkWidget *scroll = adw_alert_dialog_get_extra_child(ADW_ALERT_DIALOG(alert));
  GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
  GtkWidget *label = GTK_IS_VIEWPORT(viewport) ? gtk_viewport_get_child(GTK_VIEWPORT(viewport)) : viewport;
  const char *preview = gtk_label_get_text(GTK_LABEL(label));
  g_assert_nonnull(strstr(preview, "<Test title>"));
  g_assert_nonnull(strstr(preview, "The exact body.\nNo diagnostics."));
  g_assert_nonnull(strstr(preview, url));
  g_assert_nonnull(strstr(preview, pk));
  g_assert_nonnull(strstr(preview, "30617:" OWNER ":nostrc"));
  drain();
  g_assert_cmpuint(signer.calls, ==, 0);
  g_assert_cmpuint(relay.connections, ==, 0);
  g_signal_emit_by_name(alert, "response", "cancel");
  adw_dialog_force_close(alert);
  drain();
  g_assert_cmpuint(signer.calls, ==, 0);
  g_assert_cmpuint(relay.connections, ==, 0);
  g_signal_emit_by_name(child(dialog, "review_button"), "clicked");
  alert = adw_window_get_visible_dialog(ADW_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(alert));
  if (mode == 1) {
    g_settings_set_string(settings, "current-npub", "");
    drain();
    g_assert_cmpint(gh_account_controller_get_state(accounts), !=, GH_ACCOUNT_STATE_ACTIVE);
  }
  if (mode == 2)
    signer.hold = TRUE;
  g_signal_emit_by_name(alert, "response", "publish");
  adw_dialog_force_close(alert);
  if (mode == 1) {
    drain();
    g_assert_cmpuint(signer.calls, ==, 0);
    g_assert_cmpuint(relay.connections, ==, 0);
    gtk_window_destroy(window);
    goto cleanup;
  }
  if (mode == 2) {
    gh_test_spin_until(held, &signer);
    gtk_window_destroy(window);
    GhTestSenders pending = { &bus, &signer };
    gh_test_spin_until(gh_test_signer_senders_closed, &pending);
    gh_test_signer_release_all(&signer);
    drain();
    g_assert_cmpuint(relay.connections, ==, 0);
    goto cleanup;
  }
  gh_test_spin_until(published, child(dialog, "status"));
  g_assert_cmpuint(signer.calls, ==, 1);
  g_assert_cmpuint(relay.events, ==, 1);
  g_assert_cmpint(nostr_event_get_kind(relay.event), ==, NIP34_KIND_ISSUE);
  g_assert_cmpstr(nostr_event_get_pubkey(relay.event), ==, pk);
  g_assert_cmpstr(nostr_event_get_content(relay.event), ==, "The exact body.\nNo diagnostics.");
  tag_is(relay.event, "a", "30617:" OWNER ":nostrc");
  tag_is(relay.event, "p", OWNER);
  tag_is(relay.event, "subject", "<Test title>");
  tag_is(relay.event, "t", "bug");
  tag_is(relay.event, "t", "groundhog");
  tag_is(relay.event, "L", NIP34_ISSUE_LABEL_NAMESPACE);
  tag_is(relay.event, "l", "bug");
  tag_is(relay.event, "l", "groundhog");
  tag_is(relay.event, "alt", "git repository issue");
  g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(relay.event)), ==, 9);
  gtk_window_destroy(window);
cleanup:
  drain();
  if (relay.connection) {
    g_signal_handlers_disconnect_by_data(relay.connection, &relay);
    if (soup_websocket_connection_get_state(relay.connection) == SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_close(relay.connection, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
    g_clear_object(&relay.connection);
  }
  soup_server_disconnect(server);
  nostr_event_free(relay.event);
  gh_test_release(g_steal_pointer(&accounts));
  GhTestSenders check = { &bus, &signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  gh_test_signer_down(&bus, &signer);
  gh_test_bus_down(&bus);
}

void
gh_test_about_flows_register(void)
{
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  g_test_add_func("/groundhog/about/npub-dm", test_npub);
  g_test_add_func("/groundhog/about/nprofile-dm", test_nprofile);
  g_test_add_data_func("/groundhog/about/issue-consent", GINT_TO_POINTER(0), test_issue_consent);
  g_test_add_data_func("/groundhog/about/issue-account-changed", GINT_TO_POINTER(1), test_issue_consent);
  g_test_add_data_func("/groundhog/about/issue-close-signing", GINT_TO_POINTER(2), test_issue_consent);
}
