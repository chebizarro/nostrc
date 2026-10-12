#include "gh-about-dialog.h"
#include "gh-new-message-page.h"
#include "gh-issue-adapters.h"
#include <nostr-gtk-1.0/gn-nip34-issue-fields.h>
#include "gh-diagnostics.h"
#include "gh-conversation-list.h"
#include "gh-test-signer.h"
#include "gh-recipient.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <nip34.h>
#include <nostr-keys.h>
#include <glib/gstdio.h>
#include "gh-issue-resolver.h"
#include "../relay/wire-relay.h"
#if GH_ISSUE_UPLOADS
#include "blossom-fixture.h"
#endif

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
  /* The link slid the sidebar to New Message with its person chosen. */
  AdwNavigationPage *dialog = gh_sidebar_page_get_panel(gh_window_get_sidebar(window));
  g_assert_true(GH_IS_NEW_MESSAGE_PAGE(dialog));
  GListModel *recipients = gh_new_message_page_get_recipients(GH_NEW_MESSAGE_PAGE(dialog));
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
find_named(GtkWidget *root, const char *name)
{
  if (g_strcmp0(gtk_widget_get_name(root), name) == 0)
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_named(c, name);
    if (found)
      return found;
  }
  return NULL;
}

static GtkWidget *
child(GnNip34IssueView *dialog, const char *name)
{
  GtkWidget *widget = find_named(GTK_WIDGET(dialog), name);
  g_assert_nonnull(widget);
  return widget;
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

#define COMMIT40 "ABCDEF0123456789ABCDEF0123456789ABCDEF01"
#define COMMIT64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define DIAG "Groundhog local diagnostics (schema 1; daily counts only)\n" \
             "day\tcomponent\tevent\tresult\tcount\n"

static GnNip34IssueFieldsSnapshot *
fields_new(const char *steps, const char *labels, const char *commits, const char *urls)
{
  GnNip34IssueFieldsSnapshot *fields = g_new0(GnNip34IssueFieldsSnapshot, 1);
  fields->steps = g_strdup(steps);
  fields->expected = g_strdup("It opens.");
  fields->actual = g_strdup("It crashes.");
  fields->labels = g_strdup(labels);
  fields->related_commits = g_strdup(commits);
  fields->attachment_urls = g_strdup(urls);
  return fields;
}

static const char *const RICH_BODY =
  "Crash on open.\n\n## Steps to Reproduce\n\n1. Open\n2. Click"
  "\n\n## Expected Result\n\nIt opens.\n\n## Actual Result\n\nIt crashes."
  "\n\n## Attachments\n\n- https://example.org/shot.png\n- http://example.net/log.txt"
  "\n\n## Related commits\n\n- `abcdef0123456789abcdef0123456789abcdef01`\n- `" COMMIT64 "`";

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
  /* Mode 3: structured fields plus the opt-in local diagnostics appendix. */
  g_autofree char *state = NULL;
  GhDiagnostics *diagnostics = NULL;
  g_autofree char *snapshot = NULL;
  if (mode == 3) {
    state = g_dir_make_tmp("groundhog-issue-diagnostics-XXXXXX", NULL);
    g_settings_set_boolean(settings, "diagnostics-enabled", TRUE);
    diagnostics = gh_diagnostics_new(settings, state);
    gh_diagnostics_record(diagnostics, GH_DIAGNOSTIC_COMPONENT_RELAY,
                          GH_DIAGNOSTIC_EVENT_CONNECT_FAILED, GH_DIAGNOSTIC_RESULT_RETRY);
    gh_diagnostics_set_default(diagnostics);
    snapshot = gh_diagnostics_snapshot(diagnostics);
    g_assert_nonnull(strstr(snapshot, "relay\tconnect_failed\tretry\t1\n"));
  }
  const char *body = mode == 3 ? "Crash on open." : "The exact body.\nNo diagnostics.";
  g_autoptr(GnNip34IssueFieldsSnapshot) fields = fields_new(
    "1. Open\n2. Click", "ui, crash", COMMIT40 " " COMMIT64,
    "https://example.org/shot.png http://example.net/log.txt");
  g_autofree char *rich = mode == 3
    ? g_strconcat(RICH_BODY, "\n\n## Local diagnostics\n\n```text\n", snapshot, "```", NULL) : NULL;

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
  /* The factory returns the portable nostr-gtk form with Groundhog services. */
  /* Files: mode 2 has an attachment client with one public server the user
   * chose; the others have none, so the form offers no files. Nothing is
   * contacted either way unless a file is attached and its upload confirmed
   * (tests/net/test_issue_uploader.c covers the upload itself). */
  GhBlossomClient *blossom = NULL;
#if GH_ISSUE_UPLOADS
  g_autoptr(GhNetHttp) http = NULL;
  g_autoptr(GhBlossomClient) client = NULL;
  if (mode == 2) {
    const char *servers[] = { "https://blossom.example.com", NULL };
    g_settings_set_strv(settings, "blossom-servers", servers);
    http = gh_net_http_new(settings);
    client = gh_blossom_client_new(settings, http);
    blossom = client;
  }
#endif
  GnNip34IssueView *dialog = gh_issue_dialog_new(accounts, settings, blossom, NULL, NULL);
  g_assert_true(GN_IS_NIP34_ISSUE_VIEW(dialog));
  gn_nip34_issue_view_present(dialog, GTK_WIDGET(window));
  {
    GtkWidget *attach = child(dialog, "attach_button");
    g_autofree char *path = g_build_filename(g_get_tmp_dir(), "groundhog-issue-attach.txt", NULL);
    g_assert_true(g_file_set_contents(path, "log", -1, NULL));
    g_autoptr(GFile) file = g_file_new_for_path(path);
    if (blossom) {
      g_assert_true(gtk_widget_is_visible(attach));
    } else {
      g_assert_false(gtk_widget_is_visible(attach));
      /* No uploader: a file cannot be queued at all. */
      g_assert_false(gn_nip34_issue_view_add_attachment(dialog, file));
    }
    g_assert_cmpint(g_unlink(path), ==, 0);
  }
  gtk_editable_set_text(GTK_EDITABLE(child(dialog, "title_row")), "<Test title>");
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(child(dialog, "body_view"))),
                           body, -1);
  GtkCheckButton *include = GTK_CHECK_BUTTON(child(dialog, "diagnostics_check"));
  GtkWidget *appendix = child(dialog, "diagnostics_preview");
  /* Off for every new issue, even with collection enabled. */
  g_assert_false(gtk_check_button_get_active(include));
  g_assert_false(gtk_widget_get_visible(appendix));
  /* Offered only when diagnostics collection is on. */
  g_assert_true(gtk_widget_get_visible(gtk_widget_get_parent(GTK_WIDGET(include))) == (mode == 3));
  if (mode == 3) {
    gn_nip34_issue_fields_set_snapshot(GN_NIP34_ISSUE_FIELDS(child(dialog, "issue_fields")), fields);
    gtk_check_button_set_active(include, TRUE);
    g_assert_true(gtk_widget_get_visible(appendix));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(appendix)), ==, snapshot);
  }
  g_signal_emit_by_name(child(dialog, "review_button"), "clicked");
  AdwDialog *alert = adw_window_get_visible_dialog(ADW_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(alert));
  GtkWidget *scroll = adw_alert_dialog_get_extra_child(ADW_ALERT_DIALOG(alert));
  GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
  GtkWidget *label = GTK_IS_VIEWPORT(viewport) ? gtk_viewport_get_child(GTK_VIEWPORT(viewport)) : viewport;
  const char *preview = gtk_label_get_text(GTK_LABEL(label));
  g_assert_nonnull(strstr(preview, "<Test title>"));
  g_assert_nonnull(strstr(preview, mode == 3 ? rich : body));
  if (mode == 3) {
    g_assert_nonnull(strstr(preview, "l: ui, " NIP34_ISSUE_LABEL_NAMESPACE "\n"));
    g_assert_nonnull(strstr(preview, "t: crash\n"));
    g_assert_nonnull(strstr(adw_alert_dialog_get_body(ADW_ALERT_DIALOG(alert)), "Local diagnostics"));
  }
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
  if (mode == 3) {
    /* An edit after Review invalidates it: nothing is signed or sent. */
    g_autoptr(GnNip34IssueFieldsSnapshot) edited = fields_new("1. Open", "ui, crash", NULL, NULL);
    gn_nip34_issue_fields_set_snapshot(GN_NIP34_ISSUE_FIELDS(child(dialog, "issue_fields")), edited);
    g_signal_emit_by_name(alert, "response", "publish");
    adw_dialog_force_close(alert);
    drain();
    g_assert_cmpuint(signer.calls, ==, 0);
    g_assert_cmpuint(relay.connections, ==, 0);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(child(dialog, "status"))), "changed after review"));
    gn_nip34_issue_fields_set_snapshot(GN_NIP34_ISSUE_FIELDS(child(dialog, "issue_fields")), fields);
    g_signal_emit_by_name(child(dialog, "review_button"), "clicked");
    alert = adw_window_get_visible_dialog(ADW_WINDOW(window));
    g_assert_true(ADW_IS_ALERT_DIALOG(alert));
  }
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
  g_assert_cmpstr(nostr_event_get_content(relay.event), ==, mode == 3 ? rich : body);
  tag_is(relay.event, "a", "30617:" OWNER ":nostrc");
  tag_is(relay.event, "p", OWNER);
  tag_is(relay.event, "subject", "<Test title>");
  tag_is(relay.event, "t", "bug");
  tag_is(relay.event, "t", "groundhog");
  tag_is(relay.event, "L", NIP34_ISSUE_LABEL_NAMESPACE);
  tag_is(relay.event, "l", "bug");
  tag_is(relay.event, "l", "groundhog");
  tag_is(relay.event, "alt", "git repository issue");
  if (mode == 3) {
    tag_is(relay.event, "l", "ui");
    tag_is(relay.event, "t", "crash");
  }
  g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(relay.event)), ==, mode == 3 ? 13 : 9);
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
  g_settings_reset(settings, "blossom-servers");
  GhTestSenders check = { &bus, &signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  gh_test_signer_down(&bus, &signer);
  gh_test_bus_down(&bus);
  if (diagnostics) {
    gh_diagnostics_set_default(NULL);
    g_assert_true(gh_diagnostics_clear(diagnostics, NULL));
    gh_diagnostics_free(diagnostics);
    g_settings_reset(settings, "diagnostics-enabled");
    g_autofree char *dir = g_build_filename(state, "groundhog", "diagnostics", NULL);
    g_autofree char *parent = g_build_filename(state, "groundhog", NULL);
    g_rmdir(dir);
    g_rmdir(parent);
    g_assert_cmpint(g_rmdir(state), ==, 0);
  }
}

/* ---- Repository lookup and upload consent options (nostrc-ewx8x, -u42c7) ---- */

#define MAINT1 "1111111111111111111111111111111111111111111111111111111111111111"
#define MAINT2 "2222222222222222222222222222222222222222222222222222222222222222"
#define MAINT_OLD "3333333333333333333333333333333333333333333333333333333333333333"
#define MAINT_FORGED "4444444444444444444444444444444444444444444444444444444444444444"
#define ANNOUNCED "wss://announced.example"

/* A signed kind-30617 announcement of repo by secret's key. */
static gchar *
announcement(const char *secret, const char *repo, gint64 created_at,
             const char *const *maintainers, const char *const *relays)
{
  NostrEvent *event = nip34_create_repo_announcement(repo, repo, NULL, NULL, NULL, relays,
                                                     maintainers);
  g_assert_nonnull(event);
  nostr_event_set_created_at(event, created_at);
  g_assert_cmpint(nostr_event_sign(event, secret), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

typedef struct {
  gboolean done;
  GnIssueTarget *target;
  GError *error;
} Resolved;

static void
on_resolved(GObject *source, GAsyncResult *result, gpointer data)
{
  Resolved *r = data;
  r->target = gn_issue_repo_resolver_resolve_finish(GN_ISSUE_REPO_RESOLVER(source), result,
                                                    &r->error);
  r->done = TRUE;
}

static gboolean resolved_done(gpointer data) { return ((Resolved *)data)->done; }

/* The inbound REQ frames the relay received, joined. */
static gchar *
inbound_reqs(WireRelay *relay)
{
  GString *out = g_string_new(NULL);
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && g_str_has_prefix(frame->text, "[\"REQ\""))
      g_string_append_printf(out, "%s\n", frame->text);
  }
  return g_string_free(out, FALSE);
}

static gboolean
p_tagged(NostrEvent *event, const char *pubkey)
{
  const NostrTags *tags = nostr_event_get_tags(event);
  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), "p") == 0 &&
        g_strcmp0(nostr_tag_get(tag, 1), pubkey) == 0)
      return TRUE;
  }
  return FALSE;
}

static gboolean
status_says_no_announcement(gpointer data)
{
  return strstr(gtk_label_get_text(GTK_LABEL(data)), "No repository announcement") != NULL;
}

static gboolean
count_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void)widget;
  (void)clock;
  return ++*(guint *)data < 2 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* An AdwDialog opens on the second frame after it is mapped. */
static AdwAlertDialog *
shown_alert(GtkWindow *window)
{
  AdwDialog *dialog = adw_window_get_visible_dialog(ADW_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
  while (!gtk_widget_get_mapped(GTK_WIDGET(dialog)))
    g_main_context_iteration(NULL, TRUE);
  guint ticks = 0;
  gtk_widget_add_tick_callback(GTK_WIDGET(dialog), count_tick, &ticks, NULL);
  while (ticks < 2)
    g_main_context_iteration(NULL, TRUE);
  return ADW_ALERT_DIALOG(dialog);
}

static void
answer(AdwAlertDialog *alert, const char *response)
{
  g_signal_emit_by_name(alert, "response", response);
  adw_dialog_force_close(ADW_DIALOG(alert));
  drain();
}

/* Opt-in evidence: GROUNDHOG_TEST_SCREENSHOTS=<dir> saves the window, with
 * the application's stylesheet, as groundhog-<prefix>-<name>.png. */
static void
screenshot_as(GtkWindow *window, const char *prefix, const char *name)
{
  const char *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
  if (!dir || !*dir)
    return;
  gtk_test_widget_wait_for_draw(GTK_WIDGET(window));
  drain();
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(GTK_WIDGET(window));
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, gdk_paintable_get_intrinsic_width(paintable),
                         gdk_paintable_get_intrinsic_height(paintable));
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node, &bounds);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &bounds);
  g_autofree char *path = g_strdup_printf("%s/groundhog-%s-%s.png", dir, prefix, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s", path);
}

static void
screenshot(GtkWindow *window, const char *name)
{
  screenshot_as(window, "issue-uploads-maintainers", name);
}

static void
never_sign(gpointer data, const gchar *unsigned_json, GCancellable *cancellable,
           GAsyncReadyCallback callback, gpointer callback_data)
{
  (void)data; (void)unsigned_json; (void)cancellable; (void)callback; (void)callback_data;
  g_error("Nothing may be signed as the account in the issue form without consent");
}

static gchar *
never_sign_finish(GAsyncResult *result, GError **error)
{
  (void)result; (void)error;
  return NULL;
}

static void
test_issue_lookup(void)
{
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

  /* A local relay that serves announcements: the owner's current and older
   * one for "repo", the owner's for another repository, and a forgery. */
  WireRelay discovery = { .serve = TRUE, .record = TRUE };
  relay_init(&discovery);
  char *owner_secret = nostr_key_generate_private();
  char *owner = nostr_key_get_public(owner_secret);
  char *forger_secret = nostr_key_generate_private();
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  const char *current[] = { MAINT1, MAINT2, NULL };
  const char *older[] = { MAINT_OLD, NULL };
  const char *forged[] = { MAINT_FORGED, NULL };
  const char *announced[] = { ANNOUNCED, NULL };
  g_autofree gchar *a_current = announcement(owner_secret, "repo", now - 10, current, announced);
  g_autofree gchar *a_older = announcement(owner_secret, "repo", now - 1000, older, NULL);
  g_autofree gchar *a_other = announcement(owner_secret, "other", now, forged, NULL);
  g_autofree gchar *a_forged = announcement(forger_secret, "repo", now, forged, NULL);
  wire_relay_inject(&discovery, a_older);
  wire_relay_inject(&discovery, a_current);
  wire_relay_inject(&discovery, a_other);
  wire_relay_inject(&discovery, a_forged);

  /* No discovery relay: no resolver, so the form offers no lookup. */
  g_settings_set_strv(settings, "discovery-relays", NULL);
  g_assert_null(gh_issue_resolver_new(accounts, settings));
  const char *relays[] = { discovery.url, NULL };
  g_settings_set_strv(settings, "discovery-relays", relays);

  /* The resolver over the application's relay transport: one REQ for the
   * owner's announcement of "repo"; the newest one by the owner wins. */
  g_autoptr(GnIssueRepoResolver) resolver = gh_issue_resolver_new(accounts, settings);
  g_assert_nonnull(resolver);
  g_assert_cmpstr(gh_issue_resolver_get_relays(GH_ISSUE_RESOLVER(resolver))[0], ==, discovery.url);
  drain();
  g_assert_cmpuint(discovery.connections->len, ==, 0);
  g_autoptr(GnIssueTarget) hint = gn_issue_target_new(owner, "repo", NULL, NULL);
  Resolved r = { 0 };
  gn_issue_repo_resolver_resolve_async(resolver, hint, NULL, on_resolved, &r);
  gh_test_spin_until(resolved_done, &r);
  g_assert_no_error(r.error);
  g_assert_nonnull(r.target);
  g_assert_cmpuint(g_strv_length(r.target->maintainers), ==, 2);
  g_assert_cmpstr(r.target->maintainers[0], ==, MAINT1);
  g_assert_cmpstr(r.target->maintainers[1], ==, MAINT2);
  g_assert_cmpuint(g_strv_length(r.target->relays), ==, 1);
  g_assert_cmpstr(r.target->relays[0], ==, ANNOUNCED);
  g_autofree gchar *reqs = inbound_reqs(&discovery);
  g_assert_nonnull(strstr(reqs, "30617"));
  g_assert_nonnull(strstr(reqs, owner));
  g_assert_nonnull(strstr(reqs, "\"#d\":[\"repo\"]"));
  /* The maintainers become p tags of the issue, after the owner. */
  GnIssueDraftInput input = { .title = "Crash", .description = "It crashes." };
  g_autoptr(GnIssueDraft) draft = gn_issue_draft_new(&input, NULL);
  g_assert_nonnull(draft);
  NostrEvent *issue = gn_issue_draft_build_event(draft, r.target, pk);
  g_assert_true(p_tagged(issue, owner));
  g_assert_true(p_tagged(issue, MAINT1));
  g_assert_true(p_tagged(issue, MAINT2));
  g_assert_false(p_tagged(issue, MAINT_OLD));
  g_assert_false(p_tagged(issue, MAINT_FORGED));
  nostr_event_free(issue);
  g_clear_pointer(&r.target, gn_issue_target_free);

  /* Cancelled by the form: no result. */
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  r.done = FALSE;
  gn_issue_repo_resolver_resolve_async(resolver, hint, cancel, on_resolved, &r);
  gh_test_spin_until(resolved_done, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(r.target);
  g_clear_error(&r.error);

  /* The form: the lookup runs only after its consent, which names the
   * relays; nothing connects before. */
  if (g_getenv("GROUNDHOG_TEST_SCREENSHOTS")) {
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  GtkWindow *window = GTK_WINDOW(adw_window_new());
  gtk_window_set_default_size(window, 720, 860);
  gtk_window_present(window);
  guint connections = discovery.connections->len;
  guint frames = discovery.frames->len;
  GnNip34IssueView *dialog = gh_issue_dialog_new(accounts, settings, NULL, NULL, NULL);
  gn_nip34_issue_view_present(dialog, GTK_WIDGET(window));
  GtkWidget *lookup = child(dialog, "lookup_button");
  g_assert_true(gtk_widget_get_visible(lookup));
  g_signal_emit_by_name(lookup, "clicked");
  AdwAlertDialog *alert = shown_alert(window);
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, "Look Up the Repository?");
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert), discovery.url));
  screenshot(window, "lookup-consent");
  drain();
  g_assert_cmpuint(discovery.connections->len, ==, connections);
  answer(alert, "cancel");
  g_assert_cmpuint(discovery.connections->len, ==, connections);
  g_assert_cmpuint(discovery.frames->len, ==, frames);
  g_signal_emit_by_name(lookup, "clicked");
  answer(shown_alert(window), "lookup");
  /* nostrc's own announcement is not on this relay (only "repo" is). */
  gh_test_spin_until(status_says_no_announcement, child(dialog, "status"));
  g_autofree gchar *form_reqs = inbound_reqs(&discovery);
  g_assert_nonnull(strstr(form_reqs, OWNER));
  g_assert_nonnull(strstr(form_reqs, "\"#d\":[\"nostrc\"]"));
  gtk_window_destroy(window);
  drain();

#if GH_ISSUE_UPLOADS
  /* The upload consent offers "use my account" for a server the account has
   * not consented to, unchecked; cancelling contacts and signs nothing. */
  window = GTK_WINDOW(adw_window_new());
  gtk_window_set_default_size(window, 720, 860);
  gtk_window_present(window);
  const char *servers[] = { "https://blossom.example.com", NULL };
  g_settings_set_strv(settings, "blossom-servers", servers);
  g_autoptr(GhNetHttp) http = gh_net_http_new(settings);
  g_autoptr(GhBlossomClient) client = gh_blossom_client_new(settings, http);
  gh_blossom_client_set_account_signer(client, pk, never_sign, never_sign_finish, NULL, NULL);
  dialog = gh_issue_dialog_new(accounts, settings, client, NULL, NULL);
  gn_nip34_issue_view_present(dialog, GTK_WIDGET(window));
  gtk_editable_set_text(GTK_EDITABLE(child(dialog, "title_row")), "Screenshot of the crash");
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(child(dialog, "body_view"))),
                           "The window closes when I open a conversation.", -1);
  g_autofree char *path = g_build_filename(g_get_tmp_dir(), "groundhog-issue-shot.png", NULL);
  g_assert_true(g_file_set_contents(path, "\x89PNG\r\n\x1a\n", 8, NULL));
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_assert_true(gn_nip34_issue_view_add_attachment(dialog, file));
  g_signal_emit_by_name(child(dialog, "review_button"), "clicked");
  alert = shown_alert(window);
  GtkWidget *option = find_named(adw_alert_dialog_get_extra_child(alert), "consent_option_0");
  g_assert_nonnull(option);
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(option)));
  GtkWidget *option_label = gtk_check_button_get_child(GTK_CHECK_BUTTON(option));
  g_assert_nonnull(strstr(GTK_IS_LABEL(option_label)
                            ? gtk_label_get_text(GTK_LABEL(option_label))
                            : gtk_check_button_get_label(GTK_CHECK_BUTTON(option)),
                          "blossom.example.com"));
  screenshot(window, "upload-consent");
  answer(alert, "cancel");
  g_assert_false(gh_blossom_client_get_account_consent(client, servers[0]));
  g_assert_cmpint(g_unlink(path), ==, 0);
  gtk_window_destroy(window);
  drain();
  g_settings_reset(settings, "blossom-servers");
#else
  (void)never_sign;
  (void)never_sign_finish;
#endif

  relay_clear(&discovery);
  free(owner_secret);
  free(owner);
  free(forger_secret);
  g_settings_reset(settings, "discovery-relays");
  gh_test_release(g_steal_pointer(&accounts));
  GhTestSenders check = { &bus, &signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  gh_test_signer_down(&bus, &signer);
  gh_test_bus_down(&bus);
}

#if GH_ISSUE_UPLOADS
/* The blossom client's account signer: signs the kind-24242 authorization
 * with the test key, and counts. */
typedef struct {
  gchar *secret;
  guint calls;
} RetrySigner;

static void
retry_sign(gpointer data, const gchar *unsigned_json, GCancellable *cancellable,
           GAsyncReadyCallback callback, gpointer callback_data)
{
  RetrySigner *signer = data;
  signer->calls++;
  GTask *task = g_task_new(NULL, cancellable, callback, callback_data);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, unsigned_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 24242);
  g_assert_cmpint(nostr_event_sign(event, signer->secret), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_task_return_pointer(task, g_strdup(json), g_free);
  free(json);
  g_object_unref(task);
}

static gchar *
retry_sign_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}

typedef struct {
  GhBlossomClient *client;
  GPtrArray *servers;
} RetryRecorder;

/* What the application's recorder does (gh_attachments_set_consent()). */
static gboolean
retry_record(gpointer data, const gchar *server, GError **error)
{
  (void)error;
  RetryRecorder *recorder = data;
  g_ptr_array_add(recorder->servers, g_strdup(server));
  gh_blossom_client_set_account_consent(recorder->client, server, TRUE);
  return TRUE;
}

static gboolean
widget_visible(gpointer data)
{
  return gtk_widget_get_visible(data);
}

static gboolean
label_says_uploaded(gpointer data)
{
  return g_strcmp0(gtk_label_get_text(GTK_LABEL(data)), "Uploaded") == 0;
}

static GtkLabel *
row_status(GtkWidget *row)
{
  return g_object_get_data(G_OBJECT(row), "gn-upload-status");
}

/* nostrc-j4nvf: a server that accepts only accounts it knows refuses the
 * throwaway key (401). The failed file offers a retry; it asks for the
 * per-server account consent first (recorded as the attachments' consent),
 * then the same file is uploaded again signed by the account (200), and the
 * issue is reviewed with its address. Nothing signs as the account before. */
static void
test_issue_upload_retry(void)
{
  GhTestBus bus = {0};
  GhTestSigner signer = {0};
  gh_test_bus_up_beside_gtk(&bus);
  gh_test_signer_up(&bus, &signer);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_autofree char *npub = gh_test_npub(1);
  g_settings_set_string(settings, "current-npub", npub);
  g_settings_set_string(settings, "signer-method", "auto");
  g_settings_set_string(settings, "network-mode", "none");
  /* The issue's relay: shown in the review, never contacted (not published). */
  const char *relays[] = { "wss://relay.example.com", NULL };
  g_settings_set_strv(settings, "discovery-relays", relays);
  g_autoptr(GhAccountController) accounts = gh_account_controller_new_full(settings, bus.client, identities, NULL);
  gh_test_spin_until(active, accounts);

  BlossomFixture *blossom = blossom_fixture_new();
  const char *servers[] = { blossom_fixture_url(blossom), NULL };
  g_settings_set_strv(settings, "blossom-servers", servers);
  g_autoptr(GhNetHttp) http = gh_net_http_new(settings);
  g_autoptr(GhBlossomClient) client = gh_blossom_client_new(settings, http);
  /* The fixture is on loopback, which a public issue's server may not be. */
  gh_blossom_client_set_allow_private_hosts(client, TRUE);
  char *secret = nostr_key_generate_private();
  char *account = nostr_key_get_public(secret);
  RetrySigner sign = { g_strdup(secret), 0 };
  gh_blossom_client_set_account_signer(client, account, retry_sign, retry_sign_finish, &sign, NULL);
  blossom_fixture_require_pubkey(blossom, account);
  RetryRecorder recorder = { client, g_ptr_array_new_with_free_func(g_free) };

  if (g_getenv("GROUNDHOG_TEST_SCREENSHOTS")) {
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  GtkWindow *window = GTK_WINDOW(adw_window_new());
  gtk_window_set_default_size(window, 720, 860);
  gtk_window_present(window);
  GnNip34IssueView *dialog = gh_issue_dialog_new(accounts, settings, client, retry_record, &recorder);
  gn_nip34_issue_view_present(dialog, GTK_WIDGET(window));
  gtk_editable_set_text(GTK_EDITABLE(child(dialog, "title_row")), "Crash when opening a room");
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(child(dialog, "body_view"))),
                           "The log is attached.", -1);
  g_autofree char *path = g_build_filename(g_get_tmp_dir(), "groundhog-issue-retry.txt", NULL);
  g_assert_true(g_file_set_contents(path, "relay closed\n", -1, NULL));
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_assert_true(gn_nip34_issue_view_add_attachment(dialog, file));
  GtkWidget *retry = child(dialog, "upload_retry");
  GtkWidget *row = gtk_widget_get_ancestor(retry, GTK_TYPE_LIST_BOX_ROW);
  g_assert_false(gtk_widget_get_visible(retry));

  /* The throwaway-key default: the option stays unchecked. */
  g_signal_emit_by_name(child(dialog, "review_button"), "clicked");
  AdwAlertDialog *alert = shown_alert(window);
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, "Upload Files for This Issue?");
  answer(alert, "upload");
  gh_test_spin_until(widget_visible, retry);
  g_assert_cmpstr(gtk_label_get_text(row_status(row)), ==, "Upload failed");
  GPtrArray *requests = blossom_fixture_requests(blossom);
  g_assert_cmpuint(blossom_fixture_count(blossom, "PUT"), ==, 1);
  BlossomRequest *put = g_ptr_array_index(requests, requests->len - 1);
  g_assert_true(put->auth_valid);
  g_assert_cmpstr(put->auth_pubkey, !=, account);
  g_assert_cmpuint(sign.calls, ==, 0);
  g_assert_cmpuint(recorder.servers->len, ==, 0);
  /* The form waits for the user instead of reviewing without the file. */
  g_assert_false(ADW_IS_ALERT_DIALOG(adw_window_get_visible_dialog(ADW_WINDOW(window))));
  g_assert_true(gtk_widget_is_sensitive(retry));
  g_assert_false(gtk_widget_is_sensitive(child(dialog, "attach_button")));
  screenshot_as(window, "issue-upload-retry", "after-refused");

  /* Cancelling the consent records, signs and sends nothing. */
  g_signal_emit_by_name(retry, "clicked");
  alert = shown_alert(window);
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, "Upload as Your Account?");
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert), "Upload to 127.0.0.1 as my account"));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert), "groundhog-issue-retry.txt"));
  screenshot_as(window, "issue-upload-retry", "after-consent");
  answer(alert, "cancel");
  g_assert_true(gtk_widget_get_visible(retry));
  g_assert_cmpuint(recorder.servers->len, ==, 0);
  g_assert_false(gh_blossom_client_get_account_consent(client, servers[0]));
  g_assert_cmpuint(blossom_fixture_count(blossom, "PUT"), ==, 1);

  /* Accepted: the consent is kept for this server, then the account signs. */
  g_signal_emit_by_name(retry, "clicked");
  answer(shown_alert(window), "retry");
  g_assert_cmpuint(recorder.servers->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(recorder.servers, 0), ==, servers[0]);
  g_assert_false(gtk_widget_get_visible(retry));
  gh_test_spin_until(label_says_uploaded, row_status(row));
  g_assert_cmpuint(sign.calls, ==, 1);
  g_assert_cmpuint(blossom_fixture_count(blossom, "PUT"), ==, 2);
  put = g_ptr_array_index(requests, requests->len - 1);
  g_assert_true(put->auth_valid);
  g_assert_cmpstr(put->auth_pubkey, ==, account);

  /* The issue is reviewed again, with the file's address. */
  alert = shown_alert(window);
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, "Publish This Issue Publicly?");
  GtkWidget *preview = adw_alert_dialog_get_extra_child(alert);
  while (preview && !GTK_IS_LABEL(preview))
    preview = gtk_widget_get_first_child(preview);
  g_assert_nonnull(preview);
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(preview)), servers[0]));
  screenshot_as(window, "issue-upload-retry", "after-review");
  answer(alert, "cancel");

  g_assert_cmpint(g_unlink(path), ==, 0);
  gtk_window_destroy(window);
  drain();
  gh_blossom_client_set_account_signer(client, NULL, NULL, NULL, NULL, NULL);
  g_ptr_array_unref(recorder.servers);
  g_free(sign.secret);
  free(secret);
  free(account);
  g_clear_object(&client);
  g_clear_object(&http);
  drain();
  blossom_fixture_free(blossom);
  g_settings_reset(settings, "blossom-servers");
  g_settings_reset(settings, "network-mode");
  g_settings_reset(settings, "discovery-relays");
  gh_test_release(g_steal_pointer(&accounts));
  GhTestSenders check = { &bus, &signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  gh_test_signer_down(&bus, &signer);
  gh_test_bus_down(&bus);
}
#endif

void
gh_test_about_flows_register(void)
{
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  g_test_add_func("/groundhog/about/npub-dm", test_npub);
  g_test_add_func("/groundhog/about/nprofile-dm", test_nprofile);
  g_test_add_data_func("/groundhog/about/issue-consent", GINT_TO_POINTER(0), test_issue_consent);
  g_test_add_data_func("/groundhog/about/issue-account-changed", GINT_TO_POINTER(1), test_issue_consent);
  g_test_add_data_func("/groundhog/about/issue-close-signing", GINT_TO_POINTER(2), test_issue_consent);
  g_test_add_data_func("/groundhog/about/issue-fields-diagnostics", GINT_TO_POINTER(3), test_issue_consent);
  g_test_add_func("/groundhog/about/issue-lookup", test_issue_lookup);
#if GH_ISSUE_UPLOADS
  g_test_add_func("/groundhog/about/issue-upload-retry", test_issue_upload_retry);
#endif
}
