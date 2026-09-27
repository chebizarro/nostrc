#include "gh-account-controller.h"
#include "gh-identity.h"
#include "gh-signer.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr/nip19/nip19.h"
#ifdef GROUNDHOG_TEST_RELAY
#include "gh-relay-scope.h"
#endif

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

static gchar *npub_one, *npub_two, *npub_three;

typedef struct {
  GMutex lock;
  GCond cond;
  gboolean fail;
  gboolean empty;
  gboolean hold_next; /* the next listing blocks until released */
  gboolean released;
  gboolean holding;
} FakeStore;

static gchar *
npub_for_secret(const gchar *secret)
{
  g_autofree gchar *hex = nostr_key_get_public(secret);
  guint8 bytes[32];
  g_assert_nonnull(hex);
  for (guint i = 0; i < 32; i++) {
    unsigned int value;
    g_assert_cmpint(sscanf(hex + 2 * i, "%2x", &value), ==, 1);
    bytes[i] = value;
  }
  gchar *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  return npub;
}

static GhIdentityInfo *
identity(const gchar *npub, const gchar *label)
{
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(npub);
  info->label = g_strdup(label);
  return info;
}

/* Worker-thread listing; the configuration is captured on entry. */
static GPtrArray *
fake_list(gpointer data, GError **error)
{
  FakeStore *store = data;
  g_mutex_lock(&store->lock);
  gboolean fail = store->fail, empty = store->empty, hold = store->hold_next;
  store->hold_next = FALSE;
  if (hold) {
    store->holding = TRUE;
    g_cond_broadcast(&store->cond);
    g_main_context_wakeup(NULL); /* the test's main loop polls holding */
    while (!store->released)
      g_cond_wait(&store->cond, &store->lock);
    store->holding = FALSE;
  }
  g_mutex_unlock(&store->lock);
  if (fail) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "keyring locked");
    return NULL;
  }
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  if (!empty) {
    g_ptr_array_add(ids, identity(npub_one, "One"));
    g_ptr_array_add(ids, identity(npub_two, ""));
  }
  return ids;
}

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

/* Iterates the main context (no sleeps) until pred holds, failing after 5s. */
static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline_hit, &expired);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("condition waited for at line %d did not hold within 5s", line);
  g_source_remove(timer);
}
#define spin_until(pred, data) spin_until_at((pred), (data), __LINE__)

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static gboolean
availability_known(gpointer data)
{
  return gh_account_controller_get_signer_availability(data) !=
         GH_SIGNER_AVAILABILITY_UNKNOWN;
}

static gboolean
is_null(gpointer data)
{
  return *(gpointer *)data == NULL;
}

static GSettings *
fresh_settings(const gchar *current)
{
  GSettings *settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", current);
  g_settings_set_string(settings, "signer-method", "auto");
  return settings;
}

/* Waits for in-flight D-Bus or listing callbacks to drop their references. */
static void
release_controller(GhAccountController *controller)
{
  gpointer weak = controller;
  g_object_add_weak_pointer(G_OBJECT(controller), &weak);
  g_object_unref(controller);
  spin_until(is_null, &weak);
}

static void
test_discover_and_select(void)
{
  FakeStore store = { 0 };
  g_autoptr(GSettings) settings = fresh_settings("");
  g_autoptr(GSettings) gnostr = g_settings_new("org.gnostr.Client");
  g_settings_set_string(gnostr, "current-npub", npub_two);
  GhAccountController *controller =
    gh_account_controller_new_full(settings, NULL, fake_list, &store);
  g_autoptr(GError) error = NULL;

  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_DISCOVERING);
  g_assert_cmpint(gh_account_controller_get_signer_availability(controller), ==,
                  GH_SIGNER_AVAILABILITY_NO_BUS);
  spin_until(listed, controller);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_UNSELECTED);
  g_assert_cmpuint(gh_account_controller_get_identities(controller)->len, ==, 2);
  g_assert_null(gh_account_controller_get_active_npub(controller));

  guint64 before = gh_account_controller_get_generation(controller);
  g_autoptr(GCancellable) old = g_object_ref(gh_account_controller_get_cancellable(controller));
  g_assert_true(gh_account_controller_select(controller, npub_one, &error));
  g_assert_no_error(error);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==, GH_ACCOUNT_STATE_ACTIVE);
  g_assert_cmpstr(gh_account_controller_get_active_npub(controller), ==, npub_one);
  g_autofree gchar *saved = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(saved, ==, npub_one);
  g_assert_cmpuint(gh_account_controller_get_generation(controller), >, before);
  g_assert_false(gh_account_controller_is_current(controller, before));
  g_assert_true(g_cancellable_is_cancelled(old));
  g_assert_false(g_cancellable_is_cancelled(gh_account_controller_get_cancellable(controller)));

  /* Gnostr's own active identity is never touched by a Groundhog switch. */
  g_autofree gchar *gnostr_current = g_settings_get_string(gnostr, "current-npub");
  g_assert_cmpstr(gnostr_current, ==, npub_two);

  /* An identity outside the signer-owned listing is refused without effect. */
  guint64 active_generation = gh_account_controller_get_generation(controller);
  g_assert_false(gh_account_controller_select(controller, npub_three, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error(&error);
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==, active_generation);
  g_assert_cmpstr(gh_account_controller_get_active_npub(controller), ==, npub_one);

  /* Explicit read-only mode revokes the account. */
  g_assert_true(gh_account_controller_select(controller, "", &error));
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_UNSELECTED);
  g_assert_null(gh_account_controller_get_active_npub(controller));
  g_assert_false(gh_account_controller_is_current(controller, active_generation));

  /* Teardown revokes the final generation. */
  g_autoptr(GCancellable) last = g_object_ref(gh_account_controller_get_cancellable(controller));
  release_controller(controller);
  g_assert_true(g_cancellable_is_cancelled(last));
  g_settings_set_string(gnostr, "current-npub", "");
}

typedef struct {
  GhAccountController *controller;
  guint64 generation;
  gboolean fired;
  gboolean current_when_fired;
} StaleProbe;

static void
probe_cancelled(GCancellable *cancellable, gpointer data)
{
  StaleProbe *probe = data;
  (void)cancellable;
  probe->fired = TRUE;
  probe->current_when_fired = gh_account_controller_is_current(probe->controller,
                                                               probe->generation);
}

#ifdef GROUNDHOG_TEST_RELAY
typedef struct {
  guint opened;
  guint closed;
  guint updates;
} RelayFixture;

static gpointer
relay_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
           gpointer data, GError **error)
{
  RelayFixture *fixture = data;
  (void)scope; (void)url; (void)filters; (void)error;
  fixture->opened++;
  return GUINT_TO_POINTER(fixture->opened);
}

static void
relay_close(gpointer handle, gpointer data)
{
  RelayFixture *fixture = data;
  (void)handle;
  fixture->closed++;
}

static const GhRelayTransport relay_transport = { relay_open, relay_close };

static void
relay_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  RelayFixture *fixture = data;
  (void)scope; (void)update;
  fixture->updates++;
}

static void
cancel_scope(GCancellable *cancellable, gpointer scope)
{
  (void)cancellable;
  gh_relay_scope_cancel(scope);
}
#endif

static void
test_switch_invalidates(void)
{
  FakeStore store = { 0 };
  g_autoptr(GSettings) settings = fresh_settings("");
  g_settings_set_string(settings, "current-npub", npub_one);
  GhAccountController *controller =
    gh_account_controller_new_full(settings, NULL, fake_list, &store);
  spin_until(listed, controller);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==, GH_ACCOUNT_STATE_ACTIVE);
  g_assert_cmpstr(gh_account_controller_get_active_npub(controller), ==, npub_one);

  StaleProbe probe = { controller, gh_account_controller_get_generation(controller),
                       FALSE, TRUE };
  GCancellable *cancellable = gh_account_controller_get_cancellable(controller);
  gulong handler = g_cancellable_connect(cancellable, G_CALLBACK(probe_cancelled),
                                         &probe, NULL);
  g_autoptr(GCancellable) held = g_object_ref(cancellable);

#ifdef GROUNDHOG_TEST_RELAY
  RelayFixture relay = { 0 };
  GhRelayScope *scope = gh_relay_scope_new_with_transport(
    probe.generation, nostr_filters_new(), &relay_transport, &relay, relay_update, &relay);
  g_assert_true(gh_relay_scope_add_url(scope, "wss://relay.test.invalid", NULL));
  gh_relay_scope_start(scope);
  g_assert_cmpuint(relay.opened, ==, 1);
  gulong relay_handler = g_cancellable_connect(cancellable, G_CALLBACK(cancel_scope),
                                               gh_relay_scope_ref(scope),
                                               (GDestroyNotify)gh_relay_scope_unref);
  gh_relay_scope_eose(scope, "wss://relay.test.invalid");
  g_assert_cmpuint(relay.updates, ==, 1);
#endif

  /* An out-of-band write (e.g. gsettings CLI) switches like the menu does. */
  g_settings_set_string(settings, "current-npub", npub_two);
  g_assert_cmpstr(gh_account_controller_get_active_npub(controller), ==, npub_two);
  g_assert_true(probe.fired);
  g_assert_false(probe.current_when_fired); /* revoked before cancellation */
  g_assert_true(g_cancellable_is_cancelled(held));

#ifdef GROUNDHOG_TEST_RELAY
  /* The old account's live REQ is closed and late relay traffic dropped. */
  g_assert_cmpuint(relay.closed, ==, 1);
  gh_relay_scope_notice(scope, "wss://relay.test.invalid", GH_RELAY_NOTICE_AUTH,
                        NULL, FALSE, "challenge");
  g_assert_cmpuint(relay.updates, ==, 1);
  g_cancellable_disconnect(held, relay_handler);
  gh_relay_scope_unref(scope);
#endif
  g_cancellable_disconnect(held, handler);

  /* A selection that disappears from the store revokes the account. */
  guint64 second = gh_account_controller_get_generation(controller);
  g_settings_set_string(settings, "current-npub", npub_three);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_SELECTED_MISSING);
  g_assert_null(gh_account_controller_get_active_npub(controller));
  g_assert_false(gh_account_controller_is_current(controller, second));
  release_controller(controller);
}

static void
refresh_and_wait(GhAccountController *controller)
{
  gpointer marker = GUINT_TO_POINTER(1);
  gulong handler = g_signal_connect_swapped(controller, "changed",
                                            G_CALLBACK(g_nullify_pointer), &marker);
  gh_account_controller_refresh(controller);
  spin_until(is_null, &marker);
  g_signal_handler_disconnect(controller, handler);
}

static void
test_store_states(void)
{
  FakeStore store = { .fail = TRUE };
  g_autoptr(GSettings) settings = fresh_settings("");
  GhAccountController *controller =
    gh_account_controller_new_full(settings, NULL, fake_list, &store);
  g_autoptr(GError) error = NULL;
  spin_until(listed, controller);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_STORE_UNAVAILABLE);
  g_assert_null(gh_account_controller_get_identities(controller));
  g_assert_false(gh_account_controller_select(controller, npub_one, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_clear_error(&error);
  g_assert_true(gh_account_controller_select(controller, "", &error));

  store.fail = FALSE;
  store.empty = TRUE;
  refresh_and_wait(controller);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_NO_IDENTITIES);
  store.empty = FALSE;
  refresh_and_wait(controller);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_UNSELECTED);
  release_controller(controller);
}

static gboolean
store_holding(gpointer data)
{
  FakeStore *store = data;
  g_mutex_lock(&store->lock);
  gboolean holding = store->holding;
  g_mutex_unlock(&store->lock);
  return holding;
}

static gboolean
listing_settled(gpointer data)
{
  return G_OBJECT(data)->ref_count == 1;
}

static void
test_stale_listing(void)
{
  FakeStore store = { .hold_next = TRUE };
  g_autoptr(GSettings) settings = fresh_settings("");
  GhAccountController *controller =
    gh_account_controller_new_full(settings, NULL, fake_list, &store);
  spin_until(store_holding, &store); /* first listing (two identities) held */

  g_mutex_lock(&store.lock);
  store.empty = TRUE;
  g_mutex_unlock(&store.lock);
  refresh_and_wait(controller); /* newer listing: empty */
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_NO_IDENTITIES);

  g_mutex_lock(&store.lock);
  store.released = TRUE;
  g_cond_broadcast(&store.cond);
  g_mutex_unlock(&store.lock);
  /* A GTask keeps its source alive until its callback has run. */
  spin_until(listing_settled, controller);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_NO_IDENTITIES);
  g_assert_cmpuint(gh_account_controller_get_identities(controller)->len, ==, 0);
  release_controller(controller);
}

/* The app's shutdown path: dispose while a listing is still in flight. */
static void
test_dispose_in_flight(void)
{
  FakeStore store = { .hold_next = TRUE };
  g_autoptr(GSettings) settings = fresh_settings("");
  g_settings_set_string(settings, "current-npub", npub_one);
  GhAccountController *controller =
    gh_account_controller_new_full(settings, NULL, fake_list, &store);
  g_autoptr(GError) error = NULL;
  spin_until(store_holding, &store);
  guint64 generation = gh_account_controller_get_generation(controller);
  g_autoptr(GCancellable) cancellable =
    g_object_ref(gh_account_controller_get_cancellable(controller));

  g_object_run_dispose(G_OBJECT(controller));
  g_assert_true(g_cancellable_is_cancelled(cancellable));
  g_assert_false(gh_account_controller_is_current(controller, generation));
  g_assert_null(gh_account_controller_get_cancellable(controller));
  g_assert_false(gh_account_controller_select(controller, npub_one, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED);
  gh_account_controller_refresh(controller); /* no-op once disposed */

  g_mutex_lock(&store.lock);
  store.released = TRUE;
  g_cond_broadcast(&store.cond);
  g_mutex_unlock(&store.lock);
  spin_until(listing_settled, controller);
  /* The late listing is discarded: nothing becomes active after dispose. */
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_DISCOVERING);
  g_assert_null(gh_account_controller_get_active_npub(controller));
  g_assert_null(gh_account_controller_get_identities(controller));
  release_controller(controller);
}

typedef struct {
  GTestDBus *bus;
  GDBusConnection *client;
  GDBusConnection *owner;
  gchar *service_dir;
} BusFixture;

static void
bus_up(BusFixture *fixture, gboolean activatable)
{
  GError *error = NULL;
  fixture->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  if (activatable) {
    fixture->service_dir = g_dir_make_tmp("groundhog-services-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree gchar *path = g_build_filename(fixture->service_dir,
                                              "org.nostr.Signer.service", NULL);
    g_assert_true(g_file_set_contents(path,
      "[D-BUS Service]\nName=org.nostr.Signer\nExec=/usr/bin/false\n", -1, &error));
    g_test_dbus_add_service_dir(fixture->bus, fixture->service_dir);
  }
  g_test_dbus_up(fixture->bus);
  const gchar *address = g_test_dbus_get_bus_address(fixture->bus);
  GDBusConnectionFlags flags = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION;
  fixture->client = g_dbus_connection_new_for_address_sync(address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
  fixture->owner = g_dbus_connection_new_for_address_sync(address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
}

static void
bus_down(BusFixture *fixture)
{
  g_dbus_connection_close_sync(fixture->owner, NULL, NULL);
  g_dbus_connection_close_sync(fixture->client, NULL, NULL);
  g_clear_object(&fixture->owner);
  g_clear_object(&fixture->client);
  g_test_dbus_down(fixture->bus);
  g_clear_object(&fixture->bus);
  if (fixture->service_dir) {
    g_autofree gchar *path = g_build_filename(fixture->service_dir,
                                              "org.nostr.Signer.service", NULL);
    g_unlink(path);
    g_rmdir(fixture->service_dir);
    g_clear_pointer(&fixture->service_dir, g_free);
  }
}

typedef struct {
  GhAccountController *controller;
  GhSignerAvailability want;
} AvailabilityWait;

static gboolean
availability_is(gpointer data)
{
  AvailabilityWait *wait = data;
  return gh_account_controller_get_signer_availability(wait->controller) == wait->want;
}

static void
test_signer_availability(void)
{
  BusFixture fixture = { 0 };
  FakeStore store = { 0 };
  bus_up(&fixture, FALSE);
  g_autoptr(GSettings) settings = fresh_settings("");
  g_settings_set_string(settings, "current-npub", npub_one);
  GhAccountController *controller =
    gh_account_controller_new_full(settings, fixture.client, fake_list, &store);
  spin_until(listed, controller);
  spin_until(availability_known, controller);
  g_assert_cmpint(gh_account_controller_get_signer_availability(controller), ==,
                  GH_SIGNER_AVAILABILITY_ABSENT);
  guint64 generation = gh_account_controller_get_generation(controller);

  guint owner = g_bus_own_name_on_connection(fixture.owner, "org.nostr.Signer",
                                             G_BUS_NAME_OWNER_FLAGS_NONE,
                                             NULL, NULL, NULL, NULL);
  AvailabilityWait wait = { controller, GH_SIGNER_AVAILABILITY_RUNNING };
  spin_until(availability_is, &wait);
  g_bus_unown_name(owner);
  wait.want = GH_SIGNER_AVAILABILITY_ABSENT;
  spin_until(availability_is, &wait);
  /* Signer reachability never changes the account generation. */
  g_assert_true(gh_account_controller_is_current(controller, generation));
  release_controller(controller);
  bus_down(&fixture);
}

static void
test_signer_activatable(void)
{
  BusFixture fixture = { 0 };
  FakeStore store = { 0 };
  bus_up(&fixture, TRUE);
  g_autoptr(GSettings) settings = fresh_settings("");
  GhAccountController *controller =
    gh_account_controller_new_full(settings, fixture.client, fake_list, &store);
  spin_until(availability_known, controller);
  g_assert_cmpint(gh_account_controller_get_signer_availability(controller), ==,
                  GH_SIGNER_AVAILABILITY_ACTIVATABLE);
  spin_until(listed, controller);
  release_controller(controller);
  bus_down(&fixture);
}

typedef struct {
  GDBusNodeInfo *node;
  guint registration;
  GPtrArray *held;
  GPtrArray *senders;
  guint calls;
  gchar *last_npub;
  gboolean hold;
} MockSigner;

static void
mock_signer_call(GDBusConnection *connection, const gchar *sender, const gchar *path,
                 const gchar *interface, const gchar *method, GVariant *parameters,
                 GDBusMethodInvocation *invocation, gpointer user_data)
{
  MockSigner *mock = user_data;
  (void)connection; (void)sender; (void)path; (void)interface;
  if (g_str_equal(method, "EnableTypedApprovalErrors")) {
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }
  const gchar *input, *peer, *npub;
  g_variant_get(parameters, "(&s&s&s)", &input, &peer, &npub);
  g_assert_true(g_str_equal(method, "SignEvent") ||
                g_str_equal(method, "NIP44Encrypt") ||
                g_str_equal(method, "NIP44Decrypt"));
  g_free(mock->last_npub);
  mock->last_npub = g_strdup(g_str_equal(method, "SignEvent") ? peer : npub);
  mock->calls++;
  g_ptr_array_add(mock->senders, g_strdup(sender));
  if (mock->hold) {
    g_ptr_array_add(mock->held, g_object_ref(invocation));
    return;
  }
  if (g_str_equal(method, "SignEvent")) {
    const gchar *secret = g_str_equal(peer, npub_one) ?
      "0000000000000000000000000000000000000000000000000000000000000001" :
      "0000000000000000000000000000000000000000000000000000000000000002";
    NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_compact(event, input, NULL), ==, 1);
    g_assert_cmpint(nostr_event_sign(event, secret), ==, 0);
    gchar *signed_json = nostr_event_serialize_compact(event);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", signed_json));
    free(signed_json);
    nostr_event_free(event);
  } else if (g_str_equal(method, "NIP44Encrypt")) {
    guint8 payload[99] = { 2 };
    g_autofree gchar *ciphertext = g_base64_encode(payload, sizeof payload);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", ciphertext));
  } else {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "plaintext"));
  }
}

static const GDBusInterfaceVTable mock_vtable = { mock_signer_call, NULL, NULL, { 0 } };

static void
mock_signer_up(BusFixture *fixture, MockSigner *mock)
{
  g_autoptr(GError) error = NULL;
  mock->held = g_ptr_array_new_with_free_func(g_object_unref);
  mock->senders = g_ptr_array_new_with_free_func(g_free);
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
    "/org/nostr/signer", mock->node->interfaces[0], &mock_vtable, mock, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(fixture->owner,
    "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "RequestName", g_variant_new("(su)", "org.nostr.Signer", 4u),
    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(reply);
}

static void
mock_signer_down(BusFixture *fixture, MockSigner *mock)
{
  while (mock->held->len) {
    GDBusMethodInvocation *invocation = g_ptr_array_index(mock->held, 0);
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.ApprovalDenied", "test cleanup");
    g_ptr_array_remove_index(mock->held, 0);
  }
  g_dbus_connection_unregister_object(fixture->owner, mock->registration);
  g_ptr_array_unref(mock->held);
  g_ptr_array_unref(mock->senders);
  g_dbus_node_info_unref(mock->node);
  g_free(mock->last_npub);
}

typedef struct {
  BusFixture *fixture;
  MockSigner *mock;
} MockSenders;

static gboolean
mock_senders_closed(gpointer data)
{
  MockSenders *check = data;
  for (guint i = 0; i < check->mock->senders->len; i++) {
    const gchar *sender = g_ptr_array_index(check->mock->senders, i);
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(check->fixture->client,
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "NameHasOwner", g_variant_new("(s)", sender), G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_assert_no_error(error);
    gboolean has_owner;
    g_variant_get(reply, "(b)", &has_owner);
    if (has_owner) return FALSE;
  }
  return TRUE;
}

typedef struct {
  gboolean done;
  gboolean sign;
  gchar *value;
  GError *error;
} SignerWait;

static void
account_signer_done(GObject *source, GAsyncResult *result, gpointer data)
{
  SignerWait *wait = data;
  (void)source;
  wait->value = wait->sign ? gh_account_controller_sign_finish(result, &wait->error) :
                             gh_account_controller_nip44_finish(result, &wait->error);
  wait->done = TRUE;
}

static gboolean
signer_done(gpointer data)
{
  return ((SignerWait *)data)->done;
}

static gboolean
mock_called(gpointer data)
{
  return ((MockSigner *)data)->calls > 0;
}

typedef struct {
  MockSigner *mock;
  guint count;
} MockCount;

static gboolean
mock_count_reached(gpointer data)
{
  MockCount *want = data;
  return want->mock->calls >= want->count;
}

static gchar *
unsigned_for(const gchar *npub)
{
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub);
  return g_strdup_printf("{\"pubkey\":\"%s\",\"created_at\":1700000000,"
                         "\"kind\":1,\"tags\":[],\"content\":\"hello\"}", pubkey);
}

static void
test_account_signer_invocation(void)
{
  BusFixture fixture = { 0 };
  MockSigner mock = { 0 };
  FakeStore store = { 0 };
  bus_up(&fixture, FALSE);
  mock_signer_up(&fixture, &mock);
  g_autoptr(GSettings) settings = fresh_settings(npub_one);
  GhAccountController *controller = gh_account_controller_new_full(
    settings, fixture.client, fake_list, &store);
  spin_until(listed, controller);
  g_autofree gchar *request = unsigned_for(npub_one);
  SignerWait sign = { .sign = TRUE };
  gh_account_controller_sign_async(controller, request, account_signer_done, &sign);
  spin_until(signer_done, &sign);
  g_assert_no_error(sign.error);
  g_assert_nonnull(sign.value);
  g_assert_cmpstr(mock.last_npub, ==, npub_one);
  g_free(sign.value);

  g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub_two);
  SignerWait encrypt = { 0 };
  gh_account_controller_nip44_encrypt_async(controller, "hello", pubkey,
                                             account_signer_done, &encrypt);
  spin_until(signer_done, &encrypt);
  g_assert_no_error(encrypt.error);
  g_assert_nonnull(encrypt.value);
  g_assert_cmpstr(mock.last_npub, ==, npub_one);
  SignerWait decrypt = { 0 };
  gh_account_controller_nip44_decrypt_async(controller, encrypt.value, pubkey,
                                             account_signer_done, &decrypt);
  spin_until(signer_done, &decrypt);
  g_assert_no_error(decrypt.error);
  g_assert_cmpstr(decrypt.value, ==, "plaintext");
  g_assert_cmpuint(mock.calls, ==, 3);
  g_free(encrypt.value);
  g_free(decrypt.value);
  release_controller(controller);
  MockSenders check = { &fixture, &mock };
  spin_until(mock_senders_closed, &check);
  mock_signer_down(&fixture, &mock);
  bus_down(&fixture);
}

static void
test_account_signer_switch_dispose(void)
{
  BusFixture fixture = { 0 };
  MockSigner mock = { .hold = TRUE };
  FakeStore store = { 0 };
  bus_up(&fixture, FALSE);
  mock_signer_up(&fixture, &mock);
  g_autoptr(GSettings) settings = fresh_settings(npub_one);
  GhAccountController *controller = gh_account_controller_new_full(
    settings, fixture.client, fake_list, &store);
  spin_until(listed, controller);
  guint64 first_generation = gh_account_controller_get_generation(controller);
  g_autofree gchar *first_request = unsigned_for(npub_one);
  SignerWait first = { .sign = TRUE };
  gh_account_controller_sign_async(controller, first_request, account_signer_done, &first);
  spin_until(mock_called, &mock);
  g_assert_cmpstr(mock.last_npub, ==, npub_one);
  g_assert_true(gh_account_controller_select(controller, npub_two, NULL));
  spin_until(signer_done, &first);
  g_assert_null(first.value);
  g_assert_error(first.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_clear_error(&first.error);
  g_assert_false(gh_account_controller_is_current(controller, first_generation));

  mock.hold = FALSE;
  g_autofree gchar *second_request = unsigned_for(npub_two);
  SignerWait second = { .sign = TRUE };
  gh_account_controller_sign_async(controller, second_request, account_signer_done, &second);
  spin_until(signer_done, &second);
  g_assert_no_error(second.error);
  g_assert_nonnull(second.value);
  g_assert_cmpstr(mock.last_npub, ==, npub_two);
  g_free(second.value);

  mock.hold = TRUE;
  SignerWait third = { .sign = TRUE };
  gh_account_controller_sign_async(controller, second_request, account_signer_done, &third);
  MockCount count = { &mock, 3 };
  spin_until(mock_count_reached, &count);
  g_settings_set_string(settings, "signer-method", "nip46");
  spin_until(signer_done, &third);
  g_assert_null(third.value);
  g_assert_error(third.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_clear_error(&third.error);
  SignerWait unsupported = { .sign = TRUE };
  gh_account_controller_sign_async(controller, second_request, account_signer_done,
                                   &unsupported);
  spin_until(signer_done, &unsupported);
  g_assert_error(unsupported.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE);
  g_clear_error(&unsupported.error);
  g_assert_cmpuint(mock.calls, ==, 3);

  g_settings_set_string(settings, "signer-method", "auto");
  SignerWait fourth = { .sign = TRUE };
  gh_account_controller_sign_async(controller, second_request, account_signer_done, &fourth);
  count.count = 4;
  spin_until(mock_count_reached, &count);
  g_object_run_dispose(G_OBJECT(controller));
  spin_until(signer_done, &fourth);
  g_assert_null(fourth.value);
  g_assert_error(fourth.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_clear_error(&fourth.error);
  release_controller(controller);
  MockSenders check = { &fixture, &mock };
  spin_until(mock_senders_closed, &check);
  mock_signer_down(&fixture, &mock);
  bus_down(&fixture);
}

static void
test_account_signer_fail_closed(void)
{
  FakeStore store = { 0 };
  g_autoptr(GSettings) settings = fresh_settings(npub_one);
  GhAccountController *controller = gh_account_controller_new_full(settings, NULL,
                                                                   fake_list, &store);
  spin_until(listed, controller);
  SignerWait wait = { .sign = TRUE };
  g_autofree gchar *request = unsigned_for(npub_one);
  gh_account_controller_sign_async(controller, request, account_signer_done, &wait);
  spin_until(signer_done, &wait);
  g_assert_null(wait.value);
  g_assert_error(wait.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE);
  g_clear_error(&wait.error);
  release_controller(controller);
}

static void
assert_limits(GhAccountState state, GhSignerAvailability availability,
              const gchar *method, gboolean online, const gchar *prefix,
              const gchar *fragment)
{
  g_autofree gchar *text = gh_account_describe_limits(state, availability, method, online);
  g_assert_true(g_str_has_prefix(text, prefix));
  g_assert_nonnull(strstr(text, fragment));
}

static void
test_limits(void)
{
  assert_limits(GH_ACCOUNT_STATE_ACTIVE, GH_SIGNER_AVAILABILITY_RUNNING, "auto", FALSE,
                "Offline:", "Nothing can be sent");
  assert_limits(GH_ACCOUNT_STATE_UNSELECTED, GH_SIGNER_AVAILABILITY_RUNNING, "auto", TRUE,
                "Read-only:", "no Groundhog account");
  assert_limits(GH_ACCOUNT_STATE_SELECTED_MISSING, GH_SIGNER_AVAILABILITY_RUNNING, "auto",
                TRUE, "Read-only:", "no longer available");
  assert_limits(GH_ACCOUNT_STATE_STORE_UNAVAILABLE, GH_SIGNER_AVAILABILITY_RUNNING, "auto",
                TRUE, "Read-only:", "account store");
  assert_limits(GH_ACCOUNT_STATE_ACTIVE, GH_SIGNER_AVAILABILITY_RUNNING, "nip46", TRUE,
                "Read-only:", "NIP-46");
  assert_limits(GH_ACCOUNT_STATE_ACTIVE, GH_SIGNER_AVAILABILITY_RUNNING, "bogus", TRUE,
                "Read-only:", "unknown signer method");
  assert_limits(GH_ACCOUNT_STATE_ACTIVE, GH_SIGNER_AVAILABILITY_ABSENT, "local", TRUE,
                "Read-only:", "not installed or running");
  assert_limits(GH_ACCOUNT_STATE_ACTIVE, GH_SIGNER_AVAILABILITY_NO_BUS, "nip55l", TRUE,
                "Read-only:", "no session bus");
  assert_limits(GH_ACCOUNT_STATE_ACTIVE, GH_SIGNER_AVAILABILITY_UNKNOWN, "auto", TRUE,
                "Read-only:", "checking");
  /* Even with a reachable signer this build has no send path. */
  assert_limits(GH_ACCOUNT_STATE_ACTIVE, GH_SIGNER_AVAILABILITY_ACTIVATABLE, "auto", TRUE,
                "Signer available", "not implemented");
  assert_limits(GH_ACCOUNT_STATE_ACTIVE, GH_SIGNER_AVAILABILITY_RUNNING, "local", TRUE,
                "Signer available", "not implemented");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  npub_one = npub_for_secret("0000000000000000000000000000000000000000000000000000000000000001");
  npub_two = npub_for_secret("0000000000000000000000000000000000000000000000000000000000000002");
  npub_three = npub_for_secret("0000000000000000000000000000000000000000000000000000000000000003");
  g_test_add_func("/groundhog/account/discover-and-select", test_discover_and_select);
  g_test_add_func("/groundhog/account/switch-invalidates", test_switch_invalidates);
  g_test_add_func("/groundhog/account/store-states", test_store_states);
  g_test_add_func("/groundhog/account/stale-listing", test_stale_listing);
  g_test_add_func("/groundhog/account/dispose-in-flight", test_dispose_in_flight);
  g_test_add_func("/groundhog/account/signer-availability", test_signer_availability);
  g_test_add_func("/groundhog/account/signer-activatable", test_signer_activatable);
  g_test_add_func("/groundhog/account/limits", test_limits);
  g_test_add_func("/groundhog/account/signer-invocation", test_account_signer_invocation);
  g_test_add_func("/groundhog/account/signer-switch-dispose", test_account_signer_switch_dispose);
  g_test_add_func("/groundhog/account/signer-fail-closed", test_account_signer_fail_closed);
  int status = g_test_run();
  g_free(npub_one);
  g_free(npub_two);
  g_free(npub_three);
  return status;
}
