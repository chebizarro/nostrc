#include "gh-nip46-pair-dialog.h"
#include "gh-nip46-auth-url.h"
#include "gh-identity.h"
#include "nostrc-test-gdk-frame.h"
#include "gh-test-bunker.h"
#include <nostr/nip46/nip46_uri.h>
#include <nostr/nip19/nip19.h>

void groundhog_register_resource(void);

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gboolean closed;
} ScopeHandle;

typedef struct {
  GPtrArray *handles;
  guint closed;
} Recorder;

static void
handle_free(gpointer data)
{
  ScopeHandle *handle = data;
  gh_relay_scope_unref(handle->scope);
  g_free(handle->url);
  g_free(handle);
}

static gpointer
scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
           gpointer data, GError **error)
{
  (void)filters; (void)error;
  Recorder *recorder = data;
  ScopeHandle *handle = g_new0(ScopeHandle, 1);
  handle->scope = gh_relay_scope_ref(scope);
  handle->url = g_strdup(url);
  g_ptr_array_add(recorder->handles, handle);
  return handle;
}

static void
scope_close(gpointer data, gpointer user_data)
{
  ScopeHandle *handle = data;
  Recorder *recorder = user_data;
  if (!handle->closed) {
    handle->closed = TRUE;
    recorder->closed++;
  }
}

static const GhRelayTransport transport = { scope_open, scope_close };

static GPtrArray *
empty_identities(gpointer data, GError **error)
{
  (void)data; (void)error;
  return g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
}

static void
drain(void)
{
  for (guint i = 0; i < 100 && g_main_context_pending(NULL); i++)
    g_main_context_iteration(NULL, FALSE);
}

static void
test_qr_readiness_and_cancel(void)
{
  Recorder recorder = { .handles = g_ptr_array_new_with_free_func(handle_free) };
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_autoptr(GhAccountController) accounts = gh_account_controller_new_full(
    settings, NULL, empty_identities, NULL);
  GhNip46PairConfig config = { .accounts = accounts, .settings = settings,
    .scope_transport = &transport, .transport_data = &recorder };
  GtkWidget *window = gtk_window_new();
  GhNip46PairDialog *dialog = gh_nip46_pair_dialog_new(&config);
  adw_dialog_present(ADW_DIALOG(dialog), window);
  drain();
  g_assert_cmpuint(recorder.handles->len, ==, 4);
  g_assert_false(gh_nip46_pair_dialog_qr_is_visible(dialog));
  ScopeHandle *first = g_ptr_array_index(recorder.handles, 0);
  gh_relay_scope_eose(first->scope, first->url);
  drain();
  g_assert_true(gh_nip46_pair_dialog_qr_is_visible(dialog));

  GtkStack *stack = GTK_STACK(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "mode_stack"));
  AdwEntryRow *uri = ADW_ENTRY_ROW(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "uri_row"));
  g_assert_nonnull(stack);
  g_assert_true(g_str_has_prefix(gtk_editable_get_text(GTK_EDITABLE(uri)), "nostrconnect://"));
  gtk_stack_set_visible_child_name(stack, "bunker");
  drain();
  g_assert_false(gh_nip46_pair_dialog_qr_is_visible(dialog));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(uri)), ==, "");
  g_assert_cmpuint(recorder.closed, ==, 4);

  AdwEntryRow *bunker = ADW_ENTRY_ROW(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "bunker_row"));
  GtkButton *connect = GTK_BUTTON(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "connect_button"));
  GtkLabel *error = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "error_label"));
  gtk_editable_set_text(GTK_EDITABLE(bunker), "nostrconnect://not-a-bunker");
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(connect)));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(error)));
  g_assert_nonnull(g_strstr_len(gtk_label_get_text(error), -1, "QR tab"));
  g_autofree gchar *valid_bunker = g_strdup_printf(
    "bunker://%064d?relay=wss%%3A%%2F%%2Fnos.lol", 1);
  gtk_editable_set_text(GTK_EDITABLE(bunker), valid_bunker);
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(connect)));
  GtkLabel *details = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "bunker_details"));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(details)));
  g_assert_nonnull(g_strstr_len(gtk_label_get_text(details), -1, "1 relay"));

  adw_dialog_close(ADW_DIALOG(dialog));
  gtk_window_destroy(GTK_WINDOW(window));
  drain();
  g_ptr_array_unref(recorder.handles);
}

static gpointer
qr_scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
              gpointer data, GError **error)
{
  TestBunker *bunker = data;
  g_free(bunker->client_pubkey);
  bunker->client_pubkey = g_strdup(nostr_filter_tag_get(&filters->filters[0], 0, 1));
  return bunker_scope_open(scope, url, filters, data, error);
}

static const GhRelayTransport qr_scope_transport = { qr_scope_open, bunker_close };

static void
capture_confirmation(GhNip46PairDialog *dialog, AdwAlertDialog *alert, gpointer data)
{
  (void)dialog;
  AdwAlertDialog **result = data;
  *result = g_object_ref(alert);
}

static gboolean
deadline(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static gboolean
bunker_scopes_closed(TestBunker *bunker)
{
  for (guint i = 0; i < bunker->scopes->len; i++)
    if (!((BunkerHandle *)g_ptr_array_index(bunker->scopes, i))->closed)
      return FALSE;
  return TRUE;
}

static void
test_confirmation_cancel_discards_attempt(void)
{
  /* The fixture's client pubkey is replaced from the subscription filter by
   * qr_scope_open; the QR transport secret remains private to the dialog. */
  TestBunker bunker;
  bunker_init(&bunker,
    "1111111111111111111111111111111111111111111111111111111111111111");
  (void)bunker_scope_transport;
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_autoptr(GhAccountController) accounts = gh_account_controller_new_full(
    settings, NULL, empty_identities, NULL);
  GhNip46PairConfig config = { .accounts = accounts, .settings = settings,
    .scope_transport = &qr_scope_transport,
    .publish_transport = &bunker_publish_transport, .transport_data = &bunker };
  GtkWidget *window = gtk_window_new();
  GhNip46PairDialog *dialog = gh_nip46_pair_dialog_new(&config);
  AdwAlertDialog *alert = NULL;
  g_signal_connect(dialog, "confirmation-presented", G_CALLBACK(capture_confirmation), &alert);
  adw_dialog_present(ADW_DIALOG(dialog), window);
  AdwEntryRow *uri_row = ADW_ENTRY_ROW(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "uri_row"));
  /* The link is deliberately not shown until EOSE, but the session's REQ
   * already carries the ephemeral client pubkey. */
  g_assert_cmpuint(bunker.scopes->len, ==, 4);
  bunker_eose(&bunker);
  NostrNip46ConnectURI parsed = {0};
  g_assert_cmpint(nostr_nip46_uri_parse_connect(
    gtk_editable_get_text(GTK_EDITABLE(uri_row)), &parsed), ==, 0);
  g_assert_cmpstr(parsed.client_pubkey_hex, ==, bunker.client_pubkey);

  g_autofree gchar *secret = g_strdup_printf("\"%s\"", parsed.secret);
  char *connect = nostr_nip46_response_build_ok("connect", secret);
  bunker_reply(&bunker, connect);
  free(connect);
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline, &expired);
  while (bunker.publishes->len == 0 && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_assert_false(expired);
  bunker_accept(&bunker);
  g_autofree gchar *request_json = bunker_request_json(&bunker);
  NostrNip46Request request = {0};
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_assert_cmpstr(request.method, ==, "get_public_key");
  g_autofree gchar *quoted_user = g_strdup_printf("\"%s\"", bunker.user_pubkey);
  char *response = nostr_nip46_response_build_ok(request.id, quoted_user);
  bunker_reply(&bunker, response);
  free(response);
  nostr_nip46_request_free(&request);
  while (!alert && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_assert_false(expired);
  g_assert_nonnull(alert);
  while (!gtk_widget_get_mapped(GTK_WIDGET(alert)) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_assert_false(expired);
  g_source_remove(timer);
  g_autofree gchar *selected = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(selected, ==, "");
  g_assert_true(adw_dialog_close(ADW_DIALOG(alert)));
  expired = FALSE;
  timer = g_timeout_add_seconds(5, deadline, &expired);
  while (!bunker_scopes_closed(&bunker) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (!expired) g_source_remove(timer);
  g_object_unref(alert);
  g_assert_false(expired);
  g_clear_pointer(&selected, g_free);
  selected = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(selected, ==, "");
  nostr_nip46_uri_connect_free(&parsed);
  adw_dialog_close(ADW_DIALOG(dialog));
  gtk_window_destroy(GTK_WINDOW(window));
  drain();
  bunker_clear(&bunker);
}

typedef struct {
  GMutex lock;
  gchar *npub;
  gchar *client_secret;
  gchar *signer_pubkey;
  gchar **relays;
  gboolean fail_save;
  guint save_calls;
} PairStore;

static gchar *
npub_for_pubkey(const gchar *hex)
{
  guint8 bytes[32];
  for (guint i = 0; i < 32; i++) {
    gchar pair[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
    bytes[i] = (guint8)strtoul(pair, NULL, 16);
  }
  char *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  return npub;
}

static GPtrArray *
paired_identities(gpointer data, GError **error)
{
  (void)error;
  PairStore *store = data;
  GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  g_mutex_lock(&store->lock);
  if (store->npub) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(store->npub);
    info->label = g_strdup("Remote signer");
    info->backend = GH_SIGNER_BACKEND_NIP46;
    g_ptr_array_add(items, info);
  }
  g_mutex_unlock(&store->lock);
  return items;
}

static void
save_pair_for_test(const GhNip46Credential *credential, GAsyncReadyCallback callback,
                   gpointer callback_data, gpointer data)
{
  PairStore *store = data;
  g_mutex_lock(&store->lock);
  store->save_calls++;
  if (!store->fail_save) {
    store->npub = npub_for_pubkey(gh_nip46_credential_get_user_pubkey_hex(credential));
    store->client_secret = g_strdup(gh_nip46_credential_get_client_secret_hex(credential));
    store->signer_pubkey = g_strdup(gh_nip46_credential_get_remote_signer_pubkey_hex(credential));
    store->relays = g_strdupv((gchar **)gh_nip46_credential_get_relays(credential));
  }
  g_mutex_unlock(&store->lock);
  g_autoptr(GTask) task = g_task_new(NULL, NULL, callback, callback_data);
  if (store->fail_save)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Test credential store rejected save");
  else
    g_task_return_boolean(task, TRUE);
}

static void
pair_store_clear(PairStore *store)
{
  g_mutex_clear(&store->lock);
  g_free(store->npub);
  g_free(store->client_secret);
  g_free(store->signer_pubkey);
  g_strfreev(store->relays);
}

static GtkButton *
find_button(GtkWidget *widget, const gchar *label)
{
  if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    return GTK_BUTTON(widget);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkButton *found = find_button(child, label);
    if (found) return found;
  }
  return NULL;
}

/* Clicks the alert's own response button, as a user does: AdwAlertDialog
 * then closes itself before it emits "response" (emitting "response"
 * directly hid nostrc-p15n5.3). */
static void
click_response(AdwAlertDialog *alert, const gchar *label)
{
  GtkButton *button = find_button(GTK_WIDGET(alert), label);
  g_assert_nonnull(button);
  g_signal_emit_by_name(button, "clicked");
}

static void
wait_for_confirmation(AdwAlertDialog **alert)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline, &expired);
  while (!*alert && !expired) g_main_context_iteration(NULL, TRUE);
  g_assert_false(expired);
  g_source_remove(timer);
}

static void
wait_for_publishes(TestBunker *bunker, guint count)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline, &expired);
  while (bunker->publishes->len < count && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_assert_false(expired);
  g_source_remove(timer);
}

static void
call_done(GObject *source, GAsyncResult *result, gpointer data)
{
  gboolean *done = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *value = gh_nip46_session_call_finish(GH_NIP46_SESSION(source), result, &error);
  g_assert_no_error(error);
  *done = TRUE;
}

static void
ui_bunker_reply(TestBunker *bunker, const gchar *response)
{
  g_autofree gchar *json = bunker_response_event_json_from(bunker,
    bunker->signer_secret, bunker->client_pubkey, response);
  BunkerHandle *scope = g_ptr_array_index(bunker->scopes, bunker->scopes->len - 1);
  gh_relay_scope_event(scope->scope, scope->url, json);
}

static void
test_bunker_save_order(gconstpointer data)
{
  gboolean fail_save = GPOINTER_TO_INT(data);
  PairStore store = { .fail_save = fail_save };
  g_mutex_init(&store.lock);
  TestBunker bunker;
  bunker_init(&bunker, "1111111111111111111111111111111111111111111111111111111111111111");
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
  g_autoptr(GhAccountController) accounts = gh_account_controller_new_full_with_remote_list(
    settings, NULL, empty_identities, NULL, paired_identities, &store);
  GhNip46PairConfig config = { .accounts = accounts, .settings = settings,
    .scope_transport = &qr_scope_transport,
    .publish_transport = &bunker_publish_transport, .transport_data = &bunker,
    .test_save = save_pair_for_test, .test_save_data = &store };
  GtkWidget *window = gtk_window_new();
  GhNip46PairDialog *dialog = gh_nip46_pair_dialog_new(&config);
  g_object_ref_sink(dialog);
  AdwAlertDialog *alert = NULL;
  g_signal_connect(dialog, "confirmation-presented", G_CALLBACK(capture_confirmation), &alert);
  adw_dialog_present(ADW_DIALOG(dialog), window);
  GtkStack *stack = GTK_STACK(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "mode_stack"));
  gtk_stack_set_visible_child_name(stack, "bunker");
  drain();
  AdwEntryRow *entry = ADW_ENTRY_ROW(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "bunker_row"));
  GtkButton *button = GTK_BUTTON(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "connect_button"));
  GtkLabel *error = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "error_label"));
  gtk_editable_set_text(GTK_EDITABLE(entry), "nostrconnect://not-a-bunker");
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(button)));
  g_assert_nonnull(strstr(gtk_label_get_text(error), "QR tab"));
  g_autofree gchar *uri = g_strdup_printf("bunker://%s?relay=wss%%3A%%2F%%2Fnos.lol&secret=once",
                                          bunker.signer_pubkey);
  gtk_editable_set_text(GTK_EDITABLE(entry), uri);
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(button)));
  g_signal_emit_by_name(button, "clicked");
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "");
  g_assert_cmpuint(bunker.scopes->len, ==, 5);
  bunker_eose(&bunker);
  wait_for_publishes(&bunker, 1);
  g_autofree gchar *connect_json = bunker_request_json(&bunker);
  NostrNip46Request request = {0};
  g_assert_cmpint(nostr_nip46_request_parse(connect_json, &request), ==, 0);
  g_assert_cmpstr(request.method, ==, "connect");
  g_assert_cmpstr(request.params[1], ==, "once");
  g_autofree gchar *connect_id = g_strdup(request.id);
  nostr_nip46_request_free(&request);
  bunker_accept(&bunker);
  char *reply = nostr_nip46_response_build_ok(connect_id, "\"ack\"");
  ui_bunker_reply(&bunker, reply);
  free(reply);
  wait_for_publishes(&bunker, 2);
  g_autofree gchar *key_json = bunker_request_json(&bunker);
  g_assert_null(strstr(key_json, "once"));
  g_assert_cmpint(nostr_nip46_request_parse(key_json, &request), ==, 0);
  g_assert_cmpstr(request.method, ==, "get_public_key");
  bunker_accept(&bunker);
  g_autofree gchar *quoted_user = g_strdup_printf("\"%s\"", bunker.user_pubkey);
  reply = nostr_nip46_response_build_ok(request.id, quoted_user);
  ui_bunker_reply(&bunker, reply);
  free(reply);
  nostr_nip46_request_free(&request);
  wait_for_confirmation(&alert);
  g_assert_cmpuint(store.save_calls, ==, 0);
  g_autofree gchar *current = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(current, ==, "");
  click_response(alert, "Save Remote Signer");
  g_object_unref(alert);
  GtkLabel *details = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "bunker_details"));
  GtkWidget *cancel = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(dialog),
    GH_TYPE_NIP46_PAIR_DIALOG, "cancel_button"));
  /* Progress shows on the bunker tab, and Cancel stays available. */
  g_assert_nonnull(strstr(gtk_label_get_text(details), "Saving"));
  g_assert_true(gtk_widget_get_visible(cancel));
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline, &expired);
  while (store.save_calls == 0 && !expired) g_main_context_iteration(NULL, TRUE);
  g_assert_false(expired);
  if (!fail_save) {
    while (!expired) {
      g_autofree gchar *selected = g_settings_get_string(settings, "current-npub");
      if (g_strcmp0(selected, store.npub) == 0) break;
      g_main_context_iteration(NULL, TRUE);
    }
    g_assert_false(expired);
    g_assert_cmpstr(store.signer_pubkey, ==, bunker.signer_pubkey);
    g_assert_cmpuint(strlen(store.client_secret), ==, 64);
    g_assert_null(strstr(store.client_secret, "once"));
    g_assert_cmpstr(store.relays[0], ==, "wss://nos.lol");
    g_assert_cmpint(gh_account_controller_get_active_backend(accounts), ==, GH_SIGNER_BACKEND_NIP46);
    /* Reopening from the saved credential can only send the requested method,
     * never the one-use bunker connect token. */
    g_autoptr(GError) session_error = NULL;
    g_autoptr(GhNip46Session) retry = gh_nip46_session_new(store.client_secret,
      store.signer_pubkey, (const gchar *const *)store.relays, &qr_scope_transport,
      NULL, &bunker_publish_transport, NULL, &bunker, &session_error);
    g_assert_no_error(session_error);
    g_assert_nonnull(retry);
    gh_nip46_session_start(retry);
    bunker_eose(&bunker);
    gboolean done = FALSE;
    gh_nip46_session_call_async(retry, "get_public_key", NULL, 0, NULL, call_done, &done);
    wait_for_publishes(&bunker, 3);
    g_autofree gchar *retry_json = bunker_request_json(&bunker);
    g_assert_null(strstr(retry_json, "once"));
    g_assert_cmpint(nostr_nip46_request_parse(retry_json, &request), ==, 0);
    g_assert_cmpstr(request.method, ==, "get_public_key");
    bunker_accept(&bunker);
    reply = nostr_nip46_response_build_ok(request.id, quoted_user);
    ui_bunker_reply(&bunker, reply);
    free(reply);
    nostr_nip46_request_free(&request);
    while (!done && !expired) g_main_context_iteration(NULL, TRUE);
    g_assert_true(done);
    gh_nip46_session_cancel(retry);
  } else {
    drain();
    g_assert_null(store.npub);
    g_autofree gchar *selected = g_settings_get_string(settings, "current-npub");
    g_assert_cmpstr(selected, ==, "");
    GPtrArray *identities = gh_account_controller_get_identities(accounts);
    g_assert_true(!identities || identities->len == 0);
    g_assert_nonnull(strstr(gtk_label_get_text(error), "rejected save"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(gtk_widget_get_template_child(
      GTK_WIDGET(dialog), GH_TYPE_NIP46_PAIR_DIALOG, "bunker_details"))), ==, "Not saved.");
  }
  if (!expired) g_source_remove(timer);
  if (fail_save) adw_dialog_close(ADW_DIALOG(dialog));
  gtk_window_destroy(GTK_WINDOW(window));
  drain();
  g_object_unref(dialog);
  bunker_clear(&bunker);
  pair_store_clear(&store);
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
}

typedef struct {
  TestBunker *bunker;
  GhNip46Session *session;
  guint auth_count;
  gchar *last_url;
} AccountAuth;

static GhNip46Session *
account_session_for_test(const gchar *npub, gpointer data)
{
  AccountAuth *auth = data;
  g_autofree gchar *expected = npub_for_pubkey(auth->bunker->user_pubkey);
  g_assert_cmpstr(npub, ==, expected);
  const gchar *relays[] = { "wss://nos.lol", NULL };
  g_autoptr(GError) error = NULL;
  GhNip46Session *session = gh_nip46_session_new(auth->bunker->client_secret,
    auth->bunker->signer_pubkey, relays, &bunker_scope_transport, NULL,
    &bunker_publish_transport, NULL, auth->bunker, &error);
  g_assert_no_error(error);
  auth->session = g_object_ref(session);
  return session;
}

static gboolean
capture_account_auth(GhAccountController *accounts, const gchar *url, gpointer data)
{
  (void)accounts;
  AccountAuth *auth = data;
  auth->auth_count++;
  g_free(auth->last_url);
  auth->last_url = g_strdup(url);
  return TRUE;
}

static void
test_controller_auth_url(void)
{
  TestBunker bunker;
  bunker_init(&bunker, "1111111111111111111111111111111111111111111111111111111111111111");
  PairStore store = {0};
  g_mutex_init(&store.lock);
  store.npub = npub_for_pubkey(bunker.user_pubkey);
  AccountAuth auth = { .bunker = &bunker };
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
  g_autoptr(GhAccountController) accounts = gh_account_controller_new_full_with_remote_list(
    settings, NULL, empty_identities, NULL, paired_identities, &store);
  gh_account_controller_set_session_factory_for_test(accounts, account_session_for_test, &auth);
  g_signal_connect(accounts, "auth-url", G_CALLBACK(capture_account_auth), &auth);
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline, &expired);
  while (!gh_account_controller_get_identities(accounts) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_assert_false(expired);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_account_controller_select_backend(accounts, GH_SIGNER_BACKEND_NIP46,
                                                       store.npub, &error));
  g_assert_no_error(error);
  while (!auth.session && !expired) g_main_context_iteration(NULL, TRUE);
  g_assert_false(expired);
  bunker_eose(&bunker);
  gh_nip46_session_call_async(auth.session, "get_public_key", NULL, 0, NULL, NULL, NULL);
  wait_for_publishes(&bunker, 1);
  bunker_accept(&bunker);
  g_autofree gchar *json = bunker_request_json(&bunker);
  NostrNip46Request request = {0};
  g_assert_cmpint(nostr_nip46_request_parse(json, &request), ==, 0);
  g_autofree gchar *response = g_strdup_printf(
    "{\"id\":\"%s\",\"result\":\"auth_url\",\"error\":\"https://bunker.test.invalid/approve\"}",
    request.id);
  bunker_reply(&bunker, response);
  bunker_reply(&bunker, response);
  g_assert_cmpuint(auth.auth_count, ==, 1);
  g_assert_cmpstr(auth.last_url, ==, "https://bunker.test.invalid/approve");
  /* Account revocation closes this session; late replies cannot re-open it. */
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
  drain();
  bunker_reply(&bunker, response);
  g_assert_cmpuint(auth.auth_count, ==, 1);
  nostr_nip46_request_free(&request);
  if (!expired) g_source_remove(timer);
  gh_nip46_session_cancel(auth.session);
  g_object_unref(auth.session);
  g_free(auth.last_url);
  bunker_clear(&bunker);
  pair_store_clear(&store);
}

static void
capture_notice(GNotification *notice, gpointer data)
{
  (void)notice;
  guint *count = data;
  (*count)++;
}

static void
test_hidden_auth_notification_revoked(void)
{
  g_autoptr(GtkApplication) app = gtk_application_new(
    "org.nostr.Groundhog.TestAuth", G_APPLICATION_NON_UNIQUE);
  g_autoptr(GhNip46AuthUrl) prompt = gh_nip46_auth_url_new(app, NULL);
  guint notices = 0;
  gh_nip46_auth_url_set_notice_hook_for_test(prompt, capture_notice, &notices);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_assert_true(gh_nip46_auth_url_handle(prompt,
    "https://bunker.test.invalid/approve", cancel));
  g_assert_cmpuint(notices, ==, 1);
  g_assert_true(gh_nip46_auth_url_has_pending(prompt));
  g_cancellable_cancel(cancel);
  gh_nip46_auth_url_activate_for_test(prompt);
  g_assert_false(gh_nip46_auth_url_has_pending(prompt));
}

int
main(int argc, char **argv)
{
  if (!gtk_init_check()) return 77;
  adw_init();
  groundhog_register_resource();
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/groundhog/nip46/pair-dialog/qr-readiness-and-cancel",
                  test_qr_readiness_and_cancel);
  g_test_add_func("/groundhog/nip46/pair-dialog/confirmation-cancel",
                  test_confirmation_cancel_discards_attempt);
  g_test_add_data_func("/groundhog/nip46/pair-dialog/bunker-save-select",
                       GINT_TO_POINTER(FALSE), test_bunker_save_order);
  g_test_add_data_func("/groundhog/nip46/pair-dialog/bunker-save-failure",
                       GINT_TO_POINTER(TRUE), test_bunker_save_order);
  g_test_add_func("/groundhog/nip46/pair-dialog/controller-auth-url",
                  test_controller_auth_url);
  g_test_add_func("/groundhog/nip46/pair-dialog/hidden-auth-revoked",
                  test_hidden_auth_notification_revoked);
  return g_test_run();
}
