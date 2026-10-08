#include "gh-nip46-pair-dialog.h"
#include "gh-identity.h"
#include "nostrc-test-gdk-frame.h"
#include "gh-test-bunker.h"
#include <nostr/nip46/nip46_uri.h>

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
  if (bunker->scopes->len == 0) {
    g_free(bunker->client_pubkey);
    bunker->client_pubkey = g_strdup(nostr_filter_tag_get(&filters->filters[0], 0, 1));
  }
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
  g_source_remove(timer);
  g_autofree gchar *selected = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(selected, ==, "");
  g_assert_true(adw_dialog_close(ADW_DIALOG(alert)));
  g_object_unref(alert);
  drain();
  g_clear_pointer(&selected, g_free);
  selected = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(selected, ==, "");
  for (guint i = 0; i < bunker.scopes->len; i++)
    g_assert_true(((BunkerHandle *)g_ptr_array_index(bunker.scopes, i))->closed);
  nostr_nip46_uri_connect_free(&parsed);
  adw_dialog_close(ADW_DIALOG(dialog));
  gtk_window_destroy(GTK_WINDOW(window));
  drain();
  bunker_clear(&bunker);
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
  return g_test_run();
}
