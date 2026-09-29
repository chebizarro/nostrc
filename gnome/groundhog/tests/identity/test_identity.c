#include "gh-identity.h"
#include "gh-signer.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr/nip19/nip19.h"
#include "nostrc-test-bus.h"

#include <string.h>

static const gchar *key_one =
  "0000000000000000000000000000000000000000000000000000000000000001";
static const gchar *key_two =
  "0000000000000000000000000000000000000000000000000000000000000002";
static gchar *pub_one, *pub_two, *npub_one, *npub_two, *ciphertext;
static NostrcTestBus *test_bus;
static GDBusConnection *server, *client; /* owned by test_bus */
static GDBusNodeInfo *node;
static guint registration;
static GPtrArray *held_calls;
static GHashTable *typed_senders;
static gchar *last_sender;
static const gchar *expected_npub;

typedef enum { MOCK_OK, MOCK_DENIED, MOCK_TIMEOUT, MOCK_DENIED_TIMEOUT_TEXT,
               MOCK_HOLD, MOCK_WRONG_KEY, MOCK_CHANGED_CONTENT,
               MOCK_BAD_CIPHER, MOCK_APPROVAL_TIMEOUT, MOCK_NO_AGENT,
               MOCK_TIMEOUT_DENIED_TEXT, MOCK_IDENTITY_CHANGED } MockMode;
static MockMode mode;

static gchar *
npub_from_hex(const gchar *hex)
{
  guint8 bytes[32];
  for (guint i = 0; i < 32; i++) {
    unsigned int value;
    g_assert_cmpint(sscanf(hex + 2 * i, "%2x", &value), ==, 1);
    bytes[i] = value;
  }
  gchar *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  return npub;
}

static void
method_call(GDBusConnection *connection, const gchar *sender, const gchar *path,
            const gchar *interface, const gchar *method, GVariant *parameters,
            GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)connection; (void)path; (void)interface; (void)user_data;
  if (g_str_equal(method, "EnableTypedApprovalErrors")) {
    /* Sent without a reply callback: no reply (hence no stale one) exists. */
    g_assert_true(g_dbus_message_get_flags(g_dbus_method_invocation_get_message(invocation)) &
                  G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
    g_hash_table_add(typed_senders, g_strdup(sender));
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }
  /* Every gated call is preceded by the typed-error opt-in on its connection. */
  g_assert_true(g_hash_table_remove(typed_senders, sender));
  g_free(last_sender);
  last_sender = g_strdup(sender);
  const gchar *a, *b, *c;
  g_variant_get(parameters, "(&s&s&s)", &a, &b, &c);
  if (g_str_equal(method, "SignEvent")) {
    g_assert_cmpstr(b, ==, expected_npub);
    g_assert_cmpstr(c, ==, "");
  } else {
    g_assert_true(g_str_equal(method, "NIP44Encrypt") ||
                  g_str_equal(method, "NIP44Decrypt"));
    g_assert_cmpstr(b, ==, pub_two);
    g_assert_cmpstr(c, ==, expected_npub);
  }
  if (mode == MOCK_DENIED || mode == MOCK_DENIED_TIMEOUT_TEXT) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.ApprovalDenied",
      mode == MOCK_DENIED_TIMEOUT_TEXT ? "user timed out, approval denied" : "approval denied");
    return;
  }
  if (mode == MOCK_APPROVAL_TIMEOUT || mode == MOCK_TIMEOUT_DENIED_TEXT) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.ApprovalTimedOut",
      mode == MOCK_TIMEOUT_DENIED_TEXT ? "user denied" : "approval timed out");
    return;
  }
  if (mode == MOCK_IDENTITY_CHANGED) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.IdentityChanged", "user denied");
    return;
  }
  if (mode == MOCK_NO_AGENT) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.NoApprovalAgent", "approval denied: timed out");
    return;
  }
  if (mode == MOCK_TIMEOUT) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.freedesktop.DBus.Error.NoReply", "transport deadline elapsed");
    return;
  }
  if (mode == MOCK_HOLD) {
    g_assert_cmpstr(sender, !=, g_dbus_connection_get_unique_name(client));
    g_ptr_array_add(held_calls, g_object_ref(invocation));
    return;
  }
  if (g_str_equal(method, "SignEvent")) {
    NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_compact(event, a, NULL), ==, 1);
    if (mode == MOCK_CHANGED_CONTENT)
      nostr_event_set_content(event, "changed by signer");
    g_assert_cmpint(nostr_event_sign(event, mode == MOCK_WRONG_KEY ? key_two : key_one), ==, 0);
    gchar *json = nostr_event_serialize_compact(event);
    g_assert_nonnull(json);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", json));
    free(json);
    nostr_event_free(event);
  } else if (g_str_equal(method, "NIP44Encrypt")) {
    g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", mode == MOCK_BAD_CIPHER ? "invalid" : ciphertext));
  } else {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "plaintext"));
  }
}

static const GDBusInterfaceVTable vtable = { method_call, NULL, NULL, { 0 } };

static void
setup(void)
{
  pub_one = nostr_key_get_public(key_one);
  pub_two = nostr_key_get_public(key_two);
  g_assert_nonnull(pub_one);
  g_assert_nonnull(pub_two);
  npub_one = npub_from_hex(pub_one);
  npub_two = npub_from_hex(pub_two);
  guint8 payload[99] = { 2 };
  ciphertext = g_base64_encode(payload, sizeof payload);
  held_calls = g_ptr_array_new_with_free_func(g_object_unref);
  typed_senders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  expected_npub = npub_one;
  test_bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(test_bus);
  GError *error = NULL;
  server = nostrc_test_bus_connect(test_bus);
  node = g_dbus_node_info_new_for_xml(
    "<node><interface name='org.nostr.Signer'>"
    "<method name='EnableTypedApprovalErrors'/>"
    "<method name='SignEvent'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='NIP44Encrypt'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='NIP44Decrypt'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "</interface></node>", &error);
  g_assert_no_error(error);
  registration = g_dbus_connection_register_object(server, "/org/nostr/signer",
    node->interfaces[0], &vtable, NULL, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(registration, >, 0);
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(server,
    "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "RequestName", g_variant_new("(su)", "org.nostr.Signer", 4u),
    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
  g_assert_no_error(error);
  client = nostrc_test_bus_connect(test_bus);
}

static void
teardown(void)
{
  while (held_calls->len) {
    GDBusMethodInvocation *invocation = g_ptr_array_index(held_calls, 0);
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.ApprovalDenied", "test cleanup");
    g_ptr_array_remove_index(held_calls, 0);
  }
  g_clear_pointer(&held_calls, g_ptr_array_unref);
  g_clear_pointer(&typed_senders, g_hash_table_unref);
  g_dbus_connection_unregister_object(server, registration);
  g_clear_pointer(&node, g_dbus_node_info_unref);
  nostrc_test_bus_down(test_bus); /* releases server and client */
  test_bus = NULL;
  server = client = NULL;
  g_clear_pointer(&last_sender, g_free);
  free(pub_one); free(pub_two);
  g_free(npub_one); g_free(npub_two); g_free(ciphertext);
}

typedef struct { GMainLoop *loop; gchar *value; GError *error; gboolean sign; gboolean done; } Await;
static void
finished(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Await *wait = data;
  wait->value = wait->sign ? gh_signer_sign_finish(result, &wait->error) :
                             gh_signer_nip44_finish(result, &wait->error);
  wait->done = TRUE;
  if (wait->loop) g_main_loop_quit(wait->loop);
}

static gchar *
unsigned_event(void)
{
  return g_strdup_printf("{\"pubkey\":\"%s\",\"created_at\":1700000000,"
                         "\"kind\":1,\"tags\":[[\"p\",\"%s\"]],\"content\":\"hello\"}",
                         pub_one, pub_two);
}

static void
test_selection(void)
{
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(npub_one);
  info->label = g_strdup("shared identity");
  g_ptr_array_add(ids, info);
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_identity_select_from_list(settings, ids, npub_two, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error(&error);
  g_assert_true(gh_identity_select_from_list(settings, ids, npub_one, &error));
  g_assert_no_error(error);
  g_autofree gchar *selected = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(selected, ==, npub_one);
}

static void
test_sign(MockMode test_mode, GhSignerError expected)
{
  mode = test_mode;
  g_autoptr(GError) error = NULL;
  GhSigner *signer = gh_signer_new(client, npub_one, &error);
  g_assert_no_error(error);
  g_autofree gchar *request = unsigned_event();
  Await wait = { .loop = g_main_loop_new(NULL, FALSE), .sign = TRUE };
  gh_signer_sign_async(signer, request, NULL, finished, &wait);
  g_main_loop_run(wait.loop);
  if (test_mode == MOCK_OK) {
    g_assert_no_error(wait.error);
    g_assert_nonnull(wait.value);
  } else {
    g_assert_null(wait.value);
    g_assert_error(wait.error, GH_SIGNER_ERROR, (gint)expected);
  }
  g_free(wait.value);
  g_clear_error(&wait.error);
  g_main_loop_unref(wait.loop);
  gh_signer_free(signer);
}

static void test_sign_ok(void) { test_sign(MOCK_OK, 0); }
static void test_sign_denied(void) { test_sign(MOCK_DENIED, GH_SIGNER_ERROR_DENIED); }
static void test_sign_denied_timeout_text(void) { test_sign(MOCK_DENIED_TIMEOUT_TEXT, GH_SIGNER_ERROR_DENIED); }
static void test_sign_timeout(void) { test_sign(MOCK_TIMEOUT, GH_SIGNER_ERROR_TIMED_OUT); }
static void test_sign_approval_timeout(void) { test_sign(MOCK_APPROVAL_TIMEOUT, GH_SIGNER_ERROR_TIMED_OUT); }
static void test_sign_approval_timeout_denied_text(void) { test_sign(MOCK_TIMEOUT_DENIED_TEXT, GH_SIGNER_ERROR_TIMED_OUT); }
static void test_sign_no_agent(void) { test_sign(MOCK_NO_AGENT, GH_SIGNER_ERROR_NO_APPROVER); }
static void test_sign_identity_changed(void) { test_sign(MOCK_IDENTITY_CHANGED, GH_SIGNER_ERROR_KEY_MISMATCH); }
static void test_sign_wrong_key(void) { test_sign(MOCK_WRONG_KEY, GH_SIGNER_ERROR_KEY_MISMATCH); }
static void test_sign_changed(void) { test_sign(MOCK_CHANGED_CONTENT, GH_SIGNER_ERROR_INVALID_RESULT); }

static void
wait_for_held_calls(guint count)
{
  while (held_calls->len < count) g_main_context_iteration(NULL, TRUE);
}

static void
wait_for_held_call(void)
{
  wait_for_held_calls(1);
}

static void
release_held_call(void)
{
  g_assert_cmpuint(held_calls->len, >, 0);
  GDBusMethodInvocation *invocation = g_ptr_array_index(held_calls, 0);
  g_dbus_method_invocation_return_dbus_error(invocation,
    "org.nostr.Signer.Error.ApprovalDenied", "cancelled request");
  g_ptr_array_remove_index(held_calls, 0);
}

static void
wait_for_done(Await *wait)
{
  while (!wait->done) g_main_context_iteration(NULL, TRUE);
}

static void
test_cancel(void)
{
  mode = MOCK_HOLD;
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  g_autofree gchar *request = unsigned_event();
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  Await wait = { .loop = g_main_loop_new(NULL, FALSE), .sign = TRUE };
  gh_signer_sign_async(signer, request, cancel, finished, &wait);
  wait_for_held_call();
  g_autofree gchar *abandoned_sender = g_strdup(last_sender);
  g_cancellable_cancel(cancel);
  g_main_loop_run(wait.loop);
  g_assert_error(wait.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_assert_null(wait.value);
  g_clear_error(&wait.error);
  release_held_call();
  mode = MOCK_OK;
  gh_signer_sign_async(signer, request, NULL, finished, &wait);
  g_main_loop_run(wait.loop);
  g_assert_no_error(wait.error);
  g_assert_nonnull(wait.value);
  g_assert_cmpstr(last_sender, !=, abandoned_sender);
  g_free(wait.value);
  g_main_loop_unref(wait.loop);
  gh_signer_free(signer);
}

static void
test_switch_cancels(void)
{
  mode = MOCK_HOLD;
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  g_autofree gchar *request = unsigned_event();
  Await wait = { .loop = g_main_loop_new(NULL, FALSE), .sign = TRUE };
  gh_signer_sign_async(signer, request, NULL, finished, &wait);
  wait_for_held_call();
  g_assert_true(gh_signer_select(signer, npub_two, NULL));
  g_main_loop_run(wait.loop);
  g_assert_error(wait.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_clear_error(&wait.error);
  g_main_loop_unref(wait.loop);
  release_held_call();
  gh_signer_free(signer);
}

static void
test_concurrent_cancel(void)
{
  mode = MOCK_HOLD;
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  Await first = { 0 }, second = { 0 };
  gh_signer_nip44_decrypt_async(signer, ciphertext, pub_two, cancel, finished, &first);
  wait_for_held_call();
  gh_signer_nip44_decrypt_async(signer, ciphertext, pub_two, NULL, finished, &second);
  wait_for_held_calls(2);
  g_autofree gchar *first_sender = g_strdup(
    g_dbus_method_invocation_get_sender(g_ptr_array_index(held_calls, 0)));
  g_autofree gchar *second_sender = g_strdup(
    g_dbus_method_invocation_get_sender(g_ptr_array_index(held_calls, 1)));
  g_assert_cmpstr(first_sender, !=, second_sender);
  g_cancellable_cancel(cancel);
  wait_for_done(&first);
  g_assert_error(first.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_assert_null(first.value);
  g_assert_false(second.done);
  release_held_call();
  GDBusMethodInvocation *survivor = g_ptr_array_index(held_calls, 0);
  g_dbus_method_invocation_return_value(survivor, g_variant_new("(s)", "plaintext"));
  g_ptr_array_remove_index(held_calls, 0);
  wait_for_done(&second);
  g_assert_no_error(second.error);
  g_assert_cmpstr(second.value, ==, "plaintext");
  g_clear_error(&first.error);
  g_free(second.value);
  gh_signer_free(signer);
}

static void
test_concurrent_switch(void)
{
  mode = MOCK_HOLD;
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  Await first = { 0 }, second = { 0 }, after = { 0 };
  gh_signer_nip44_decrypt_async(signer, ciphertext, pub_two, NULL, finished, &first);
  wait_for_held_call();
  gh_signer_nip44_decrypt_async(signer, ciphertext, pub_two, NULL, finished, &second);
  wait_for_held_calls(2);
  g_autofree gchar *old_sender = g_strdup(
    g_dbus_method_invocation_get_sender(g_ptr_array_index(held_calls, 0)));
  g_assert_true(gh_signer_select(signer, npub_two, NULL));
  wait_for_done(&first);
  wait_for_done(&second);
  g_assert_error(first.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_assert_error(second.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_assert_null(first.value);
  g_assert_null(second.value);
  g_clear_error(&first.error);
  g_clear_error(&second.error);
  release_held_call();
  release_held_call();
  expected_npub = npub_two;
  mode = MOCK_OK;
  gh_signer_nip44_encrypt_async(signer, "after switch", pub_two, NULL, finished, &after);
  wait_for_done(&after);
  g_assert_no_error(after.error);
  g_assert_cmpstr(after.value, ==, ciphertext);
  g_assert_cmpstr(last_sender, !=, old_sender);
  g_free(after.value);
  gh_signer_free(signer);
  expected_npub = npub_one;
}

static void
test_switch_during_setup(void)
{
  mode = MOCK_OK;
  g_clear_pointer(&last_sender, g_free);
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  Await before = { 0 }, after = { 0 };
  gh_signer_nip44_encrypt_async(signer, "before switch", pub_two, NULL, finished, &before);
  g_assert_true(gh_signer_select(signer, npub_two, NULL));
  wait_for_done(&before);
  g_assert_error(before.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_assert_null(before.value);
  g_assert_null(last_sender);
  g_clear_error(&before.error);
  expected_npub = npub_two;
  gh_signer_nip44_encrypt_async(signer, "after switch", pub_two, NULL, finished, &after);
  wait_for_done(&after);
  g_assert_no_error(after.error);
  g_assert_cmpstr(after.value, ==, ciphertext);
  g_free(after.value);
  gh_signer_free(signer);
  expected_npub = npub_one;
}

static void
test_nip44(void)
{
  mode = MOCK_OK;
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  Await wait = { .loop = g_main_loop_new(NULL, FALSE) };
  gh_signer_nip44_encrypt_async(signer, "plaintext", pub_two, NULL, finished, &wait);
  g_main_loop_run(wait.loop);
  g_assert_no_error(wait.error);
  g_assert_cmpstr(wait.value, ==, ciphertext);
  g_autofree gchar *account_sender = g_strdup(last_sender);
  g_free(wait.value);
  wait.value = NULL;
  gh_signer_nip44_decrypt_async(signer, ciphertext, pub_two, NULL, finished, &wait);
  g_main_loop_run(wait.loop);
  g_assert_no_error(wait.error);
  g_assert_cmpstr(wait.value, ==, "plaintext");
  g_assert_cmpstr(last_sender, !=, account_sender);
  g_free(wait.value);
  g_main_loop_unref(wait.loop);
  gh_signer_free(signer);
}

static void
test_nip44_error(MockMode test_mode, GhSignerError expected)
{
  mode = test_mode;
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  Await wait = { .loop = g_main_loop_new(NULL, FALSE) };
  gh_signer_nip44_encrypt_async(signer, "plaintext", pub_two, NULL, finished, &wait);
  g_main_loop_run(wait.loop);
  g_assert_error(wait.error, GH_SIGNER_ERROR, (gint)expected);
  g_assert_null(wait.value);
  g_clear_error(&wait.error);
  g_main_loop_unref(wait.loop);
  gh_signer_free(signer);
}

static void test_nip44_denied(void) { test_nip44_error(MOCK_DENIED, GH_SIGNER_ERROR_DENIED); }
static void test_nip44_timeout(void) { test_nip44_error(MOCK_TIMEOUT, GH_SIGNER_ERROR_TIMED_OUT); }
static void test_nip44_approval_timeout(void) { test_nip44_error(MOCK_APPROVAL_TIMEOUT, GH_SIGNER_ERROR_TIMED_OUT); }
static void test_nip44_no_agent(void) { test_nip44_error(MOCK_NO_AGENT, GH_SIGNER_ERROR_NO_APPROVER); }
static void test_nip44_identity_changed(void) { test_nip44_error(MOCK_IDENTITY_CHANGED, GH_SIGNER_ERROR_KEY_MISMATCH); }

static void
test_nip44_cancel(void)
{
  mode = MOCK_HOLD;
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  Await wait = { .loop = g_main_loop_new(NULL, FALSE) };
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  gh_signer_nip44_decrypt_async(signer, ciphertext, pub_two, cancel, finished, &wait);
  wait_for_held_call();
  g_cancellable_cancel(cancel);
  g_main_loop_run(wait.loop);
  g_assert_error(wait.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_assert_null(wait.value);
  g_clear_error(&wait.error);
  g_main_loop_unref(wait.loop);
  release_held_call();
  gh_signer_free(signer);
}

static void
test_bad_nip44_result(void)
{
  mode = MOCK_BAD_CIPHER;
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  Await wait = { .loop = g_main_loop_new(NULL, FALSE) };
  gh_signer_nip44_encrypt_async(signer, "plaintext", pub_two, NULL, finished, &wait);
  g_main_loop_run(wait.loop);
  g_assert_error(wait.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT);
  g_assert_null(wait.value);
  g_clear_error(&wait.error);
  g_main_loop_unref(wait.loop);
  gh_signer_free(signer);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  setup();
  g_test_add_func("/groundhog/identity/selection", test_selection);
  g_test_add_func("/groundhog/signer/sign", test_sign_ok);
  g_test_add_func("/groundhog/signer/sign-denied", test_sign_denied);
  g_test_add_func("/groundhog/signer/sign-denied-timeout-text", test_sign_denied_timeout_text);
  g_test_add_func("/groundhog/signer/timeout", test_sign_timeout);
  g_test_add_func("/groundhog/signer/approval-timeout", test_sign_approval_timeout);
  g_test_add_func("/groundhog/signer/approval-timeout-denied-text",
                  test_sign_approval_timeout_denied_text);
  g_test_add_func("/groundhog/signer/no-approval-agent", test_sign_no_agent);
  g_test_add_func("/groundhog/signer/identity-changed", test_sign_identity_changed);
  g_test_add_func("/groundhog/signer/cancel", test_cancel);
  g_test_add_func("/groundhog/signer/switch-cancels", test_switch_cancels);
  g_test_add_func("/groundhog/signer/concurrent-cancel", test_concurrent_cancel);
  g_test_add_func("/groundhog/signer/concurrent-switch", test_concurrent_switch);
  g_test_add_func("/groundhog/signer/switch-during-setup", test_switch_during_setup);
  g_test_add_func("/groundhog/signer/key-mismatch", test_sign_wrong_key);
  g_test_add_func("/groundhog/signer/payload-mismatch", test_sign_changed);
  g_test_add_func("/groundhog/signer/nip44", test_nip44);
  g_test_add_func("/groundhog/signer/nip44-denial", test_nip44_denied);
  g_test_add_func("/groundhog/signer/nip44-timeout", test_nip44_timeout);
  g_test_add_func("/groundhog/signer/nip44-approval-timeout", test_nip44_approval_timeout);
  g_test_add_func("/groundhog/signer/nip44-no-approval-agent", test_nip44_no_agent);
  g_test_add_func("/groundhog/signer/nip44-identity-changed", test_nip44_identity_changed);
  g_test_add_func("/groundhog/signer/nip44-cancel", test_nip44_cancel);
  g_test_add_func("/groundhog/signer/nip44-invalid", test_bad_nip44_result);
  int status = g_test_run();
  teardown();
  return status;
}
