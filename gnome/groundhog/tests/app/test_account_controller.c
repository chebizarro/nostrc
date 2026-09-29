#include "gh-account-controller.h"
#include "gh-identity.h"
#include "gh-signer.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr/nip19/nip19.h"
#include "nostrc-test-bus.h"
#ifdef GROUNDHOG_TEST_NIP17
#include "gh-nip17-envelope.h"
#include "gh-nip17-inbox.h"
#include "nostr/nip17/nip17.h"
#include "nostr/nip44/nip44.h"
#include "nostr/nip59/nip59.h"
#include "nostr-utils.h"
#endif
#ifdef GROUNDHOG_TEST_RELAY
#include "gh-relay-scope.h"
#endif

#include <glib/gstdio.h>
#include <sys/stat.h>
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

static gboolean
spin_tick(gpointer data)
{
  (void)data;
  return G_SOURCE_CONTINUE;
}

/* Iterates the main context (no sleeps) until pred holds, failing after 5s. */
static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline_hit, &expired);
  /* The tick re-checks pred when nothing else would wake the loop
   * (nostrc-qp24.8.5): a GTask drops its source object on its worker thread
   * after queueing the callback, so a ref-count or weak-pointer condition
   * can turn true with no main-context event, and a condition asked of the
   * bus synchronously (has a sender disconnected?) has none either. */
  guint tick = g_timeout_add(10, spin_tick, NULL);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
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

/* A private bus per test (tests/common/nostrc-test-bus.h, never GTestDBus:
 * closing GDBus connections races GDBus worker polls on macOS). */
typedef struct {
  NostrcTestBus *bus;
  GDBusConnection *client; /* owned by bus: never close or unref */
  GDBusConnection *owner;  /* owned by bus: never close or unref */
  gchar *service_dir;
} BusFixture;

static void
bus_up(BusFixture *fixture, gboolean activatable)
{
  GError *error = NULL;
  fixture->bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  if (activatable) {
    fixture->service_dir = g_dir_make_tmp("groundhog-services-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree gchar *path = g_build_filename(fixture->service_dir,
                                              "org.nostr.Signer.service", NULL);
    g_assert_true(g_file_set_contents(path,
      "[D-BUS Service]\nName=org.nostr.Signer\nExec=/usr/bin/false\n", -1, &error));
    nostrc_test_bus_add_service_dir(fixture->bus, fixture->service_dir);
  }
  nostrc_test_bus_up(fixture->bus);
  fixture->client = nostrc_test_bus_connect(fixture->bus);
  fixture->owner = nostrc_test_bus_connect(fixture->bus);
}

static void
bus_down(BusFixture *fixture)
{
  /* The connections only see the bus vanish (see nostrc_test_bus_down()). */
  nostrc_test_bus_down(fixture->bus);
  fixture->bus = NULL;
  fixture->client = fixture->owner = NULL;
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
  guint hold_call;
  guint deny_call;
  guint bad_sign_call;
  guint wrong_key_call;
  gboolean real_crypto;
  GPtrArray *encrypt_plaintexts;
  GPtrArray *encrypt_peers;
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
#ifdef GROUNDHOG_TEST_NIP17
  if (g_str_equal(method, "NIP44Encrypt") && mock->real_crypto) {
    g_ptr_array_add(mock->encrypt_plaintexts, g_strdup(input));
    g_ptr_array_add(mock->encrypt_peers, g_strdup(peer));
  }
#endif
  if (mock->hold || mock->hold_call == mock->calls) {
    g_ptr_array_add(mock->held, g_object_ref(invocation));
    return;
  }
  if (mock->deny_call == mock->calls) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Signer.Error.ApprovalDenied", "test denial");
    return;
  }
  if (mock->bad_sign_call == mock->calls) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "{}"));
    return;
  }
  if (g_str_equal(method, "SignEvent")) {
    const gchar *secret = g_str_equal(peer, npub_one) && mock->wrong_key_call != mock->calls ?
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
#ifdef GROUNDHOG_TEST_NIP17
    if (mock->real_crypto) {
      const gchar *secret = g_str_equal(npub, npub_one) ?
        "0000000000000000000000000000000000000000000000000000000000000001" :
        "0000000000000000000000000000000000000000000000000000000000000002";
      guint8 sk[32], pk[32];
      g_assert_true(nostr_hex2bin(sk, secret, sizeof sk));
      g_assert_true(nostr_hex2bin(pk, peer, sizeof pk));
      char *ciphertext = NULL;
      g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)input,
                                              strlen(input), &ciphertext), ==, 0);
      g_assert_nonnull(ciphertext);
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", ciphertext));
      free(ciphertext);
      return;
    }
#endif
    guint8 payload[99] = { 2 };
    g_autofree gchar *ciphertext = g_base64_encode(payload, sizeof payload);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", ciphertext));
  } else {
#ifdef GROUNDHOG_TEST_NIP17
    if (mock->real_crypto) {
      const gchar *secret = g_str_equal(npub, npub_one) ?
        "0000000000000000000000000000000000000000000000000000000000000001" :
        "0000000000000000000000000000000000000000000000000000000000000002";
      guint8 sk[32], pk[32];
      g_assert_true(nostr_hex2bin(sk, secret, sizeof sk));
      g_assert_true(nostr_hex2bin(pk, peer, sizeof pk));
      guint8 *plaintext = NULL;
      size_t plaintext_len = 0;
      if (nostr_nip44_decrypt_v2(sk, pk, input, &plaintext, &plaintext_len) != 0) {
        g_dbus_method_invocation_return_dbus_error(invocation,
          "org.nostr.Signer.Error.Failed", "test decrypt failure");
        return;
      }
      g_autofree gchar *text = g_strndup((const gchar *)plaintext, plaintext_len);
      free(plaintext);
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", text));
      return;
    }
#endif
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "plaintext"));
  }
}

static const GDBusInterfaceVTable mock_vtable = { mock_signer_call, NULL, NULL, { 0 } };

static void
mock_signer_up(BusFixture *fixture, MockSigner *mock)
{
  g_autoptr(GError) error = NULL;
  mock->held = g_ptr_array_new_with_free_func(g_object_unref);
  mock->encrypt_plaintexts = g_ptr_array_new_with_free_func(g_free);
  mock->encrypt_peers = g_ptr_array_new_with_free_func(g_free);
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
  g_ptr_array_unref(mock->encrypt_plaintexts);
  g_ptr_array_unref(mock->encrypt_peers);
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
  /* An active account with a reachable signer can send: no limit. */
  g_autofree gchar *none = gh_account_describe_limits(GH_ACCOUNT_STATE_ACTIVE,
                                                      GH_SIGNER_AVAILABILITY_ACTIVATABLE,
                                                      "auto", TRUE);
  g_assert_null(none);
  g_autofree gchar *running = gh_account_describe_limits(GH_ACCOUNT_STATE_ACTIVE,
                                                         GH_SIGNER_AVAILABILITY_RUNNING,
                                                         "local", TRUE);
  g_assert_null(running);
}

#ifdef GROUNDHOG_TEST_NIP17
typedef struct {
  BusFixture bus;
  MockSigner mock;
  FakeStore store;
  GSettings *settings;
  GhAccountController *accounts;
} EnvelopeFixture;

typedef struct {
  gboolean done;
  GhNip17Envelope *envelope;
  GError *error;
} EnvelopeWait;

static void
envelope_done(GObject *source, GAsyncResult *result, gpointer data)
{
  EnvelopeWait *wait = data;
  (void)source;
  wait->envelope = gh_nip17_envelope_build_finish(result, &wait->error);
  wait->done = TRUE;
}

static gboolean
envelope_finished(gpointer data)
{
  return ((EnvelopeWait *)data)->done;
}

static void
envelope_fixture_up(EnvelopeFixture *fixture)
{
  bus_up(&fixture->bus, FALSE);
  fixture->mock.real_crypto = TRUE;
  mock_signer_up(&fixture->bus, &fixture->mock);
  fixture->settings = fresh_settings(npub_one);
  fixture->accounts = gh_account_controller_new_full(
    fixture->settings, fixture->bus.client, fake_list, &fixture->store);
  spin_until(listed, fixture->accounts);
}

static void
envelope_fixture_down(EnvelopeFixture *fixture)
{
  release_controller(fixture->accounts);
  MockSenders check = { &fixture->bus, &fixture->mock };
  spin_until(mock_senders_closed, &check);
  mock_signer_down(&fixture->bus, &fixture->mock);
  g_object_unref(fixture->settings);
  bus_down(&fixture->bus);
}

static const gchar *
mock_last(EnvelopeFixture *fixture)
{
  return fixture->mock.last_npub;
}

static NostrEvent *
parse_event(const gchar *json)
{
  NostrEvent *event = nostr_event_new();
  g_assert_nonnull(event);
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  return event;
}

static void
assert_wrap_contains_rumor(const gchar *wrap_json, const gchar *recipient,
                           const gchar *recipient_secret, const gchar *sender,
                           NostrEvent *rumor, const gchar *rumor_json)
{
  NostrEvent *wrap = parse_event(wrap_json);
  g_assert_true(nostr_nip59_validate_gift_wrap(wrap));
  g_autofree gchar *wrap_recipient = nostr_nip59_get_recipient(wrap);
  g_assert_cmpstr(wrap_recipient, ==, recipient);
  g_assert_cmpstr(nostr_event_get_pubkey(wrap), !=, sender);
  NostrEvent *seal = nostr_nip59_unwrap(wrap, recipient_secret);
  g_assert_nonnull(seal);
  g_assert_true(nostr_nip17_validate_seal(seal, rumor));
  g_assert_cmpint(nostr_event_get_kind(seal), ==, 13);
  g_assert_cmpstr(nostr_event_get_pubkey(seal), ==, sender);
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, recipient_secret, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, sender, sizeof pk));
  guint8 *plaintext = NULL;
  size_t plaintext_len = 0;
  g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, nostr_event_get_content(seal),
                                         &plaintext, &plaintext_len), ==, 0);
  g_assert_cmpuint(plaintext_len, ==, strlen(rumor_json));
  g_assert_cmpmem(plaintext, plaintext_len, rumor_json, strlen(rumor_json));
  free(plaintext);
  nostr_event_free(seal);
  nostr_event_free(wrap);
}

static void
test_nip17_envelope_roundtrip(void)
{
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  g_autofree gchar *sender = gh_identity_pubkey_hex(npub_one);
  g_autofree gchar *recipient = gh_identity_pubkey_hex(npub_two);
  EnvelopeWait wait = { 0 };
  gh_nip17_envelope_build_async(fixture.accounts, recipient, "hello envelope",
                                NULL, envelope_done, &wait);
  spin_until(envelope_finished, &wait);
  g_assert_no_error(wait.error);
  g_assert_nonnull(wait.envelope);
  g_assert_cmpuint(fixture.mock.calls, ==, 4);
  g_assert_cmpuint(fixture.mock.encrypt_plaintexts->len, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(fixture.mock.encrypt_peers, 0), ==, recipient);
  g_assert_cmpstr(g_ptr_array_index(fixture.mock.encrypt_peers, 1), ==, sender);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpstr(g_ptr_array_index(fixture.mock.encrypt_plaintexts, i), ==,
                    wait.envelope->rumor_json);
  NostrEvent *rumor = parse_event(wait.envelope->rumor_json);
  g_assert_cmpint(nostr_event_get_kind(rumor), ==, 14);
  g_assert_cmpstr(nostr_event_get_pubkey(rumor), ==, sender);
  g_assert_cmpstr(nostr_event_get_content(rumor), ==, "hello envelope");
  g_assert_null(nostr_event_get_sig(rumor));
  g_assert_cmpint(nostr_event_validate_id(rumor, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  const gchar *recipient_secret =
    "0000000000000000000000000000000000000000000000000000000000000002";
  const gchar *sender_secret =
    "0000000000000000000000000000000000000000000000000000000000000001";
  assert_wrap_contains_rumor(wait.envelope->recipient_wrap_json, recipient,
                             recipient_secret, sender, rumor, wait.envelope->rumor_json);
  assert_wrap_contains_rumor(wait.envelope->sender_wrap_json, sender,
                             sender_secret, sender, rumor, wait.envelope->rumor_json);
  NostrEvent *recipient_wrap = parse_event(wait.envelope->recipient_wrap_json);
  NostrEvent *sender_wrap = parse_event(wait.envelope->sender_wrap_json);
  g_assert_cmpstr(nostr_event_get_pubkey(recipient_wrap), !=,
                  nostr_event_get_pubkey(sender_wrap));
  nostr_event_free(recipient_wrap);
  nostr_event_free(sender_wrap);
  nostr_event_free(rumor);
  gh_nip17_envelope_free(wait.envelope);
  envelope_fixture_down(&fixture);
}

static void
test_nip17_envelope_rejects_bad_inputs(void)
{
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  g_autofree gchar *sender = gh_identity_pubkey_hex(npub_one);
  EnvelopeWait bad_peer = { 0 };
  gh_nip17_envelope_build_async(fixture.accounts, "not-a-pubkey", "hello",
                                NULL, envelope_done, &bad_peer);
  spin_until(envelope_finished, &bad_peer);
  g_assert_null(bad_peer.envelope);
  g_assert_error(bad_peer.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&bad_peer.error);
  EnvelopeWait self_peer = { 0 };
  gh_nip17_envelope_build_async(fixture.accounts, sender, "hello",
                                NULL, envelope_done, &self_peer);
  spin_until(envelope_finished, &self_peer);
  g_assert_null(self_peer.envelope);
  g_assert_error(self_peer.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&self_peer.error);
  g_assert_cmpuint(fixture.mock.calls, ==, 0);
  envelope_fixture_down(&fixture);
}

static void
test_nip17_envelope_signer_failures(void)
{
  for (guint failure = 0; failure < 8; failure++) {
    EnvelopeFixture fixture = { 0 };
    envelope_fixture_up(&fixture);
    guint stage = failure < 4 ? failure + 1 : (failure % 2 ? 4 : 2);
    if (failure < 4) fixture.mock.deny_call = stage;
    else if (failure < 6) fixture.mock.bad_sign_call = stage;
    else fixture.mock.wrong_key_call = stage;
    g_autofree gchar *recipient = gh_identity_pubkey_hex(npub_two);
    EnvelopeWait wait = { 0 };
    gh_nip17_envelope_build_async(fixture.accounts, recipient, "hello", NULL,
                                  envelope_done, &wait);
    spin_until(envelope_finished, &wait);
    g_assert_null(wait.envelope);
    if (failure < 4)
      g_assert_error(wait.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED);
    else
      g_assert_true(g_error_matches(wait.error, GH_SIGNER_ERROR,
                         GH_SIGNER_ERROR_INVALID_RESULT) ||
                    g_error_matches(wait.error, GH_SIGNER_ERROR,
                         GH_SIGNER_ERROR_KEY_MISMATCH));
    g_clear_error(&wait.error);
    g_assert_cmpuint(fixture.mock.calls, ==, stage);
    envelope_fixture_down(&fixture);
  }
}

static void
run_interrupted_stage(guint stage, gboolean switch_account)
{
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  fixture.mock.hold_call = stage;
  g_autofree gchar *recipient = gh_identity_pubkey_hex(npub_two);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  EnvelopeWait wait = { 0 };
  gh_nip17_envelope_build_async(fixture.accounts, recipient, "hello", cancel,
                                envelope_done, &wait);
  MockCount count = { &fixture.mock, stage };
  spin_until(mock_count_reached, &count);
  if (switch_account)
    g_assert_true(gh_account_controller_select(fixture.accounts, npub_two, NULL));
  else
    g_cancellable_cancel(cancel);
  spin_until(envelope_finished, &wait);
  g_assert_null(wait.envelope);
  g_assert_error(wait.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&wait.error);
  g_assert_cmpuint(fixture.mock.calls, ==, stage);
  MockSenders check = { &fixture.bus, &fixture.mock };
  spin_until(mock_senders_closed, &check);
  envelope_fixture_down(&fixture);
}

static void
envelope_switch_before_finish(GObject *source, GAsyncResult *result, gpointer data)
{
  EnvelopeWait *wait = data;
  g_assert_true(gh_account_controller_select(GH_ACCOUNT_CONTROLLER(source),
                                              npub_two, NULL));
  wait->envelope = gh_nip17_envelope_build_finish(result, &wait->error);
  wait->done = TRUE;
}

static void
test_nip17_envelope_switch_before_finish(void)
{
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  g_autofree gchar *recipient = gh_identity_pubkey_hex(npub_two);
  EnvelopeWait wait = { 0 };
  gh_nip17_envelope_build_async(fixture.accounts, recipient, "hello", NULL,
                                envelope_switch_before_finish, &wait);
  spin_until(envelope_finished, &wait);
  g_assert_null(wait.envelope);
  g_assert_error(wait.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&wait.error);
  g_assert_cmpuint(fixture.mock.calls, ==, 4);
  envelope_fixture_down(&fixture);
}

static void
test_nip17_envelope_cancel_and_switch(void)
{
  for (guint stage = 1; stage <= 4; stage++) {
    run_interrupted_stage(stage, FALSE);
    run_interrupted_stage(stage, TRUE);
  }
}

/* ---- inbound unwrap ------------------------------------------------------ */

static const gchar *const test_secret[] = {
  NULL,
  "0000000000000000000000000000000000000000000000000000000000000001",
  "0000000000000000000000000000000000000000000000000000000000000002",
  "0000000000000000000000000000000000000000000000000000000000000003",
  "0000000000000000000000000000000000000000000000000000000000000004", /* wrap key */
};

static gchar *
test_pub(guint key)
{
  gchar *pub = nostr_key_get_public(test_secret[key]);
  g_assert_nonnull(pub);
  return pub;
}

typedef struct {
  gboolean done;
  GhNip17Message *message;
  GError *error;
  guint signer_calls;
} UnwrapWait;

/* Signer calls of the last unwrap_now(), as the unwrap itself reports them. */
static guint last_signer_calls;

static void
unwrap_done(GObject *source, GAsyncResult *result, gpointer data)
{
  UnwrapWait *wait = data;
  (void)source;
  wait->signer_calls = gh_nip17_unwrap_get_signer_calls(result);
  wait->message = gh_nip17_unwrap_finish(result, &wait->error);
  wait->done = TRUE;
}

static gboolean
unwrap_finished(gpointer data)
{
  return ((UnwrapWait *)data)->done;
}

static GhNip17Message *
unwrap_now(GhAccountController *accounts, const gchar *wrap_json, GError **error)
{
  UnwrapWait wait = { 0 };
  gh_nip17_unwrap_async(accounts, wrap_json, NULL, unwrap_done, &wait);
  spin_until(unwrap_finished, &wait);
  last_signer_calls = wait.signer_calls;
  g_assert_true((wait.message == NULL) != (wait.error == NULL));
  if (wait.error) g_propagate_error(error, wait.error);
  return wait.message;
}

/* Test-only local keys stand in for a remote peer. The app under test holds
 * no key: its decrypts go through the mock org.nostr.Signer. */
typedef struct {
  guint sender;       /* seal signer */
  guint rumor_author; /* 0: sender */
  guint recipient;    /* wrap p tag and NIP-44 peer */
  guint rumor_p;      /* 0: recipient */
  int rumor_kind;     /* 0: 14 */
  gboolean rumor_no_id, rumor_bad_id, rumor_signed;
  int seal_kind;      /* 0: 13 */
  gboolean seal_tagged, seal_bad_sig;
  guint extra_wrap_p; /* adds a second p tag to the (re-signed) wrap */
} Craft;

static gchar *
craft_wrap(const Craft *c)
{
  guint author_key = c->rumor_author ? c->rumor_author : c->sender;
  g_autofree gchar *sender = test_pub(c->sender);
  g_autofree gchar *author = test_pub(author_key);
  g_autofree gchar *recipient = test_pub(c->recipient);
  g_autofree gchar *rumor_p = test_pub(c->rumor_p ? c->rumor_p : c->recipient);
  NostrEvent *rumor = nostr_nip17_create_rumor(author, rumor_p, "crafted", 0);
  g_assert_nonnull(rumor);
  if (c->rumor_kind) nostr_event_set_kind(rumor, c->rumor_kind);
  if (c->rumor_signed)
    g_assert_cmpint(nostr_event_sign(rumor, test_secret[author_key]), ==, 0);
  else if (c->rumor_bad_id)
    rumor->id = strdup("00000000000000000000000000000000000000000000000000000000000000ab");
  else if (!c->rumor_no_id)
    rumor->id = nostr_event_get_id(rumor);
  char *rumor_json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);

  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, test_secret[c->sender], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, recipient, sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)rumor_json,
                                          strlen(rumor_json), &ciphertext), ==, 0);
  free(rumor_json);
  NostrEvent *seal = nostr_event_new();
  nostr_event_set_kind(seal, c->seal_kind ? c->seal_kind : 13);
  nostr_event_set_pubkey(seal, sender);
  nostr_event_set_content(seal, ciphertext);
  nostr_event_set_created_at(seal, 1700000000);
  nostr_event_set_tags(seal, c->seal_tagged ?
                       nostr_tags_new(1, nostr_tag_new("p", recipient, NULL)) :
                       nostr_tags_new(0));
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(seal, test_secret[c->sender]), ==, 0);
  if (c->seal_bad_sig)
    seal->sig[10] = seal->sig[10] == '0' ? '1' : '0';

  guint8 ephemeral[32];
  g_assert_true(nostr_hex2bin(ephemeral, test_secret[4], sizeof ephemeral));
  NostrEvent *wrap = nostr_nip59_wrap_with_key(seal, recipient, ephemeral);
  nostr_event_free(seal);
  g_assert_nonnull(wrap);
  if (c->extra_wrap_p) {
    g_autofree gchar *extra = test_pub(c->extra_wrap_p);
    nostr_tags_append(nostr_event_get_tags(wrap), nostr_tag_new("p", extra, NULL));
    g_assert_cmpint(nostr_event_sign(wrap, test_secret[4]), ==, 0);
  }
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

static gchar *
tamper(const gchar *json, const gchar *field)
{
  gchar *copy = g_strdup(json);
  g_autofree gchar *needle = g_strdup_printf("\"%s\":\"", field);
  gchar *at = strstr(copy, needle);
  g_assert_nonnull(at);
  at += strlen(needle) + 5;
  *at = *at == '0' ? '1' : '0';
  return copy;
}

static void
test_nip17_unwrap_roundtrip(void)
{
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  g_autofree gchar *sender = gh_identity_pubkey_hex(npub_one);
  g_autofree gchar *recipient = gh_identity_pubkey_hex(npub_two);
  EnvelopeWait built = { 0 };
  gh_nip17_envelope_build_async(fixture.accounts, recipient, "hello inbox", NULL,
                                envelope_done, &built);
  spin_until(envelope_finished, &built);
  g_assert_no_error(built.error);
  NostrEvent *rumor = parse_event(built.envelope->rumor_json);
  g_autofree gchar *rumor_id = g_strdup(rumor->id);
  nostr_event_free(rumor);
  NostrEvent *sender_wrap = parse_event(built.envelope->sender_wrap_json);
  NostrEvent *recipient_wrap = parse_event(built.envelope->recipient_wrap_json);
  guint calls = fixture.mock.calls;

  /* Sender self-copy, unwrapped by the sender's own account. */
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip17Message) self_copy =
    unwrap_now(fixture.accounts, built.envelope->sender_wrap_json, &error);
  g_assert_no_error(error);
  g_assert_true(self_copy->self_copy);
  g_assert_cmpstr(self_copy->account_pubkey, ==, sender);
  g_assert_cmpstr(self_copy->sender_pubkey, ==, sender);
  g_assert_cmpstr(self_copy->rumor_id, ==, rumor_id);
  g_assert_cmpstr(self_copy->wrap_id, ==, sender_wrap->id);
  g_assert_cmpstr(self_copy->rumor_json, ==, built.envelope->rumor_json);
  g_assert_cmpuint(g_strv_length(self_copy->recipients), ==, 1);
  g_assert_cmpstr(self_copy->recipients[0], ==, recipient);
  g_assert_cmpint(self_copy->created_at, >, 0);
  g_assert_cmpuint(fixture.mock.calls, ==, calls + 2);
  g_assert_cmpuint(last_signer_calls, ==, 2);
  g_assert_cmpstr(mock_last(&fixture), ==, npub_one);

  /* The recipient's copy is not addressed to the sender: no signer call. */
  g_assert_null(unwrap_now(fixture.accounts, built.envelope->recipient_wrap_json, &error));
  g_assert_error(error, GH_NIP17_INBOX_ERROR, GH_NIP17_INBOX_ERROR_WRONG_RECIPIENT);
  g_clear_error(&error);
  g_assert_cmpuint(fixture.mock.calls, ==, calls + 2);

  /* The recipient account receives the same canonical rumor. */
  g_assert_true(gh_account_controller_select(fixture.accounts, npub_two, NULL));
  g_autoptr(GhNip17Message) received =
    unwrap_now(fixture.accounts, built.envelope->recipient_wrap_json, &error);
  g_assert_no_error(error);
  g_assert_false(received->self_copy);
  g_assert_cmpstr(received->account_pubkey, ==, recipient);
  g_assert_cmpstr(received->sender_pubkey, ==, sender);
  g_assert_cmpstr(received->rumor_id, ==, rumor_id);
  g_assert_cmpstr(received->wrap_id, ==, recipient_wrap->id);
  g_assert_cmpstr(received->rumor_json, ==, built.envelope->rumor_json);
  g_assert_cmpstr(received->recipients[0], ==, recipient);
  g_assert_null(received->recipients[1]);
  g_assert_cmpint(received->created_at, ==, self_copy->created_at);
  g_assert_cmpstr(mock_last(&fixture), ==, npub_two);
  g_assert_null(unwrap_now(fixture.accounts, built.envelope->sender_wrap_json, &error));
  g_assert_error(error, GH_NIP17_INBOX_ERROR, GH_NIP17_INBOX_ERROR_WRONG_RECIPIENT);
  g_clear_error(&error);

  /* A crafted peer rumor without a declared id gets its canonical id. */
  Craft plain = { .sender = 3, .recipient = 2, .rumor_no_id = TRUE };
  g_autofree gchar *crafted = craft_wrap(&plain);
  g_autoptr(GhNip17Message) peer = unwrap_now(fixture.accounts, crafted, &error);
  g_assert_no_error(error);
  g_autofree gchar *third = test_pub(3);
  g_assert_cmpstr(peer->sender_pubkey, ==, third);
  NostrEvent *canonical = parse_event(peer->rumor_json);
  g_assert_cmpstr(canonical->id, ==, peer->rumor_id);
  g_assert_cmpint(nostr_event_validate_id(canonical, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  nostr_event_free(canonical);

  nostr_event_free(sender_wrap);
  nostr_event_free(recipient_wrap);
  gh_nip17_envelope_free(built.envelope);
  envelope_fixture_down(&fixture);
}

typedef struct {
  const gchar *name;
  Craft craft;
  const gchar *tamper; /* outer field to corrupt after crafting */
  gint code;
  guint calls;         /* signer calls made before rejection */
} RejectCase;

static void
test_nip17_unwrap_rejects(void)
{
  const RejectCase cases[] = {
    { "wrong p", { .sender = 3, .recipient = 1 }, NULL,
      GH_NIP17_INBOX_ERROR_WRONG_RECIPIENT, 0 },
    { "second p", { .sender = 3, .recipient = 2, .extra_wrap_p = 1 }, NULL,
      GH_NIP17_INBOX_ERROR_WRONG_RECIPIENT, 0 },
    { "outer id", { .sender = 3, .recipient = 2 }, "id",
      GH_NIP17_INBOX_ERROR_INVALID_WRAP, 0 },
    { "outer sig", { .sender = 3, .recipient = 2 }, "sig",
      GH_NIP17_INBOX_ERROR_INVALID_WRAP, 0 },
    { "outer content", { .sender = 3, .recipient = 2 }, "content",
      GH_NIP17_INBOX_ERROR_INVALID_WRAP, 0 },
    { "seal sig", { .sender = 3, .recipient = 2, .seal_bad_sig = TRUE }, NULL,
      GH_NIP17_INBOX_ERROR_INVALID_SEAL, 1 },
    { "seal kind", { .sender = 3, .recipient = 2, .seal_kind = 1 }, NULL,
      GH_NIP17_INBOX_ERROR_INVALID_SEAL, 1 },
    { "seal tags", { .sender = 3, .recipient = 2, .seal_tagged = TRUE }, NULL,
      GH_NIP17_INBOX_ERROR_INVALID_SEAL, 1 },
    { "sender mismatch", { .sender = 3, .rumor_author = 1, .recipient = 2 }, NULL,
      GH_NIP17_INBOX_ERROR_SENDER_MISMATCH, 2 },
    /* G21 admits kind 15 only with valid file tags (gh-nip17-file.h). */
    { "kind 15 without file tags", { .sender = 3, .recipient = 2, .rumor_kind = 15 }, NULL,
      GH_NIP17_INBOX_ERROR_INVALID_RUMOR, 2 },
    { "kind 1", { .sender = 3, .recipient = 2, .rumor_kind = 1 }, NULL,
      GH_NIP17_INBOX_ERROR_UNSUPPORTED_KIND, 2 },
    { "signed rumor", { .sender = 3, .recipient = 2, .rumor_signed = TRUE }, NULL,
      GH_NIP17_INBOX_ERROR_INVALID_RUMOR, 2 },
    { "rumor id", { .sender = 3, .recipient = 2, .rumor_bad_id = TRUE }, NULL,
      GH_NIP17_INBOX_ERROR_INVALID_RUMOR, 2 },
    { "not a participant", { .sender = 3, .recipient = 2, .rumor_p = 1 }, NULL,
      GH_NIP17_INBOX_ERROR_WRONG_RECIPIENT, 2 },
  };
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  g_assert_true(gh_account_controller_select(fixture.accounts, npub_two, NULL));
  guint expected_calls = 0;
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    expected_calls += cases[i].calls;
    g_test_message("reject case: %s", cases[i].name);
    g_autofree gchar *wrap = craft_wrap(&cases[i].craft);
    g_autofree gchar *input = cases[i].tamper ? tamper(wrap, cases[i].tamper) : g_strdup(wrap);
    guint before = fixture.mock.calls;
    g_autoptr(GError) error = NULL;
    g_assert_null(unwrap_now(fixture.accounts, input, &error));
    g_assert_error(error, GH_NIP17_INBOX_ERROR, cases[i].code);
    g_assert_cmpuint(fixture.mock.calls - before, ==, cases[i].calls);
    /* What the inbox uses to decide whether to record the wrap as rejected. */
    g_assert_cmpuint(last_signer_calls, ==, cases[i].calls);
  }
  /* Bounds and garbage are rejected before any parse or signer call. */
  g_autofree gchar *huge = g_strnfill(GH_NIP17_MAX_WRAP_JSON + 1, 'x');
  g_autoptr(GError) error = NULL;
  g_assert_null(unwrap_now(fixture.accounts, huge, &error));
  g_assert_error(error, GH_NIP17_INBOX_ERROR, GH_NIP17_INBOX_ERROR_TOO_LARGE);
  g_clear_error(&error);
  g_assert_null(unwrap_now(fixture.accounts, "{\"kind\":1059}", &error));
  g_assert_error(error, GH_NIP17_INBOX_ERROR, GH_NIP17_INBOX_ERROR_INVALID_WRAP);
  g_clear_error(&error);
  g_assert_cmpuint(fixture.mock.calls, ==, expected_calls);
  envelope_fixture_down(&fixture);
}

static void
test_nip17_unwrap_no_account(void)
{
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  Craft craft = { .sender = 3, .recipient = 1 };
  g_autofree gchar *wrap = craft_wrap(&craft);
  g_assert_true(gh_account_controller_select(fixture.accounts, "", NULL));
  g_autoptr(GError) error = NULL;
  g_assert_null(unwrap_now(fixture.accounts, wrap, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_assert_cmpuint(fixture.mock.calls, ==, 0);
  envelope_fixture_down(&fixture);
}

static void
test_nip17_unwrap_signer_denied(void)
{
  for (guint stage = 1; stage <= 2; stage++) {
    EnvelopeFixture fixture = { 0 };
    envelope_fixture_up(&fixture);
    fixture.mock.deny_call = stage;
    Craft craft = { .sender = 3, .recipient = 1 };
    g_autofree gchar *wrap = craft_wrap(&craft);
    g_autoptr(GError) error = NULL;
    g_assert_null(unwrap_now(fixture.accounts, wrap, &error));
    g_assert_error(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED);
    g_assert_cmpuint(fixture.mock.calls, ==, stage);
    envelope_fixture_down(&fixture);
  }
}

static void
run_interrupted_unwrap(guint stage, gboolean switch_account)
{
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  fixture.mock.hold_call = stage;
  Craft craft = { .sender = 3, .recipient = 1 };
  g_autofree gchar *wrap = craft_wrap(&craft);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  UnwrapWait wait = { 0 };
  gh_nip17_unwrap_async(fixture.accounts, wrap, cancel, unwrap_done, &wait);
  MockCount count = { &fixture.mock, stage };
  spin_until(mock_count_reached, &count);
  if (switch_account)
    g_assert_true(gh_account_controller_select(fixture.accounts, npub_two, NULL));
  else
    g_cancellable_cancel(cancel);
  spin_until(unwrap_finished, &wait);
  g_assert_null(wait.message);
  g_assert_error(wait.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&wait.error);
  g_assert_cmpuint(fixture.mock.calls, ==, stage);
  /* The held approval's private sender was closed, revoking it. */
  MockSenders check = { &fixture.bus, &fixture.mock };
  spin_until(mock_senders_closed, &check);
  envelope_fixture_down(&fixture);
}

static void
test_nip17_unwrap_cancel_and_switch(void)
{
  for (guint stage = 1; stage <= 2; stage++) {
    run_interrupted_unwrap(stage, FALSE);
    run_interrupted_unwrap(stage, TRUE);
  }
  /* Already cancelled: nothing reaches the signer. */
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  Craft craft = { .sender = 3, .recipient = 1 };
  g_autofree gchar *wrap = craft_wrap(&craft);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  UnwrapWait wait = { 0 };
  gh_nip17_unwrap_async(fixture.accounts, wrap, cancel, unwrap_done, &wait);
  spin_until(unwrap_finished, &wait);
  g_assert_error(wait.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&wait.error);
  g_assert_cmpuint(fixture.mock.calls, ==, 0);
  envelope_fixture_down(&fixture);
}

/* A reply that lands after the account switched away (and back) is stale:
 * the generation moved on even though the npub matches again. */
static void
unwrap_switch_before_finish(GObject *source, GAsyncResult *result, gpointer data)
{
  UnwrapWait *wait = data;
  GhAccountController *accounts = GH_ACCOUNT_CONTROLLER(source);
  g_assert_true(gh_account_controller_select(accounts, npub_two, NULL));
  g_assert_true(gh_account_controller_select(accounts, npub_one, NULL));
  wait->message = gh_nip17_unwrap_finish(result, &wait->error);
  wait->done = TRUE;
}

static void
test_nip17_unwrap_stale_callback(void)
{
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  Craft craft = { .sender = 3, .recipient = 1 };
  g_autofree gchar *wrap = craft_wrap(&craft);
  UnwrapWait wait = { 0 };
  gh_nip17_unwrap_async(fixture.accounts, wrap, NULL, unwrap_switch_before_finish, &wait);
  spin_until(unwrap_finished, &wait);
  g_assert_null(wait.message);
  g_assert_error(wait.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&wait.error);
  g_assert_cmpuint(fixture.mock.calls, ==, 2);
  envelope_fixture_down(&fixture);
}

static GhNip17Message *
fake_message(const gchar *account, gchar wrap_digit, gchar rumor_digit)
{
  GhNip17Message *message = g_new0(GhNip17Message, 1);
  message->account_pubkey = g_strdup(account);
  message->wrap_id = g_strnfill(64, wrap_digit);
  message->rumor_id = g_strnfill(64, rumor_digit);
  return message;
}

static void
test_nip17_seen_restart(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = g_dir_make_tmp("groundhog-seen-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *path = g_build_filename(dir, "seen", NULL);
  g_autofree gchar *account = test_pub(2);
  g_autofree gchar *other = test_pub(1);

  g_autoptr(GhNip17Seen) seen = gh_nip17_seen_open(path, account, 3, &error);
  g_assert_no_error(error);
  g_autoptr(GhNip17Message) first = fake_message(account, 'a', 'b');
  g_assert_false(gh_nip17_seen_has_wrap(seen, first->wrap_id));
  g_assert_true(gh_nip17_seen_record(seen, first, &error));
  g_assert_no_error(error);
  /* The same rumor re-wrapped: a new wrap id, a known rumor id. */
  g_autoptr(GhNip17Message) rewrap = fake_message(account, 'c', 'b');
  g_assert_false(gh_nip17_seen_has_wrap(seen, rewrap->wrap_id));
  g_assert_true(gh_nip17_seen_has_rumor(seen, rewrap->rumor_id));
  g_assert_true(gh_nip17_seen_record(seen, rewrap, &error));
  g_autoptr(GhNip17Message) foreign = fake_message(other, 'd', 'e');
  g_assert_false(gh_nip17_seen_record(seen, foreign, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_clear_pointer(&seen, gh_nip17_seen_free);

  /* Restart: the seen keys survive, including a torn final append. */
  g_autofree gchar *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_autofree gchar *torn = g_strconcat(contents, "w 12", NULL);
  g_assert_true(g_file_set_contents(path, torn, -1, NULL));
  seen = gh_nip17_seen_open(path, account, 3, &error);
  g_assert_no_error(error);
  g_assert_true(gh_nip17_seen_has_wrap(seen, first->wrap_id));
  g_assert_true(gh_nip17_seen_has_wrap(seen, rewrap->wrap_id));
  g_assert_true(gh_nip17_seen_has_rumor(seen, first->rumor_id));
  g_assert_false(gh_nip17_seen_has_rumor(seen, first->wrap_id));

  /* Bounded: the oldest key is evicted and the log is compacted. */
  g_autoptr(GhNip17Message) later = fake_message(account, 'f', '0');
  g_assert_true(gh_nip17_seen_record(seen, later, &error));
  g_autoptr(GhNip17Message) latest = fake_message(account, '1', '2');
  g_assert_true(gh_nip17_seen_record(seen, latest, &error));
  g_assert_no_error(error);
  g_clear_pointer(&seen, gh_nip17_seen_free);
  seen = gh_nip17_seen_open(path, account, 3, &error);
  g_assert_no_error(error);
  g_assert_false(gh_nip17_seen_has_wrap(seen, first->wrap_id));
  g_assert_true(gh_nip17_seen_has_wrap(seen, latest->wrap_id));
  g_assert_true(gh_nip17_seen_has_rumor(seen, latest->rumor_id));
  g_clear_pointer(&seen, gh_nip17_seen_free);

  /* The file is bound to its account and parsed strictly. */
  g_assert_null(gh_nip17_seen_open(path, other, 3, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_clear_pointer(&contents, g_free);
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_autofree gchar *corrupt = g_strconcat(contents, "x nothex\n", NULL);
  g_assert_true(g_file_set_contents(path, corrupt, -1, NULL));
  g_assert_null(gh_nip17_seen_open(path, account, 3, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);

  g_unlink(path);
  g_rmdir(dir);
}

/* The rejected namespace ("x" lines): a seen-set written before it existed
 * still loads, a rejected id is not a seen wrap, and it survives a restart. */
static void
test_nip17_seen_rejected(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = g_dir_make_tmp("groundhog-seen-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *path = g_build_filename(dir, "seen", NULL);
  g_autofree gchar *account = test_pub(2);
  g_autofree gchar *wrap = g_strnfill(64, 'a');
  g_autofree gchar *rumor = g_strnfill(64, 'b');
  g_autofree gchar *bad = g_strnfill(64, 'c');
  g_autofree gchar *old = g_strdup_printf("groundhog-nip17-seen 1 %s\nw %s\nr %s\n", account,
                                          wrap, rumor);
  g_assert_true(g_file_set_contents(path, old, -1, NULL));

  g_autoptr(GhNip17Seen) seen = gh_nip17_seen_open(path, account, 8, &error);
  g_assert_no_error(error);
  g_assert_true(gh_nip17_seen_has_wrap(seen, wrap));
  g_assert_true(gh_nip17_seen_has_rumor(seen, rumor));
  g_assert_false(gh_nip17_seen_has_rejected(seen, wrap));
  g_assert_true(gh_nip17_seen_record_rejected(seen, bad, &error));
  g_assert_no_error(error);
  g_assert_true(gh_nip17_seen_has_rejected(seen, bad));
  g_assert_false(gh_nip17_seen_has_wrap(seen, bad));
  g_assert_true(gh_nip17_seen_record_rejected(seen, bad, &error)); /* idempotent */
  g_autofree gchar *upper = g_strnfill(64, 'C');
  g_assert_false(gh_nip17_seen_record_rejected(seen, upper, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_clear_pointer(&seen, gh_nip17_seen_free);

  g_autofree gchar *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_autofree gchar *expected = g_strdup_printf("%sx %s\n", old, bad);
  g_assert_cmpstr(contents, ==, expected); /* appended once, old lines kept */
  seen = gh_nip17_seen_open(path, account, 8, &error);
  g_assert_no_error(error);
  g_assert_true(gh_nip17_seen_has_rejected(seen, bad));
  g_assert_true(gh_nip17_seen_has_wrap(seen, wrap));
  g_assert_false(gh_nip17_seen_has_rejected(seen, rumor));
  g_clear_pointer(&seen, gh_nip17_seen_free);
  g_unlink(path);
  g_rmdir(dir);
}

/* A crash can leave a torn final append. The next record must not glue a new
 * entry onto it; with room to spare it would otherwise append, not compact. */
static void
test_nip17_seen_torn_tail(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = g_dir_make_tmp("groundhog-seen-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *path = g_build_filename(dir, "seen", NULL);
  g_autofree gchar *account = test_pub(2);

  g_autoptr(GhNip17Seen) seen = gh_nip17_seen_open(path, account, 16, &error);
  g_assert_no_error(error);
  g_autoptr(GhNip17Message) old = fake_message(account, 'a', 'b');
  g_assert_true(gh_nip17_seen_record(seen, old, &error));
  g_assert_no_error(error);
  g_clear_pointer(&seen, gh_nip17_seen_free);

  g_autofree gchar *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_autofree gchar *torn = g_strconcat(contents, "w 12", NULL);
  g_assert_true(g_file_set_contents(path, torn, -1, NULL));

  seen = gh_nip17_seen_open(path, account, 16, &error);
  g_assert_no_error(error);
  g_autoptr(GhNip17Message) fresh = fake_message(account, 'c', 'd');
  g_assert_true(gh_nip17_seen_record(seen, fresh, &error));
  g_assert_no_error(error);
  g_clear_pointer(&seen, gh_nip17_seen_free);

  seen = gh_nip17_seen_open(path, account, 16, &error);
  g_assert_no_error(error);
  g_assert_true(gh_nip17_seen_has_wrap(seen, old->wrap_id));
  g_assert_true(gh_nip17_seen_has_rumor(seen, old->rumor_id));
  g_assert_true(gh_nip17_seen_has_wrap(seen, fresh->wrap_id));
  g_assert_true(gh_nip17_seen_has_rumor(seen, fresh->rumor_id));
  g_clear_pointer(&seen, gh_nip17_seen_free);

  g_unlink(path);
  g_rmdir(dir);
}

#ifdef G_OS_UNIX
static void
assert_private_mode(const gchar *path)
{
  GStatBuf st;
  g_assert_cmpint(g_stat(path, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0600);
}

/* The seen-set reveals per-account message metadata: owner-only, whether it
 * was created by an append or by a compacting rewrite. */
static void
test_nip17_seen_file_mode(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = g_dir_make_tmp("groundhog-seen-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *path = g_build_filename(dir, "seen", NULL);
  g_autofree gchar *account = test_pub(2);
  mode_t old_umask = umask(0022);

  /* First record creates the file. */
  g_autoptr(GhNip17Seen) seen = gh_nip17_seen_open(path, account, 2, &error);
  g_assert_no_error(error);
  g_autoptr(GhNip17Message) first = fake_message(account, 'a', 'b');
  g_assert_true(gh_nip17_seen_record(seen, first, &error));
  assert_private_mode(path);

  /* An append to an existing file keeps it private. */
  g_assert_cmpint(g_chmod(path, 0644), ==, 0);
  g_autoptr(GhNip17Message) second = fake_message(account, 'c', 'd');
  g_assert_true(gh_nip17_seen_record(seen, second, &error));
  g_assert_no_error(error);
  assert_private_mode(path);

  /* A compacting rewrite (capacity 2, now past 2x) is private too. */
  g_autoptr(GhNip17Message) third = fake_message(account, 'e', 'f');
  g_assert_true(gh_nip17_seen_record(seen, third, &error));
  g_assert_no_error(error);
  assert_private_mode(path);
  g_clear_pointer(&seen, gh_nip17_seen_free);

  /* Recreating a file deleted underneath is private too. */
  g_unlink(path);
  seen = gh_nip17_seen_open(path, account, 2, &error);
  g_assert_no_error(error);
  g_autoptr(GhNip17Message) fourth = fake_message(account, '1', '2');
  g_assert_true(gh_nip17_seen_record(seen, fourth, &error));
  g_assert_no_error(error);
  assert_private_mode(path);
  g_clear_pointer(&seen, gh_nip17_seen_free);

  umask(old_umask);
  g_unlink(path);
  g_rmdir(dir);
}
#endif
#endif

#ifdef GROUNDHOG_TEST_NIP17
/* ---- disappearing messages: expiration on seal, wrap and rumor (G07a) ---- */

/* NIP-17: "Clients MAY offer disappearing messages by setting an `expiration`
 * tag in the gift wrap of each receiver ... This tag SHOULD be included on the
 * `kind:13` seal as well, in case it leaks." NIP-59 otherwise requires empty
 * seal tags, so the seal admits exactly one well-formed expiration and
 * nothing else. The peer (key 3) sends to the active account (key 2). */
#define EXP_TAGS 3
typedef struct {
  const gchar *name;
  const gchar *rumor[EXP_TAGS][4]; /* extra rumor tags after its p tag */
  const gchar *seal[EXP_TAGS][4];  /* all seal tags */
  const gchar *wrap[EXP_TAGS][4];  /* extra wrap tags after its p tag */
} ExpCraft;

static guint
exp_add_tags(NostrTags *tags, const gchar *const specs[][4])
{
  guint added = 0;
  for (guint i = 0; i < EXP_TAGS && specs[i][0]; i++, added++) {
    NostrTag *tag = nostr_tag_new(specs[i][0], NULL);
    for (guint j = 1; j < 4 && specs[i][j]; j++)
      nostr_tag_append(tag, specs[i][j]);
    nostr_tags_append(tags, tag);
  }
  return added;
}

static gchar *
craft_expiring_wrap(const ExpCraft *c)
{
  g_autofree gchar *sender = test_pub(3);
  g_autofree gchar *recipient = test_pub(2);
  NostrEvent *rumor = nostr_nip17_create_rumor(sender, recipient, "expiring", 0);
  g_assert_nonnull(rumor);
  exp_add_tags(nostr_event_get_tags(rumor), c->rumor);
  rumor->id = nostr_event_get_id(rumor);
  char *rumor_json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);

  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, test_secret[3], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, recipient, sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)rumor_json,
                                          strlen(rumor_json), &ciphertext), ==, 0);
  free(rumor_json);
  NostrEvent *seal = nostr_event_new();
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, sender);
  nostr_event_set_content(seal, ciphertext);
  nostr_event_set_created_at(seal, 1700000000);
  NostrTags *seal_tags = nostr_tags_new(0);
  exp_add_tags(seal_tags, c->seal);
  nostr_event_set_tags(seal, seal_tags);
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(seal, test_secret[3]), ==, 0);

  guint8 ephemeral[32];
  g_assert_true(nostr_hex2bin(ephemeral, test_secret[4], sizeof ephemeral));
  NostrEvent *wrap = nostr_nip59_wrap_with_key(seal, recipient, ephemeral);
  nostr_event_free(seal);
  g_assert_nonnull(wrap);
  if (exp_add_tags(nostr_event_get_tags(wrap), c->wrap) > 0)
    g_assert_cmpint(nostr_event_sign(wrap, test_secret[4]), ==, 0);
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

/* Returns the number of occurrences of `needle` in `haystack`. */
static guint
exp_count(const gchar *haystack, const gchar *needle)
{
  guint n = 0;
  for (const gchar *at = strstr(haystack, needle); at; at = strstr(at + 1, needle)) n++;
  return n;
}

static void
test_nip17_seal_expiration_admit(void)
{
  struct {
    ExpCraft craft;
    gint64 rumor, seal, wrap, expires_at;
  } cases[] = {
    /* The interop case: NIP-17 wrap and seal both carry the expiration. */
    { { .name = "seal and wrap",
        .seal = { { "expiration", "1700086400" } },
        .wrap = { { "expiration", "1700089200" } } },
      0, 1700086400, 1700089200, 1700086400 },
    { { .name = "seal only", .seal = { { "expiration", "1700086400" } } },
      0, 1700086400, 0, 1700086400 },
    { { .name = "wrap only", .wrap = { { "expiration", "1700089200" } } },
      0, 0, 1700089200, 1700089200 },
    { { .name = "all layers",
        .rumor = { { "expiration", "1700086399" } },
        .seal = { { "expiration", "1700086400" } },
        .wrap = { { "expiration", "1700089200" } } },
      1700086399, 1700086400, 1700089200, 1700086399 },
    { { .name = "rumor only", .rumor = { { "expiration", "1700086399" } } },
      1700086399, 0, 0, 1700086399 },
    { { .name = "none" }, 0, 0, 0, 0 },
    /* Already expired is admitted and surfaced; dropping it is the caller's
     * policy (seen-only, not stored), so it can still be recorded. */
    { { .name = "already expired", .seal = { { "expiration", "1" } } }, 0, 1, 0, 1 },
    { { .name = "upper bound", .seal = { { "expiration", "253402300799" } } },
      0, GH_NIP17_MAX_EXPIRATION, 0, GH_NIP17_MAX_EXPIRATION },
  };
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  g_assert_true(gh_account_controller_select(fixture.accounts, npub_two, NULL));
  g_autofree gchar *peer = test_pub(3);
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_test_message("admit case: %s", cases[i].craft.name);
    g_autofree gchar *wrap = craft_expiring_wrap(&cases[i].craft);
    guint before = fixture.mock.calls;
    g_autoptr(GError) error = NULL;
    g_autoptr(GhNip17Message) message = unwrap_now(fixture.accounts, wrap, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.mock.calls - before, ==, 2);
    g_assert_cmpstr(message->sender_pubkey, ==, peer);
    g_assert_false(message->self_copy);
    g_assert_cmpint(message->rumor_expiration, ==, cases[i].rumor);
    g_assert_cmpint(message->seal_expiration, ==, cases[i].seal);
    g_assert_cmpint(message->wrap_expiration, ==, cases[i].wrap);
    g_assert_cmpint(message->expires_at, ==, cases[i].expires_at);
    /* The rumor expiration stays in the canonical rumor; the seal and wrap
     * ones never leak into it. */
    g_assert_cmpuint(exp_count(message->rumor_json, "\"expiration\""), ==,
                     cases[i].rumor ? 1 : 0);
  }
  envelope_fixture_down(&fixture);
}

static void
test_nip17_seal_expiration_rejects(void)
{
  const ExpCraft cases[] = {
    { .name = "two expirations",
      .seal = { { "expiration", "1700086400" }, { "expiration", "1700086400" } } },
    { .name = "two different expirations",
      .seal = { { "expiration", "1700086400" }, { "expiration", "1800000000" } } },
    { .name = "expiration and p",
      .seal = { { "expiration", "1700086400" }, { "p", "00" } } },
    { .name = "p before expiration",
      .seal = { { "p", "00" }, { "expiration", "1700086400" } } },
    { .name = "other tag", .seal = { { "alt", "hello" } } },
    { .name = "no value", .seal = { { "expiration" } } },
    { .name = "extra element", .seal = { { "expiration", "1700086400", "x" } } },
    { .name = "empty", .seal = { { "expiration", "" } } },
    { .name = "zero", .seal = { { "expiration", "0" } } },
    { .name = "leading zero", .seal = { { "expiration", "01700086400" } } },
    { .name = "negative", .seal = { { "expiration", "-1700086400" } } },
    { .name = "plus sign", .seal = { { "expiration", "+1700086400" } } },
    { .name = "space", .seal = { { "expiration", " 1700086400" } } },
    { .name = "trailing junk", .seal = { { "expiration", "1700086400s" } } },
    { .name = "fraction", .seal = { { "expiration", "1700086400.5" } } },
    { .name = "exponent", .seal = { { "expiration", "17e8" } } },
    { .name = "hex", .seal = { { "expiration", "0x6553f100" } } },
    { .name = "milliseconds past bound", .seal = { { "expiration", "1700086400000" } } },
    { .name = "bound plus one", .seal = { { "expiration", "253402300800" } } },
    { .name = "int64 overflow", .seal = { { "expiration", "99999999999999999999999" } } },
    { .name = "wrong case", .seal = { { "Expiration", "1700086400" } } },
  };
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  g_assert_true(gh_account_controller_select(fixture.accounts, npub_two, NULL));
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_test_message("seal reject case: %s", cases[i].name);
    g_autofree gchar *wrap = craft_expiring_wrap(&cases[i]);
    guint before = fixture.mock.calls;
    g_autoptr(GError) error = NULL;
    g_assert_null(unwrap_now(fixture.accounts, wrap, &error));
    g_assert_error(error, GH_NIP17_INBOX_ERROR, GH_NIP17_INBOX_ERROR_INVALID_SEAL);
    /* Only the wrap decrypt ran: the seal is rejected before its decrypt. */
    g_assert_cmpuint(fixture.mock.calls - before, ==, 1);
  }
  envelope_fixture_down(&fixture);
}

static void
test_nip17_seal_expiration_other_layers(void)
{
  struct {
    ExpCraft craft;
    gint code;
    guint calls;
  } cases[] = {
    { { .name = "wrap two expirations",
        .wrap = { { "expiration", "1700089200" }, { "expiration", "1700089200" } } },
      GH_NIP17_INBOX_ERROR_INVALID_WRAP, 0 },
    { { .name = "wrap malformed", .wrap = { { "expiration", "soon" } } },
      GH_NIP17_INBOX_ERROR_INVALID_WRAP, 0 },
    { { .name = "wrap no value", .wrap = { { "expiration" } } },
      GH_NIP17_INBOX_ERROR_INVALID_WRAP, 0 },
    { { .name = "rumor two expirations",
        .rumor = { { "expiration", "1700086399" }, { "expiration", "1700086300" } } },
      GH_NIP17_INBOX_ERROR_INVALID_RUMOR, 2 },
    { { .name = "rumor malformed", .rumor = { { "expiration", "-1" } } },
      GH_NIP17_INBOX_ERROR_INVALID_RUMOR, 2 },
    /* A valid seal expiration does not excuse a bad wrap expiration. */
    { { .name = "valid seal, bad wrap",
        .seal = { { "expiration", "1700086400" } },
        .wrap = { { "expiration", "1700089200.0" } } },
      GH_NIP17_INBOX_ERROR_INVALID_WRAP, 0 },
  };
  EnvelopeFixture fixture = { 0 };
  envelope_fixture_up(&fixture);
  g_assert_true(gh_account_controller_select(fixture.accounts, npub_two, NULL));
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_test_message("layer reject case: %s", cases[i].craft.name);
    g_autofree gchar *wrap = craft_expiring_wrap(&cases[i].craft);
    guint before = fixture.mock.calls;
    g_autoptr(GError) error = NULL;
    g_assert_null(unwrap_now(fixture.accounts, wrap, &error));
    g_assert_error(error, GH_NIP17_INBOX_ERROR, cases[i].code);
    g_assert_cmpuint(fixture.mock.calls - before, ==, cases[i].calls);
  }
  /* Other wrap and rumor tags remain allowed next to an expiration. */
  const ExpCraft tolerated = {
    .name = "other tags",
    .rumor = { { "subject", "hi" }, { "expiration", "1700086399" } },
    .seal = { { "expiration", "1700086400" } },
    .wrap = { { "alt", "x" }, { "expiration", "1700089200" } },
  };
  g_autofree gchar *wrap = craft_expiring_wrap(&tolerated);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip17Message) message = unwrap_now(fixture.accounts, wrap, &error);
  g_assert_no_error(error);
  g_assert_cmpint(message->expires_at, ==, 1700086399);
  g_assert_cmpint(message->wrap_expiration, ==, 1700089200);
  envelope_fixture_down(&fixture);
}
#endif

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
#ifdef GROUNDHOG_TEST_NIP17
  g_test_add_func("/groundhog/nip17/envelope-roundtrip", test_nip17_envelope_roundtrip);
  g_test_add_func("/groundhog/nip17/envelope-bad-input", test_nip17_envelope_rejects_bad_inputs);
  g_test_add_func("/groundhog/nip17/envelope-signer-failures", test_nip17_envelope_signer_failures);
  g_test_add_func("/groundhog/nip17/envelope-cancel-switch", test_nip17_envelope_cancel_and_switch);
  g_test_add_func("/groundhog/nip17/envelope-switch-before-finish", test_nip17_envelope_switch_before_finish);
  g_test_add_func("/groundhog/nip17/unwrap-roundtrip", test_nip17_unwrap_roundtrip);
  g_test_add_func("/groundhog/nip17/unwrap-rejects", test_nip17_unwrap_rejects);
  g_test_add_func("/groundhog/nip17/unwrap-no-account", test_nip17_unwrap_no_account);
  g_test_add_func("/groundhog/nip17/unwrap-signer-denied", test_nip17_unwrap_signer_denied);
  g_test_add_func("/groundhog/nip17/unwrap-cancel-switch", test_nip17_unwrap_cancel_and_switch);
  g_test_add_func("/groundhog/nip17/unwrap-stale-callback", test_nip17_unwrap_stale_callback);
  g_test_add_func("/groundhog/nip17/seen-restart", test_nip17_seen_restart);
  g_test_add_func("/groundhog/nip17/seen-torn-tail", test_nip17_seen_torn_tail);
  g_test_add_func("/groundhog/nip17/seen-rejected", test_nip17_seen_rejected);
#ifdef G_OS_UNIX
  g_test_add_func("/groundhog/nip17/seen-file-mode", test_nip17_seen_file_mode);
#endif
#endif
#ifdef GROUNDHOG_TEST_NIP17
  g_test_add_func("/groundhog/nip17/seal-expiration-admit", test_nip17_seal_expiration_admit);
  g_test_add_func("/groundhog/nip17/seal-expiration-rejects", test_nip17_seal_expiration_rejects);
  g_test_add_func("/groundhog/nip17/seal-expiration-other-layers",
                  test_nip17_seal_expiration_other_layers);
#endif
  int status = g_test_run();
  g_free(npub_one);
  g_free(npub_two);
  g_free(npub_three);
  return status;
}
