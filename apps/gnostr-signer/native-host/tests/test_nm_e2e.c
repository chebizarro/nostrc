/* test_nm_e2e — nostr-signer-webext-host against the real org.nostr.Signer
 * daemon (nostrc-jjyp).
 *
 * Mirrors nips/nip55l/tests/test_signer_dbus_contract.c: a private
 * GTestDBus session bus, the actual nostr-signer-daemon on it with a
 * hermetic identity ($NOSTR_SIGNER_SECKEY_HEX, BIP-340 vector 0: sk = 3)
 * and a pre-seeded ACL, then the real host binary spawned with pipes and
 * driven with native-messaging frames exactly as a browser would:
 *
 *   host.hello, getPublicKey (npub -> hex), signEvent via the ACL
 *   (allowed / denied origins; NIP-01 id recomputed here), signEvent via
 *   the live ApprovalRequested -> ApproveRequest flow with the page origin
 *   as app_id (approve + deny), out-of-order replies while an approval is
 *   pending, approval timeout, getRelays (configured / NotFound -> {}),
 *   nip04 + nip44 encrypt/decrypt round-trips, daemon error mapping, host-
 *   side validation, and signer_unavailable once the daemon is gone.
 *
 * The host runs with NOSTR_SIGNER_BRIDGE_DEBUG=1 so GLib debug output is
 * produced throughout; every frame still parsing proves fd 1 isolation.
 */
#include "native_messaging.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#ifndef NMH_DAEMON_PATH
#error "NMH_DAEMON_PATH must point at nostr-signer-daemon"
#endif
#ifndef NMH_HOST_PATH
#error "NMH_HOST_PATH must point at nostr-signer-webext-host"
#endif

#define SK_HEX "0000000000000000000000000000000000000000000000000000000000000003"
#define PK_HEX "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9"

#define CHECK(c) do { if (!(c)) { g_printerr("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
  GTestDBus *tbus;
  GSubprocess *daemon;
  GSubprocess *host;
  GOutputStream *to_host;
  GInputStream *from_host;
  GDBusConnection *bus;
  gchar *tmpdir;
  gchar *relays_path;
  /* approval signal capture */
  gchar *last_app_id;
  gchar *last_request_id;
} E2E;

static void write_file(const gchar *path, const gchar *body) {
  CHECK(g_file_set_contents(path, body, -1, NULL));
  CHECK(g_chmod(path, 0600) == 0);
}

static void on_name(GDBusConnection *c, const gchar *n, const gchar *o, gpointer ud) {
  *(gboolean *)ud = TRUE;
}

static void wait_for_signer(GDBusConnection *bus) {
  gboolean up = FALSE;
  guint w = g_bus_watch_name_on_connection(bus, "org.nostr.Signer", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                           on_name, NULL, &up, NULL);
  gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (!up && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
  g_bus_unwatch_name(w);
  CHECK(up);
}

static void on_approval_requested(GDBusConnection *c, const gchar *sender, const gchar *path,
                                  const gchar *iface, const gchar *signal, GVariant *params,
                                  gpointer ud) {
  E2E *e = ud;
  const gchar *app_id, *identity, *kind, *preview, *req_id;
  g_variant_get(params, "(&s&s&s&s&s)", &app_id, &identity, &kind, &preview, &req_id);
  g_free(e->last_app_id);
  g_free(e->last_request_id);
  e->last_app_id = g_strdup(app_id);
  e->last_request_id = g_strdup(req_id);
}

static void send_frame(E2E *e, const gchar *json) {
  g_autoptr(GBytes) f = nm_frame_encode(json, strlen(json));
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(f, &n);
  CHECK(g_output_stream_write_all(e->to_host, d, n, NULL, NULL, NULL));
  CHECK(g_output_stream_flush(e->to_host, NULL, NULL));
}

/* Blocking read of one reply frame; returns the parsed object (owned by
 * the returned node). */
static JsonNode *recv_frame(E2E *e) {
  guint8 prefix[4];
  gsize got = 0;
  CHECK(g_input_stream_read_all(e->from_host, prefix, 4, &got, NULL, NULL) && got == 4);
  guint32 len = nm_frame_decode_length(prefix);
  CHECK(len > 0 && len <= NM_MAX_MESSAGE_SIZE);
  g_autofree gchar *buf = g_malloc0(len + 1);
  CHECK(g_input_stream_read_all(e->from_host, buf, len, &got, NULL, NULL) && got == len);
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, buf, len, NULL)) {
    g_printerr("unparseable frame from host: %s\n", buf);
    exit(1);
  }
  JsonNode *root = json_node_ref(json_parser_get_root(p));
  CHECK(JSON_NODE_HOLDS_OBJECT(root));
  if (g_getenv("NMH_E2E_VERBOSE")) g_printerr("<- %s\n", buf);
  return root;
}

static JsonNode *call(E2E *e, const gchar *json) {
  if (g_getenv("NMH_E2E_VERBOSE")) g_printerr("-> %s\n", json);
  send_frame(e, json);
  return recv_frame(e);
}

static const gchar *reply_id(JsonNode *n) {
  return json_object_get_string_member(json_node_get_object(n), "id");
}

static JsonNode *result_of(JsonNode *n) {
  JsonObject *o = json_node_get_object(n);
  if (!json_object_has_member(o, "result")) {
    g_autoptr(JsonGenerator) g = json_generator_new();
    json_generator_set_root(g, n);
    g_autofree gchar *s = json_generator_to_data(g, NULL);
    g_printerr("expected result, got %s\n", s);
    exit(1);
  }
  return json_object_get_member(o, "result");
}

static const gchar *error_code_of(JsonNode *n) {
  JsonObject *o = json_node_get_object(n);
  CHECK(json_object_has_member(o, "error"));
  return json_object_get_string_member(json_object_get_object_member(o, "error"), "code");
}

static void assert_error(JsonNode *n, const gchar *id, const gchar *code) {
  CHECK(g_strcmp0(reply_id(n), id) == 0);
  if (g_strcmp0(error_code_of(n), code) != 0) {
    g_printerr("id %s: want error %s, got %s\n", id, code, error_code_of(n));
    exit(1);
  }
}

/* NIP-01: id = sha256([0,pubkey,created_at,kind,tags,content]). Only used
 * with ASCII content that needs no escaping beyond json-glib's output. */
static void assert_valid_signed(JsonNode *res, gint64 kind, gint64 created_at, const gchar *content) {
  CHECK(JSON_NODE_HOLDS_OBJECT(res));
  JsonObject *ev = json_node_get_object(res);
  CHECK(g_strcmp0(json_object_get_string_member(ev, "pubkey"), PK_HEX) == 0);
  CHECK(json_object_get_int_member(ev, "kind") == kind);
  CHECK(json_object_get_int_member(ev, "created_at") == created_at);
  CHECK(g_strcmp0(json_object_get_string_member(ev, "content"), content) == 0);
  CHECK(strlen(json_object_get_string_member(ev, "sig")) == 128);

  JsonArray *arr = json_array_new();
  json_array_add_int_element(arr, 0);
  json_array_add_string_element(arr, PK_HEX);
  json_array_add_int_element(arr, created_at);
  json_array_add_int_element(arr, kind);
  json_array_add_element(arr, json_node_copy(json_object_get_member(ev, "tags")));
  json_array_add_string_element(arr, content);
  JsonNode *an = json_node_new(JSON_NODE_ARRAY);
  json_node_take_array(an, arr);
  g_autoptr(JsonGenerator) g = json_generator_new();
  json_generator_set_root(g, an);
  g_autofree gchar *ser = json_generator_to_data(g, NULL);
  json_node_unref(an);
  g_autofree gchar *id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, ser, -1);
  CHECK(g_strcmp0(json_object_get_string_member(ev, "id"), id) == 0);
}

static void wait_for_approval(E2E *e) {
  gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!e->last_request_id && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, TRUE);
  CHECK(e->last_request_id != NULL);
}

static void approve(E2E *e, gboolean decision) {
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
      e->bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "ApproveRequest",
      g_variant_new("(sbbt)", e->last_request_id, decision, FALSE, (guint64)0),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
  CHECK(r != NULL);
  g_clear_pointer(&e->last_request_id, g_free);
}

static void setup(E2E *e) {
  memset(e, 0, sizeof *e);
  e->tmpdir = g_dir_make_tmp("nmh_e2e_XXXXXX", NULL);
  CHECK(e->tmpdir);
  g_autofree gchar *cfg = g_build_filename(e->tmpdir, "config", NULL);
  g_autofree gchar *data = g_build_filename(e->tmpdir, "data", NULL);
  g_autofree gchar *rt = g_build_filename(e->tmpdir, "runtime", NULL);
  g_autofree gchar *gdir = g_build_filename(cfg, "gnostr", NULL);
  g_autofree gchar *ndir = g_build_filename(cfg, "nostr", NULL);
  g_mkdir_with_parents(gdir, 0700);
  g_mkdir_with_parents(ndir, 0700);
  g_mkdir_with_parents(data, 0700);
  g_mkdir_with_parents(rt, 0700);
  g_setenv("XDG_CONFIG_HOME", cfg, TRUE);
  g_setenv("XDG_DATA_HOME", data, TRUE);
  g_setenv("XDG_RUNTIME_DIR", rt, TRUE);
  g_setenv("HOME", e->tmpdir, TRUE);
  g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
  g_unsetenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", SK_HEX, TRUE);

  /* nip55l 0.4.0 grants: [kind] "<principal>|<npub or *>". Web origins are
   * principals only when asserted by the trusted bridge (this build-tree
   * host, via the test-build NOSTR_SIGNER_TEST_ORIGIN_BRIDGES); the host
   * calls GetPublicKey/GetRelays/NIP-04/44 without an app_id, so those run
   * as the host itself (exe:<path>). */
  g_autofree gchar *host_real = realpath(NMH_HOST_PATH, NULL);
  CHECK(host_real);
  g_autofree gchar *self_real = g_file_read_link("/proc/self/exe", NULL);
  CHECK(self_real);
  g_setenv("NOSTR_SIGNER_TEST_ORIGIN_BRIDGES", host_real, TRUE);
  g_setenv("NOSTR_SIGNER_TEST_APPROVERS", self_real, TRUE);
  g_autofree gchar *grants = g_build_filename(gdir, "signer-grants.ini", NULL);
  g_autofree gchar *body = g_strdup_printf(
      "[event]\nhttps://allowed.example|*=allow\nhttps://denied.example|*=deny\n"
      "[get_public_key]\nexe:%1$s|*=allow\n[get_relays]\nexe:%1$s|*=allow\n"
      "[nip04_encrypt]\nexe:%1$s|*=allow\n[nip04_decrypt]\nexe:%1$s|*=allow\n"
      "[nip44_encrypt]\nexe:%1$s|*=allow\n[nip44_decrypt]\nexe:%1$s|*=allow\n", host_real);
  write_file(grants, body);
  e->relays_path = g_build_filename(ndir, "relays.conf", NULL);
  write_file(e->relays_path, "[\"wss://relay.example\", \"wss://nos.lol\"]\n");

  e->tbus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(e->tbus);

  g_autoptr(GError) err = NULL;
  e->daemon = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                               &err, NMH_DAEMON_PATH, NULL);
  CHECK(e->daemon);
  e->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  CHECK(e->bus);
  wait_for_signer(e->bus);
  /* An approval agent must be on the bus for prompts (nip55l 0.4.0). */
  g_autoptr(GVariant) rn = g_dbus_connection_call_sync(e->bus, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.gnostr.Signer", 4u), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
  CHECK(rn);

  g_dbus_connection_signal_subscribe(e->bus, "org.nostr.Signer", "org.nostr.Signer",
                                     "ApprovalRequested", "/org/nostr/signer", NULL,
                                     G_DBUS_SIGNAL_FLAGS_NONE, on_approval_requested, e, NULL);

  /* The host must not see the signer key; it only needs the bus. */
  g_autoptr(GSubprocessLauncher) l = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_unsetenv(l, "NOSTR_SIGNER_SECKEY_HEX");
  g_subprocess_launcher_setenv(l, "NOSTR_SIGNER_BRIDGE_DEBUG", "1", TRUE);
  g_subprocess_launcher_setenv(l, "G_MESSAGES_DEBUG", "all", TRUE);
  g_subprocess_launcher_setenv(l, "NOSTR_SIGNER_BRIDGE_APPROVAL_TIMEOUT_MS", "4000", TRUE);
  /* Browser-style argv: Firefox passes manifest path + extension id. */
  e->host = g_subprocess_launcher_spawn(l, &err, NMH_HOST_PATH,
                                        "/usr/lib/mozilla/native-messaging-hosts/org.nostr.signer_bridge.json",
                                        "signer-bridge@gnostr.org", NULL);
  CHECK(e->host);
  e->to_host = g_subprocess_get_stdin_pipe(e->host);
  e->from_host = g_subprocess_get_stdout_pipe(e->host);
}

static void teardown(E2E *e) {
  g_output_stream_close(e->to_host, NULL, NULL);
  CHECK(g_subprocess_wait(e->host, NULL, NULL));
  CHECK(g_subprocess_get_if_exited(e->host) && g_subprocess_get_exit_status(e->host) == 0);
  g_object_unref(e->host);
  if (e->daemon) {
    g_subprocess_force_exit(e->daemon);
    g_subprocess_wait(e->daemon, NULL, NULL);
    g_object_unref(e->daemon);
  }
  g_object_unref(e->bus);
  g_test_dbus_down(e->tbus);
  g_object_unref(e->tbus);
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", e->tmpdir);
  int rc = system(cmd);
  (void)rc;
  g_free(e->tmpdir);
  g_free(e->relays_path);
  g_free(e->last_app_id);
  g_free(e->last_request_id);
}

int main(void) {
  E2E e;
  setup(&e);
  JsonNode *r;

  /* hello */
  r = call(&e, "{\"id\":\"h\",\"method\":\"host.hello\"}");
  CHECK(g_strcmp0(json_object_get_string_member(json_node_get_object(result_of(r)), "host"),
                  "org.nostr.signer_bridge") == 0);
  json_node_unref(r);

  /* getPublicKey: daemon npub -> NIP-07 hex */
  r = call(&e, "{\"id\":\"pk\",\"method\":\"getPublicKey\",\"origin\":\"https://allowed.example\"}");
  CHECK(g_strcmp0(json_node_get_string(result_of(r)), PK_HEX) == 0);
  json_node_unref(r);
  g_print("ok getPublicKey -> %s\n", PK_HEX);

  /* signEvent, ACL allow: extra/forged fields are dropped by the host */
  r = call(&e, "{\"id\":\"s1\",\"method\":\"signEvent\",\"origin\":\"https://allowed.example\","
               "\"params\":{\"event\":{\"kind\":1,\"created_at\":1700000000,\"tags\":[[\"t\",\"nip07\"]],"
               "\"content\":\"hello from the bridge\",\"id\":\"forged\",\"sig\":\"forged\",\"x\":1}}}");
  CHECK(g_strcmp0(reply_id(r), "s1") == 0);
  assert_valid_signed(result_of(r), 1, 1700000000, "hello from the bridge");
  json_node_unref(r);
  g_print("ok signEvent (ACL allow, id recomputed)\n");

  /* signEvent, ACL deny */
  r = call(&e, "{\"id\":\"s2\",\"method\":\"signEvent\",\"origin\":\"https://denied.example\","
               "\"params\":{\"event\":{\"kind\":1,\"created_at\":1,\"tags\":[],\"content\":\"no\"}}}");
  assert_error(r, "s2", "rejected");
  json_node_unref(r);
  g_print("ok signEvent (ACL deny -> rejected)\n");

  /* signEvent, interactive approve — and a getPublicKey answered while the
   * approval is pending (out-of-order correlation). */
  send_frame(&e, "{\"id\":\"s3\",\"method\":\"signEvent\",\"origin\":\"https://prompt.example\","
                 "\"params\":{\"event\":{\"kind\":7,\"created_at\":1700000001,\"tags\":[[\"e\",\"" PK_HEX "\"]],"
                 "\"content\":\"+\"}}}");
  wait_for_approval(&e);
  CHECK(g_strcmp0(e.last_app_id, "https://prompt.example") == 0);
  r = call(&e, "{\"id\":\"pk2\",\"method\":\"getPublicKey\",\"origin\":\"https://other.example\"}");
  CHECK(g_strcmp0(reply_id(r), "pk2") == 0);
  json_node_unref(r);
  approve(&e, TRUE);
  r = recv_frame(&e);
  CHECK(g_strcmp0(reply_id(r), "s3") == 0);
  assert_valid_signed(result_of(r), 7, 1700000001, "+");
  json_node_unref(r);
  g_print("ok signEvent (ApprovalRequested app_id=https://prompt.example, approved; pk2 answered first)\n");

  g_usleep(200 * 1000); /* daemon rate-limits un-ACL'd SignEvent per sender */

  /* signEvent, interactive deny */
  send_frame(&e, "{\"id\":\"s4\",\"method\":\"signEvent\",\"origin\":\"http://localhost:5173\","
                 "\"params\":{\"event\":{\"kind\":1,\"tags\":[],\"content\":\"deny me\"}}}");
  wait_for_approval(&e);
  CHECK(g_strcmp0(e.last_app_id, "http://localhost:5173") == 0);
  approve(&e, FALSE);
  r = recv_frame(&e);
  assert_error(r, "s4", "rejected");
  json_node_unref(r);
  g_print("ok signEvent (localhost origin, user denied -> rejected)\n");

  g_usleep(200 * 1000);

  /* signEvent, nobody answers -> timeout (host approval timeout 4 s) */
  r = call(&e, "{\"id\":\"s5\",\"method\":\"signEvent\",\"origin\":\"https://slow.example\","
               "\"params\":{\"event\":{\"kind\":1,\"tags\":[],\"content\":\"ignored\"}}}");
  assert_error(r, "s5", "timeout");
  json_node_unref(r);
  g_clear_pointer(&e.last_request_id, g_free);
  g_print("ok signEvent (no approval -> timeout)\n");

  /* host-side validation and origin policy never reach the daemon */
  r = call(&e, "{\"id\":\"v1\",\"method\":\"signEvent\",\"origin\":\"https://allowed.example\","
               "\"params\":{\"event\":{\"kind\":1,\"tags\":[[\"p\",5]],\"content\":\"\"}}}");
  assert_error(r, "v1", "invalid_request");
  json_node_unref(r);
  r = call(&e, "{\"id\":\"v2\",\"method\":\"signEvent\",\"origin\":\"http://allowed.example\","
               "\"params\":{\"event\":{\"kind\":1,\"tags\":[],\"content\":\"\"}}}");
  assert_error(r, "v2", "origin_denied");
  json_node_unref(r);
  CHECK(e.last_request_id == NULL);
  g_print("ok malformed event -> invalid_request, http origin -> origin_denied\n");

  /* getRelays: configured list -> NIP-07 shape */
  r = call(&e, "{\"id\":\"r1\",\"method\":\"getRelays\",\"origin\":\"https://allowed.example\"}");
  JsonObject *relays = json_node_get_object(result_of(r));
  CHECK(json_object_has_member(relays, "wss://relay.example"));
  CHECK(json_object_get_boolean_member(json_object_get_object_member(relays, "wss://nos.lol"), "write"));
  json_node_unref(r);
  /* ... and Error.NotFound -> {} */
  CHECK(g_unlink(e.relays_path) == 0);
  r = call(&e, "{\"id\":\"r2\",\"method\":\"getRelays\",\"origin\":\"https://allowed.example\"}");
  CHECK(json_object_get_size(json_node_get_object(result_of(r))) == 0);
  json_node_unref(r);
  g_print("ok getRelays (list -> {url:{read,write}}, NotFound -> {})\n");

  /* NIP-44 and NIP-04 round-trips to self */
  const gchar *schemes[] = { "nip44", "nip04" };
  for (gsize i = 0; i < G_N_ELEMENTS(schemes); i++) {
    g_autofree gchar *enc = g_strdup_printf(
        "{\"id\":\"%s-e\",\"method\":\"%s.encrypt\",\"origin\":\"https://allowed.example\","
        "\"params\":{\"pubkey\":\"%s\",\"plaintext\":\"secret \\u00e9 message\"}}",
        schemes[i], schemes[i], "F9308A019258C31049344F85F89D5229B531C845836F99B08601F113BCE036F9");
    r = call(&e, enc);
    const gchar *ct = json_node_get_string(result_of(r));
    CHECK(ct && *ct);
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "pubkey", PK_HEX);
    json_object_set_string_member(params, "ciphertext", ct);
    JsonObject *req = json_object_new();
    json_object_set_string_member(req, "id", "dec");
    g_autofree gchar *method = g_strdup_printf("%s.decrypt", schemes[i]);
    json_object_set_string_member(req, "method", method);
    json_object_set_string_member(req, "origin", "https://allowed.example");
    json_object_set_object_member(req, "params", params);
    JsonNode *rn = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(rn, req);
    g_autoptr(JsonGenerator) g = json_generator_new();
    json_generator_set_root(g, rn);
    g_autofree gchar *dec = json_generator_to_data(g, NULL);
    json_node_unref(rn);
    json_node_unref(r);
    r = call(&e, dec);
    CHECK(g_strcmp0(json_node_get_string(result_of(r)), "secret \xc3\xa9 message") == 0);
    json_node_unref(r);
    g_print("ok %s.encrypt/decrypt round-trip\n", schemes[i]);
  }

  /* daemon error -> bridge error: undecryptable payload */
  r = call(&e, "{\"id\":\"d1\",\"method\":\"nip44.decrypt\",\"origin\":\"https://allowed.example\","
               "\"params\":{\"pubkey\":\"" PK_HEX "\",\"ciphertext\":\"not-a-payload\"}}");
  assert_error(r, "d1", "internal");
  json_node_unref(r);
  g_print("ok nip44.decrypt garbage -> Error.Internal -> internal\n");

  /* WebLN with no wallet agent on this bus (test_nm_webln_e2e covers the
   * agent): wallet_unavailable, and status says "don't inject". */
  r = call(&e, "{\"id\":\"w\",\"method\":\"webln.getInfo\",\"origin\":\"https://allowed.example\"}");
  assert_error(r, "w", "wallet_unavailable");
  json_node_unref(r);
  r = call(&e, "{\"id\":\"ws\",\"method\":\"webln.status\"}");
  CHECK(!json_object_get_boolean_member(json_node_get_object(result_of(r)), "available"));
  json_node_unref(r);
  g_print("ok webln without an agent -> wallet_unavailable / available:false\n");

  /* signer gone -> signer_unavailable */
  g_subprocess_force_exit(e.daemon);
  g_subprocess_wait(e.daemon, NULL, NULL);
  g_clear_object(&e.daemon);
  r = call(&e, "{\"id\":\"gone\",\"method\":\"getPublicKey\",\"origin\":\"https://allowed.example\"}");
  assert_error(r, "gone", "signer_unavailable");
  json_node_unref(r);
  g_print("ok daemon stopped -> signer_unavailable\n");

  teardown(&e);
  g_print("PASS nostr-signer-webext-host e2e\n");
  return 0;
}
