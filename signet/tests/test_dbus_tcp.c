/* SPDX-License-Identifier: MIT */
/* Access GDBusServer's ephemeral address, as in test_challenge_transports. */
#include "../src/dbus_tcp.c"
#include "signet/dbus_unix.h"
#include "signet/key_store.h"
#include "signet/nostr_auth.h"
#include "test_check.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <sodium.h>
#include <time.h>
#include <unistd.h>

#define AGENT "dbus-test-agent"
#define OBJECT "/net/signet/Signer"
#define AUTH "net.signet.Auth"
#define SIGNER "net.signet.Signer"
#define CREDENTIALS "net.signet.Credentials"
#define TCP_DENIED "net.signet.Error.CredentialsOverTcpDisabled"
#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
static const char signing_key[] = "0000000000000000000000000000000000000000000000000000000000000001";

static SignetKeyStore *seed(const char *path) {
  SignetKeyStoreConfig cfg = { .db_path = path, .master_key = MASTER_KEY };
  SignetKeyStore *keys = signet_key_store_new(NULL, &cfg);
  CHECK(keys && signet_key_store_get_store(keys));
  uint8_t raw[32] = {0};
  raw[31] = 1;
  char pubkey[65];
  CHECK(signet_key_store_adopt_agent(keys, AGENT, raw, NULL, "initial", NULL,
                                    NULL, 0, pubkey, NULL) == SIGNET_ADOPT_OK);
  return keys;
}

static GVariant *call(GDBusConnection *connection, const char *destination,
                      const char *interface, const char *method, GVariant *args,
                      const char *expected_error) {
  g_autoptr(GError) error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(connection, destination, OBJECT,
      interface, method, args, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  if (expected_error) {
    g_autofree char *name = error ? g_dbus_error_get_remote_error(error) : NULL;
    if (g_strcmp0(name, expected_error) != 0)
      g_printerr("%s: expected %s, got %s\n", method, expected_error,
                 error ? error->message : "a successful reply");
    CHECK(!reply && name && strcmp(name, expected_error) == 0);
  } else {
    if (error) g_printerr("%s: %s\n", method, error->message);
    CHECK(reply && !error);
  }
  return reply;
}

static GDBusConnection *authenticate(const char *address) {
  g_autoptr(GError) error = NULL;
  GDBusConnection *conn = g_dbus_connection_new_for_address_sync(address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, NULL, &error);
  if (error) g_printerr("%s: %s\n", address, error->message);
  CHECK(conn && !error);

  g_autoptr(GVariant) challenge_reply = call(conn, NULL, AUTH, "GetChallenge",
                                             g_variant_new("(s)", AGENT), NULL);
  const char *challenge;
  g_variant_get(challenge_reply, "(&s)", &challenge);
  NostrEvent *event = nostr_event_new();
  CHECK(event);
  nostr_event_set_kind(event, SIGNET_AUTH_KIND);
  nostr_event_set_created_at(event, time(NULL));
  nostr_event_set_content(event, "");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("agent", AGENT, NULL));
  nostr_tags_append(tags, nostr_tag_new("purpose", "signet-auth", NULL));
  nostr_tags_append(tags, nostr_tag_new("challenge", challenge, NULL));
  nostr_event_set_tags(event, tags);
  CHECK(nostr_event_sign(event, signing_key) == 0);
  char *json = nostr_event_serialize_compact(event);
  CHECK(json);
  g_autoptr(GVariant) auth_reply = call(conn, NULL, AUTH, "Authenticate",
                                       g_variant_new("(s)", json), NULL);
  free(json);
  nostr_event_free(event);
  gboolean ok;
  const char *agent;
  g_variant_get(auth_reply, "(b&s)", &ok, &agent);
  CHECK(ok && strcmp(agent, AGENT) == 0);
  return conn;
}

static void check_refused(GDBusConnection *conn, const char *id, const char *error) {
  call(conn, NULL, CREDENTIALS, "GetToken", g_variant_new("(s)", id), error);
  call(conn, NULL, CREDENTIALS, "GetSession", g_variant_new("(s)", "https://example.test"), error);
}

static void check_allowed(GDBusConnection *conn, const char *destination, const char *id) {
  g_autoptr(GVariant) token = call(conn, destination, CREDENTIALS, "GetToken",
                                     g_variant_new("(s)", id), NULL);
  g_autoptr(GVariant) session = call(conn, destination, CREDENTIALS, "GetSession",
                                       g_variant_new("(s)", "https://example.test"), NULL);
  const char *value;
  gint64 expires;
  CHECK(g_variant_is_of_type(token, G_VARIANT_TYPE("(sx)")));
  g_variant_get(token, "(&sx)", &value, &expires);
  CHECK(strcmp(value, "626561726572") == 0 && expires > time(NULL)); /* "bearer" */
  CHECK(g_variant_is_of_type(session, G_VARIANT_TYPE("(sx)")));
  g_variant_get(session, "(&sx)", &value, &expires);
  CHECK(strlen(value) == 64 && strspn(value, "0123456789abcdef") == 64 && expires > time(NULL));
}

typedef struct {
  GMainLoop *loop;
  GThread *client;
  GDBusConnection *bus;
  const char *tcp_default;
  const char *tcp_opt_in;
  const char *credential_id;
} WireFixture;

static gboolean quit_loop(gpointer data) {
  g_main_loop_quit(data);
  return G_SOURCE_REMOVE;
}

static gpointer authorized_client(gpointer data) {
  WireFixture *f = data;
  g_autoptr(GDBusConnection) closed = authenticate(f->tcp_default);
  g_autoptr(GDBusConnection) opted_in = authenticate(f->tcp_opt_in);
  check_refused(closed, f->credential_id, TCP_DENIED);
  check_allowed(opted_in, NULL, f->credential_id);
  check_allowed(f->bus, SIGNER, f->credential_id);
  CHECK(g_dbus_connection_close_sync(closed, NULL, NULL));
  CHECK(g_dbus_connection_close_sync(opted_in, NULL, NULL));
  g_main_context_invoke(NULL, quit_loop, f->loop);
  return NULL;
}

static void on_unix_ready(GDBusConnection *connection, const gchar *name,
                          const gchar *owner, gpointer data) {
  (void)name;
  (void)owner;
  WireFixture *f = data;
  CHECK(!f->client);
  f->bus = g_object_ref(connection);
  f->client = g_thread_new("authorized-client", authorized_client, f);
}

static char *uid_resolve(uid_t uid, void *data) {
  (void)data;
  return uid == geteuid() ? g_strdup(AGENT) : NULL;
}

static char *fleet_pubkey(const char *id, void *data) {
  char hex[65];
  return signet_key_store_get_agent_pubkey(data, id, hex, sizeof(hex)) ? g_strdup(hex) : NULL;
}

static void test_authorized(void) {
  g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  SignetKeyStore *keys = seed(":memory:");
  SignetStore *store = signet_key_store_get_store(keys);
  SignetPolicyRegistry *registry = signet_policy_registry_new();
  char *caps[] = { SIGNET_CAP_CREDENTIAL_GET_TOKEN, SIGNET_CAP_CREDENTIAL_GET_SESSION };
  SignetAgentPolicy policy = { .name = "test", .capabilities = caps, .n_capabilities = G_N_ELEMENTS(caps) };
  CHECK(signet_policy_registry_add(registry, &policy) == 0);
  CHECK(signet_policy_registry_assign(registry, AGENT, policy.name) == 0);
  SignetSecretMetadata cred = {0};
  int64_t now = time(NULL);
  CHECK(signet_store_create_secret(store, AGENT, SIGNET_SECRET_API_TOKEN, "test",
      (const uint8_t *)"bearer", 6, NULL, now + 3600, "created", NULL, now, &cred) == SIGNET_SECRET_OK);

  SignetChallengeStore *challenges = signet_challenge_store_new();
  SignetFleetRegistry fleet = { .get_agent_pubkey = fleet_pubkey, .user_data = keys };
  SignetDbusTcpServerConfig tcp = { .listen_address = "tcp:host=127.0.0.1,port=0",
    .keys = keys, .store = store, .policy = registry, .challenges = challenges, .fleet = &fleet };
  SignetDbusTcpServer *closed = signet_dbus_tcp_server_new(&tcp);
  tcp.allow_credentials = true;
  SignetDbusTcpServer *opted_in = signet_dbus_tcp_server_new(&tcp);
  CHECK(signet_dbus_tcp_server_start(closed) == 0);
  CHECK(signet_dbus_tcp_server_start(opted_in) == 0);
  SignetDbusServerConfig local = { .keys = keys, .store = store, .policy = registry,
                                   .uid_resolver = uid_resolve };
  SignetDbusServer *unix_server = signet_dbus_server_new(&local);
  CHECK(signet_dbus_server_start(unix_server) == 0);

  WireFixture fixture = { .loop = g_main_loop_new(NULL, FALSE), .credential_id = cred.id,
    .tcp_default = g_dbus_server_get_client_address(closed->server),
    .tcp_opt_in = g_dbus_server_get_client_address(opted_in->server) };
  guint watch = g_bus_watch_name(G_BUS_TYPE_SESSION, SIGNER, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                 on_unix_ready, NULL, &fixture, NULL);
  g_main_loop_run(fixture.loop);
  g_thread_join(fixture.client);
  g_bus_unwatch_name(watch);
  g_object_unref(fixture.bus);
  g_main_loop_unref(fixture.loop);
  signet_dbus_server_free(unix_server);
  signet_dbus_tcp_server_free(opted_in);
  signet_dbus_tcp_server_free(closed);
  signet_challenge_store_free(challenges);
  signet_secret_metadata_clear(&cred);
  signet_policy_registry_free(registry);
  signet_key_store_free(keys);
  g_test_dbus_down(bus);
}

int main(int argc, char **argv) {
  CHECK(sodium_init() >= 0);
  if (argc == 1) {
    test_authorized();
  } else if (argc == 3 && strcmp(argv[1], "seed") == 0) {
    signet_key_store_free(seed(argv[2]));
  } else {
    CHECK(argc == 4 && strcmp(argv[1], "probe") == 0);
    g_autoptr(GDBusConnection) conn = authenticate(argv[2]);
    g_autoptr(GVariant) pubkey = call(conn, NULL, SIGNER, "GetPublicKey", NULL, NULL);
    CHECK(g_variant_is_of_type(pubkey, G_VARIANT_TYPE("(s)")));
    check_refused(conn, "missing", strcmp(argv[3], "opt-in") == 0 ?
                  "net.signet.Error.CapabilityDenied" : TCP_DENIED);
    CHECK(g_dbus_connection_close_sync(conn, NULL, NULL));
  }
  return 0;
}
