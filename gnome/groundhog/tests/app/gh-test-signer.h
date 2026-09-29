/* Shared Groundhog app-test helpers: a private test D-Bus
 * (tests/common/nostrc-test-bus.h, never GTestDBus), a mock
 * org.nostr.Signer that performs real NIP-44 and event signing for the fixed
 * test keys, and bounded main-loop waits. The app under test never holds a
 * key; only this stand-in signer does. Waits iterate the default main
 * context; their deadline is a failure bound, never a source of progress.
 * Header-only: include it once per test executable. */
#ifndef GH_TEST_SIGNER_H
#define GH_TEST_SIGNER_H

#include "gh-account-controller.h"
#include "gh-identity.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-utils.h"
#include "nostr/nip19/nip19.h"
#include "nostr/nip44/nip44.h"
#include "nostrc-test-bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Key 4 is reserved for gift-wrap ephemeral keys in crafted fixtures. */
#define GH_TEST_KEYS 5
static const gchar *const gh_test_secret[GH_TEST_KEYS] = {
  NULL,
  "0000000000000000000000000000000000000000000000000000000000000001",
  "0000000000000000000000000000000000000000000000000000000000000002",
  "0000000000000000000000000000000000000000000000000000000000000003",
  "0000000000000000000000000000000000000000000000000000000000000004",
};

static G_GNUC_UNUSED gchar *
gh_test_pub(guint key)
{
  g_assert_true(key > 0 && key < GH_TEST_KEYS);
  char *hex = nostr_key_get_public(gh_test_secret[key]);
  g_assert_nonnull(hex);
  gchar *copy = g_strdup(hex);
  free(hex);
  return copy;
}

static G_GNUC_UNUSED gchar *
gh_test_npub(guint key)
{
  g_autofree gchar *hex = gh_test_pub(key);
  guint8 bytes[32];
  g_assert_true(nostr_hex2bin(bytes, hex, sizeof bytes));
  gchar *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  return npub;
}

static G_GNUC_UNUSED gboolean
gh_test_deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static G_GNUC_UNUSED gboolean
gh_test_tick(gpointer data)
{
  (void)data;
  return G_SOURCE_CONTINUE;
}

static G_GNUC_UNUSED void
gh_test_spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(10, gh_test_deadline_hit, &expired);
  /* The tick re-checks pred when nothing else would wake the loop
   * (nostrc-qp24.8.5): a GTask drops its source object on its worker thread
   * after queueing the callback, so a ref-count or weak-pointer condition
   * can turn true with no main-context event, and a condition asked of the
   * bus synchronously (has a sender disconnected?) has none either. */
  guint tick = g_timeout_add(10, gh_test_tick, NULL);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
  if (expired)
    g_error("condition waited for at line %d did not hold within 10s", line);
  g_source_remove(timer);
}
#define gh_test_spin_until(pred, data) gh_test_spin_until_at((pred), (data), __LINE__)

static G_GNUC_UNUSED gboolean
gh_test_is_null(gpointer data)
{
  return *(gpointer *)data == NULL;
}

/* Disposes object and waits until in-flight callbacks drop their refs. */
static G_GNUC_UNUSED void
gh_test_release(gpointer object)
{
  gpointer weak = object;
  g_object_add_weak_pointer(G_OBJECT(object), &weak);
  g_object_run_dispose(G_OBJECT(object));
  g_object_unref(object);
  gh_test_spin_until(gh_test_is_null, &weak);
}

typedef struct {
  NostrcTestBus *bus;
  GDBusConnection *client; /* owned by bus: never close or unref */
  GDBusConnection *owner;  /* owned by bus: never close or unref */
} GhTestBus;

static G_GNUC_UNUSED void
gh_test_bus_up(GhTestBus *fixture)
{
  fixture->bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(fixture->bus);
  fixture->client = nostrc_test_bus_connect(fixture->bus);
  fixture->owner = nostrc_test_bus_connect(fixture->bus);
}

/* For a GUI test, after gtk_init_check() and adw_init(): GTK and libadwaita
 * keep the session bus they were given (the accessibility bus and, on Linux,
 * libadwaita's settings portal live there), so the private bus for the mock
 * signer is not the session bus. GhSigner opens its private senders to the
 * address DBUS_SESSION_BUS_ADDRESS names at each call, so that is pointed at
 * it. (gh_test_bus_up() after GTK fails on Linux: the session connection
 * libadwaita made is not the test bus's; nostrc-qp24.88.) */
static G_GNUC_UNUSED void
gh_test_bus_up_beside_gtk(GhTestBus *fixture)
{
  fixture->bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NOT_SESSION);
  nostrc_test_bus_up(fixture->bus);
  fixture->client = nostrc_test_bus_connect(fixture->bus);
  fixture->owner = nostrc_test_bus_connect(fixture->bus);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", nostrc_test_bus_get_address(fixture->bus), TRUE);
}

/* Stops the bus; the connections only see it vanish (see
 * nostrc_test_bus_down()). */
static G_GNUC_UNUSED void
gh_test_bus_down(GhTestBus *fixture)
{
  nostrc_test_bus_down(fixture->bus);
  *fixture = (GhTestBus){ 0 };
}

/* Answers with the secret of the npub the request names. hold parks every
 * call (an approval the user has not answered yet) until released. */
typedef struct {
  GDBusNodeInfo *node;
  guint registration;
  GPtrArray *held;    /* GDBusMethodInvocation, oldest first */
  GPtrArray *senders; /* unique name of every caller */
  GPtrArray *methods; /* "Method from :sender" of every counted call, in order */
  gchar *npubs[GH_TEST_KEYS];
  guint calls;
  guint max_held;
  gboolean hold;
  gboolean deny;      /* answer every call with ApprovalDenied */
} GhTestSigner;

static G_GNUC_UNUSED const gchar *
gh_test_signer_secret(GhTestSigner *mock, const gchar *npub)
{
  for (guint key = 1; key < GH_TEST_KEYS; key++)
    if (g_strcmp0(mock->npubs[key], npub) == 0)
      return gh_test_secret[key];
  return NULL;
}

static G_GNUC_UNUSED void
gh_test_signer_answer(GhTestSigner *mock, GDBusMethodInvocation *invocation)
{
  const gchar *method = g_dbus_method_invocation_get_method_name(invocation);
  const gchar *input, *peer, *npub;
  g_variant_get(g_dbus_method_invocation_get_parameters(invocation), "(&s&s&s)",
                &input, &peer, &npub);
  /* SignEvent(event, npub, app) names the account second. */
  const gchar *secret = gh_test_signer_secret(mock, g_str_equal(method, "SignEvent") ? peer : npub);
  if (mock->deny || !secret) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.ApprovalDenied", "test denial");
    return;
  }
  if (g_str_equal(method, "SignEvent")) {
    NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_compact(event, input, NULL), ==, 1);
    g_assert_cmpint(nostr_event_sign(event, secret), ==, 0);
    char *json = nostr_event_serialize_compact(event);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", json));
    free(json);
    nostr_event_free(event);
    return;
  }
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, secret, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, peer, sizeof pk));
  if (g_str_equal(method, "NIP44Encrypt")) {
    char *ciphertext = NULL;
    g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)input, strlen(input),
                                            &ciphertext), ==, 0);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", ciphertext));
    free(ciphertext);
    return;
  }
  guint8 *plaintext = NULL;
  size_t length = 0;
  if (nostr_nip44_decrypt_v2(sk, pk, input, &plaintext, &length) != 0) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.Failed", "test decrypt failure");
    return;
  }
  g_autofree gchar *text = g_strndup((const gchar *)plaintext, length);
  free(plaintext);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", text));
}

static G_GNUC_UNUSED void
gh_test_signer_call(GDBusConnection *connection, const gchar *sender, const gchar *path,
                    const gchar *interface, const gchar *method, GVariant *parameters,
                    GDBusMethodInvocation *invocation, gpointer user_data)
{
  GhTestSigner *mock = user_data;
  (void)connection; (void)path; (void)interface; (void)parameters;
  if (g_str_equal(method, "EnableTypedApprovalErrors")) {
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }
  mock->calls++;
  g_ptr_array_add(mock->senders, g_strdup(sender));
  g_ptr_array_add(mock->methods, g_strdup_printf("%s from %s", method, sender));
  if (mock->hold) {
    g_ptr_array_add(mock->held, g_object_ref(invocation));
    mock->max_held = MAX(mock->max_held, mock->held->len);
    return;
  }
  gh_test_signer_answer(mock, invocation);
}

static const GDBusInterfaceVTable gh_test_signer_vtable = {
  gh_test_signer_call, NULL, NULL, { 0 }
};

static G_GNUC_UNUSED void
gh_test_signer_up(GhTestBus *fixture, GhTestSigner *mock)
{
  g_autoptr(GError) error = NULL;
  mock->held = g_ptr_array_new_with_free_func(g_object_unref);
  mock->senders = g_ptr_array_new_with_free_func(g_free);
  mock->methods = g_ptr_array_new_with_free_func(g_free);
  for (guint key = 1; key < GH_TEST_KEYS; key++)
    mock->npubs[key] = gh_test_npub(key);
  mock->node = g_dbus_node_info_new_for_xml(
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
  mock->registration = g_dbus_connection_register_object(fixture->owner,
    "/org/nostr/signer", mock->node->interfaces[0], &gh_test_signer_vtable, mock, NULL,
    &error);
  g_assert_no_error(error);
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(fixture->owner,
    "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "RequestName", g_variant_new("(su)", "org.nostr.Signer", 4u),
    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
  g_assert_no_error(error);
}

/* Fails, naming them, unless no call was counted since the count since. */
static G_GNUC_UNUSED void
gh_test_signer_assert_no_calls_since_at(GhTestSigner *mock, guint since, int line)
{
  if (mock->calls == since)
    return;
  g_autoptr(GString) calls = g_string_new(NULL);
  for (guint i = since; i < mock->methods->len; i++)
    g_string_append_printf(calls, "%s%s", i > since ? "; " : "", (gchar *)mock->methods->pdata[i]);
  g_error("line %d: %u signer call(s) where none was expected: %s", line, mock->calls - since,
          calls->str);
}
#define gh_test_signer_assert_no_calls_since(mock, since) \
  gh_test_signer_assert_no_calls_since_at((mock), (since), __LINE__)

/* Answers the oldest parked call as if the user approved it now. */
static G_GNUC_UNUSED void
gh_test_signer_release_one(GhTestSigner *mock)
{
  g_assert_cmpuint(mock->held->len, >, 0);
  GDBusMethodInvocation *invocation = g_object_ref(g_ptr_array_index(mock->held, 0));
  g_ptr_array_remove_index(mock->held, 0);
  gh_test_signer_answer(mock, invocation);
  g_object_unref(invocation);
}

static G_GNUC_UNUSED void
gh_test_signer_release_all(GhTestSigner *mock)
{
  while (mock->held->len)
    gh_test_signer_release_one(mock);
}

typedef struct {
  GhTestBus *bus;
  GhTestSigner *mock;
} GhTestSenders;

/* Every private sender connection that called the signer has closed, which
 * is how GhSigner revokes a pending approval. */
static G_GNUC_UNUSED gboolean
gh_test_signer_senders_closed(gpointer data)
{
  GhTestSenders *check = data;
  for (guint i = 0; i < check->mock->senders->len; i++) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(check->bus->client,
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "NameHasOwner", g_variant_new("(s)", g_ptr_array_index(check->mock->senders, i)),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_assert_no_error(error);
    gboolean has_owner;
    g_variant_get(reply, "(b)", &has_owner);
    if (has_owner)
      return FALSE;
  }
  return TRUE;
}

static G_GNUC_UNUSED void
gh_test_signer_down(GhTestBus *fixture, GhTestSigner *mock)
{
  mock->deny = TRUE;
  gh_test_signer_release_all(mock);
  g_dbus_connection_unregister_object(fixture->owner, mock->registration);
  g_ptr_array_unref(mock->held);
  g_ptr_array_unref(mock->senders);
  g_ptr_array_unref(mock->methods);
  for (guint key = 1; key < GH_TEST_KEYS; key++)
    g_free(mock->npubs[key]);
  g_dbus_node_info_unref(mock->node);
}

#endif
