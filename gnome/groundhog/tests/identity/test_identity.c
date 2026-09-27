#include "gh-identity.h"
#include "gh-signer.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr/nip19/nip19.h"

#include <string.h>

static const gchar *key_one =
  "0000000000000000000000000000000000000000000000000000000000000001";
static const gchar *key_two =
  "0000000000000000000000000000000000000000000000000000000000000002";
static gchar *pub_one, *pub_two, *npub_one, *npub_two, *ciphertext;
static GTestDBus *test_bus;
static GDBusConnection *server, *client;
static GDBusNodeInfo *node;
static guint registration;
static GDBusMethodInvocation *held;

typedef enum { MOCK_OK, MOCK_DENIED, MOCK_TIMEOUT, MOCK_DENIED_TIMEOUT_TEXT,
               MOCK_HOLD, MOCK_WRONG_KEY, MOCK_CHANGED_CONTENT,
               MOCK_BAD_CIPHER } MockMode;
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
  (void)connection; (void)sender; (void)path; (void)interface; (void)user_data;
  const gchar *a, *b, *c;
  g_variant_get(parameters, "(&s&s&s)", &a, &b, &c);
  if (g_str_equal(method, "SignEvent")) {
    g_assert_cmpstr(b, ==, npub_one);
    g_assert_cmpstr(c, ==, "");
  } else {
    g_assert_true(g_str_equal(method, "NIP44Encrypt") ||
                  g_str_equal(method, "NIP44Decrypt"));
    g_assert_cmpstr(b, ==, pub_two);
    g_assert_cmpstr(c, ==, npub_one);
  }
  if (mode == MOCK_DENIED || mode == MOCK_DENIED_TIMEOUT_TEXT) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.ApprovalDenied",
      mode == MOCK_DENIED_TIMEOUT_TEXT ? "user timed out, approval denied" : "approval denied");
    return;
  }
  if (mode == MOCK_TIMEOUT) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.freedesktop.DBus.Error.NoReply", "transport deadline elapsed");
    return;
  }
  if (mode == MOCK_HOLD) {
    g_assert_null(held);
    held = g_object_ref(invocation);
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
  test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(test_bus);
  GError *error = NULL;
  server = g_dbus_connection_new_for_address_sync(
    g_test_dbus_get_bus_address(test_bus),
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
  g_assert_no_error(error);
  node = g_dbus_node_info_new_for_xml(
    "<node><interface name='org.nostr.Signer'>"
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
  client = g_dbus_connection_new_for_address_sync(
    g_test_dbus_get_bus_address(test_bus),
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
  g_assert_no_error(error);
}

static void
teardown(void)
{
  if (held) {
    g_dbus_method_invocation_return_dbus_error(held, "org.nostr.Signer.Error.ApprovalDenied",
                                                "test cleanup");
    g_clear_object(&held);
  }
  g_dbus_connection_unregister_object(server, registration);
  g_clear_object(&client);
  g_clear_object(&server);
  g_clear_pointer(&node, g_dbus_node_info_unref);
  g_test_dbus_down(test_bus);
  g_clear_object(&test_bus);
  free(pub_one); free(pub_two);
  g_free(npub_one); g_free(npub_two); g_free(ciphertext);
}

typedef struct { GMainLoop *loop; gchar *value; GError *error; gboolean sign; } Await;
static void
finished(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Await *wait = data;
  wait->value = wait->sign ? gh_signer_sign_finish(result, &wait->error) :
                             gh_signer_nip44_finish(result, &wait->error);
  g_main_loop_quit(wait->loop);
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
static void test_sign_wrong_key(void) { test_sign(MOCK_WRONG_KEY, GH_SIGNER_ERROR_KEY_MISMATCH); }
static void test_sign_changed(void) { test_sign(MOCK_CHANGED_CONTENT, GH_SIGNER_ERROR_INVALID_RESULT); }

static void
wait_for_held_call(void)
{
  /* Drive the private bus until the mock has the request, then cancel it. */
  while (!held) g_main_context_iteration(NULL, TRUE);
}

static void
release_held_call(void)
{
  g_assert_nonnull(held);
  g_dbus_method_invocation_return_dbus_error(held,
    "org.nostr.Signer.Error.ApprovalDenied", "cancelled request");
  g_clear_object(&held);
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
test_nip44(void)
{
  mode = MOCK_OK;
  GhSigner *signer = gh_signer_new(client, npub_one, NULL);
  Await wait = { .loop = g_main_loop_new(NULL, FALSE) };
  gh_signer_nip44_encrypt_async(signer, "plaintext", pub_two, NULL, finished, &wait);
  g_main_loop_run(wait.loop);
  g_assert_no_error(wait.error);
  g_assert_cmpstr(wait.value, ==, ciphertext);
  g_free(wait.value);
  wait.value = NULL;
  gh_signer_nip44_decrypt_async(signer, ciphertext, pub_two, NULL, finished, &wait);
  g_main_loop_run(wait.loop);
  g_assert_no_error(wait.error);
  g_assert_cmpstr(wait.value, ==, "plaintext");
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
  g_test_add_func("/groundhog/signer/cancel", test_cancel);
  g_test_add_func("/groundhog/signer/switch-cancels", test_switch_cancels);
  g_test_add_func("/groundhog/signer/key-mismatch", test_sign_wrong_key);
  g_test_add_func("/groundhog/signer/payload-mismatch", test_sign_changed);
  g_test_add_func("/groundhog/signer/nip44", test_nip44);
  g_test_add_func("/groundhog/signer/nip44-denial", test_nip44_denied);
  g_test_add_func("/groundhog/signer/nip44-timeout", test_nip44_timeout);
  g_test_add_func("/groundhog/signer/nip44-cancel", test_nip44_cancel);
  g_test_add_func("/groundhog/signer/nip44-invalid", test_bad_nip44_result);
  int status = g_test_run();
  teardown();
  return status;
}
