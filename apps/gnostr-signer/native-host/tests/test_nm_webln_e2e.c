/* test_nm_webln_e2e — nostr-signer-webext-host (WebLN) against the real
 * nostr-wallet-agent (org.nostr.Wallet1) on a private GTestDBus bus
 * (nostrc-jjyp).
 *
 * The agent runs headless (no dialogs: anything needing the user is
 * denied) with an in-memory pairing store and no wallet paired, and is told
 * to trust the build-tree host as the browser bridge through its
 * test-build-only NOSTR_WALLET_AGENT_ORIGIN_BRIDGES. The real host binary
 * is then driven with native-messaging frames exactly as the browser would:
 *
 *   webln.status (no origin) -> {available: true, paired: false}
 *   enable / getInfo / getBalance / makeInvoice -> not_paired, i.e. the
 *     agent accepted the host's origin assertion (a refused assertion
 *     would be "rejected") and ran the call as the page origin — the
 *     agent's own log names "https://shop.example" / "http://localhost:5173"
 *     as the failing principal;
 *   sendPayment with an (expired) spec invoice -> the agent's InvalidArgs
 *     text passed through; host-side validation (too_large, bad invoice,
 *     unsupported methods, origin_denied) never reaches the agent;
 *   the same *For methods called directly by this test process (not the
 *     bridge) -> org.nostr.Wallet1.Error.Denied, malformed origin ->
 *     InvalidArgs, plain GetInfo unaffected;
 *   agent stopped -> status available:false, calls wallet_unavailable;
 *   stdin closed with a request in flight -> still answered, clean exit.
 *
 * A paired wallet (budgets, payments, invoices) is covered by
 * test_nm_webln_paired_e2e.c.
 */
#include "native_messaging.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef NMH_AGENT_PATH
#error "NMH_AGENT_PATH must point at nostr-wallet-agent"
#endif
#ifndef NMH_HOST_PATH
#error "NMH_HOST_PATH must point at nostr-signer-webext-host"
#endif

/* BOLT-11 spec vector ("1 cup coffee", 2017): valid, long expired. */
#define COFFEE "lnbc2500u1pvjluezsp5zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygspp5qqqsyqcyq5rqwzqf" \
               "qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqdq5xysxxatsyp3k7enxv4jsxqzpu9qrsgquk0rl77nj30yxdy8j9vdx" \
               "85fkpmdla2087ne0xh8nhedh8w27kyke0lp53ut353s06fv3qfegext0eh0ymjpf39tuven09sam30g4vgpfna3rh"

static void fail_dump(void);
#define CHECK(c) do { if (!(c)) { g_printerr("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fail_dump(); exit(1); } } while (0)

typedef struct {
  GTestDBus *tbus;
  GSubprocess *agent;
  GSubprocess *host;
  GOutputStream *to_host;
  GInputStream *from_host;
  GDBusConnection *bus;
  gchar *tmpdir;
  gchar *agent_log;
} E2E;

static const gchar *agent_log_path;

/* On failure show what the agent saw (caller identification, gate). */
static void fail_dump(void) {
  g_autofree gchar *log = NULL;
  if (agent_log_path && g_file_get_contents(agent_log_path, &log, NULL, NULL))
    g_printerr("---- nostr-wallet-agent log ----\n%s---- end ----\n", log);
}

static void on_name(GDBusConnection *c, const gchar *n, const gchar *o, gpointer ud) {
  *(gboolean *)ud = TRUE;
}

static void wait_for_name(GDBusConnection *bus, const gchar *name) {
  gboolean up = FALSE;
  guint w = g_bus_watch_name_on_connection(bus, name, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                           on_name, NULL, &up, NULL);
  gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (!up && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
  g_bus_unwatch_name(w);
  CHECK(up);
}

static void send_frame(E2E *e, const gchar *json) {
  g_autoptr(GBytes) f = nm_frame_encode(json, strlen(json));
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(f, &n);
  CHECK(g_output_stream_write_all(e->to_host, d, n, NULL, NULL, NULL));
  CHECK(g_output_stream_flush(e->to_host, NULL, NULL));
}

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
  g_print("  <- %s\n", buf);
  return root;
}

static JsonNode *call(E2E *e, const gchar *json) {
  g_print("  -> %s\n", json);
  send_frame(e, json);
  return recv_frame(e);
}

static JsonObject *result_obj(JsonNode *n) {
  JsonObject *o = json_node_get_object(n);
  CHECK(json_object_has_member(o, "result"));
  return json_object_get_object_member(o, "result");
}

static void assert_error(JsonNode *n, const gchar *id, const gchar *code, const gchar *msg_part) {
  JsonObject *o = json_node_get_object(n);
  CHECK(g_strcmp0(json_object_get_string_member(o, "id"), id) == 0);
  CHECK(json_object_has_member(o, "error"));
  JsonObject *err = json_object_get_object_member(o, "error");
  const gchar *got = json_object_get_string_member(err, "code");
  if (g_strcmp0(got, code) != 0) {
    g_printerr("id %s: want error %s, got %s\n", id, code, got);
    fail_dump();
    exit(1);
  }
  const gchar *msg = json_object_get_string_member(err, "message");
  CHECK(msg && *msg);
  if (msg_part && !strstr(msg, msg_part)) {
    g_printerr("id %s: message \"%s\" lacks \"%s\"\n", id, msg, msg_part);
    exit(1);
  }
}

static void expect_error(E2E *e, const gchar *id, const gchar *frame, const gchar *code,
                         const gchar *msg_part) {
  JsonNode *r = call(e, frame);
  assert_error(r, id, code, msg_part);
  json_node_unref(r);
}

static gboolean log_contains(E2E *e, const gchar *needle) {
  g_autofree gchar *log = NULL;
  return g_file_get_contents(e->agent_log, &log, NULL, NULL) && strstr(log, needle);
}

static GError *direct_call(E2E *e, const gchar *method, GVariant *args, GVariant **out) {
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_sync(e->bus, "org.nostr.Wallet1", "/org/nostr/Wallet1",
                                            "org.nostr.Wallet1", method, args, NULL,
                                            G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
  if (out) *out = r;
  else if (r) g_variant_unref(r);
  return err;
}

static void setup(E2E *e) {
  memset(e, 0, sizeof *e);
  e->tmpdir = g_dir_make_tmp("nmh_webln_XXXXXX", NULL);
  CHECK(e->tmpdir);
  static const gchar *const sub[] = { "config", "data", "state", "runtime", "cache" };
  static const gchar *const var[] = { "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
                                      "XDG_RUNTIME_DIR", "XDG_CACHE_HOME" };
  for (gsize i = 0; i < G_N_ELEMENTS(sub); i++) {
    g_autofree gchar *d = g_build_filename(e->tmpdir, sub[i], NULL);
    g_mkdir_with_parents(d, 0700);
    g_setenv(var[i], d, TRUE);
  }
  g_setenv("HOME", e->tmpdir, TRUE);
  g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
  g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
  g_unsetenv("DISPLAY");
  g_unsetenv("WAYLAND_DISPLAY");

  e->tbus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(e->tbus);
  e->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  CHECK(e->bus);

  /* The agent compares the bridge path with readlink(/proc/<pid>/exe),
   * which is fully resolved. */
  g_autofree gchar *host_real = realpath(NMH_HOST_PATH, NULL);
  CHECK(host_real);
  e->agent_log = g_build_filename(e->tmpdir, "agent.log", NULL);
  agent_log_path = e->agent_log;

  g_autoptr(GError) err = NULL;
  g_autoptr(GSubprocessLauncher) al = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
  g_subprocess_launcher_set_stderr_file_path(al, e->agent_log);
  g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_EPHEMERAL", "1", TRUE);
  g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_HEADLESS", "1", TRUE);
  g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_ORIGIN_BRIDGES", host_real, TRUE);
  g_subprocess_launcher_setenv(al, "G_MESSAGES_DEBUG", "all", TRUE);
  e->agent = g_subprocess_launcher_spawn(al, &err, NMH_AGENT_PATH, "--gapplication-service", NULL);
  CHECK(e->agent);
  wait_for_name(e->bus, "org.nostr.Wallet1");

  g_autoptr(GSubprocessLauncher) hl = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_setenv(hl, "NOSTR_SIGNER_BRIDGE_DEBUG", "1", TRUE);
  g_subprocess_launcher_setenv(hl, "G_MESSAGES_DEBUG", "all", TRUE);
  e->host = g_subprocess_launcher_spawn(hl, &err, host_real,
                                        "/usr/lib/mozilla/native-messaging-hosts/org.nostr.signer_bridge.json",
                                        "signer-bridge@gnostr.org", NULL);
  CHECK(e->host);
  e->to_host = g_subprocess_get_stdin_pipe(e->host);
  e->from_host = g_subprocess_get_stdout_pipe(e->host);
}

static void stop_agent(E2E *e) {
  if (!e->agent) return;
  g_subprocess_send_signal(e->agent, SIGTERM);
  if (!g_subprocess_wait(e->agent, NULL, NULL)) g_subprocess_force_exit(e->agent);
  g_clear_object(&e->agent);
}

static void teardown(E2E *e) {
  g_output_stream_close(e->to_host, NULL, NULL);
  CHECK(g_subprocess_wait(e->host, NULL, NULL));
  CHECK(g_subprocess_get_if_exited(e->host) && g_subprocess_get_exit_status(e->host) == 0);
  g_object_unref(e->host);
  stop_agent(e);
  g_object_unref(e->bus);
  g_test_dbus_down(e->tbus);
  g_object_unref(e->tbus);
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", e->tmpdir);
  (void)!system(cmd);
  agent_log_path = NULL;
  g_free(e->agent_log);
  g_free(e->tmpdir);
}

int main(void) {
  E2E e;
  setup(&e);
  JsonNode *r;

  /* host.hello advertises the WebLN surface */
  r = call(&e, "{\"id\":\"h\",\"method\":\"host.hello\"}");
  JsonArray *m = json_object_get_array_member(json_object_get_object_member(result_obj(r), "providers"), "webln");
  gboolean has_status = FALSE;
  for (guint i = 0; i < json_array_get_length(m); i++)
    has_status |= g_strcmp0(json_array_get_string_element(m, i), "webln.status") == 0;
  CHECK(has_status);
  json_node_unref(r);
  g_print("ok host.hello lists webln.*\n");

  /* status: no origin needed, never prompts */
  r = call(&e, "{\"id\":\"s\",\"method\":\"webln.status\"}");
  CHECK(json_object_get_boolean_member(result_obj(r), "available"));
  CHECK(!json_object_get_boolean_member(result_obj(r), "paired"));
  json_node_unref(r);
  g_print("ok webln.status -> available, not paired (extension would not inject)\n");

  expect_error(&e, "en", "{\"id\":\"en\",\"method\":\"webln.enable\",\"origin\":\"https://shop.example\"}",
               "not_paired", NULL);
  g_print("ok webln.enable -> not_paired\n");

  /* The agent ran these as the page origin: a refused origin assertion
   * would be "rejected", not "not_paired". */
  expect_error(&e, "gi", "{\"id\":\"gi\",\"method\":\"webln.getInfo\",\"origin\":\"https://shop.example\"}",
               "not_paired", NULL);
  expect_error(&e, "gb", "{\"id\":\"gb\",\"method\":\"webln.getBalance\",\"origin\":\"https://shop.example\"}",
               "not_paired", NULL);
  expect_error(&e, "mi", "{\"id\":\"mi\",\"method\":\"webln.makeInvoice\",\"origin\":\"https://shop.example\","
                         "\"params\":{\"amount\":21,\"defaultMemo\":\"coffee\"}}", "not_paired", NULL);
  expect_error(&e, "ml", "{\"id\":\"ml\",\"method\":\"webln.makeInvoice\",\"origin\":\"http://localhost:5173\","
                         "\"params\":{\"amount\":\"1000\"}}", "not_paired", NULL);
  CHECK(log_contains(&e, "GetBalance from shop.example (https://shop.example) failed: no wallet is paired"));
  CHECK(log_contains(&e, "MakeInvoice from shop.example (https://shop.example) failed"));
  CHECK(log_contains(&e, "MakeInvoice from localhost:5173 (http://localhost:5173) failed"));
  g_print("ok getInfo/getBalance/makeInvoice -> not_paired, principal = page origin (agent log)\n");

  /* agent-side validation surfaces with its message */
  expect_error(&e, "sp", "{\"id\":\"sp\",\"method\":\"webln.sendPayment\",\"origin\":\"https://shop.example\","
                         "\"params\":{\"paymentRequest\":\"" COFFEE "\"}}", "invalid_request", "invoice has expired");
  g_print("ok sendPayment(expired invoice) -> invalid_request \"invoice has expired\" (from the agent)\n");

  /* host-side validation: never reaches the agent */
  expect_error(&e, "big", "{\"id\":\"big\",\"method\":\"webln.makeInvoice\",\"origin\":\"https://shop.example\","
                          "\"params\":{\"amount\":4294968}}", "too_large", "4294967");
  expect_error(&e, "amt", "{\"id\":\"amt\",\"method\":\"webln.makeInvoice\",\"origin\":\"https://shop.example\","
                          "\"params\":{\"amount\":1.5}}", "invalid_request", NULL);
  expect_error(&e, "bad", "{\"id\":\"bad\",\"method\":\"webln.sendPayment\",\"origin\":\"https://shop.example\","
                          "\"params\":{\"paymentRequest\":\"lnurl1dp68gurn8ghj7um9wfmxjcm99e3k7mf0\"}}",
               "invalid_request", NULL);
  expect_error(&e, "ks", "{\"id\":\"ks\",\"method\":\"webln.keysend\",\"origin\":\"https://shop.example\","
                         "\"params\":{\"destination\":\"02\",\"amount\":1}}", "unsupported", NULL);
  expect_error(&e, "sm", "{\"id\":\"sm\",\"method\":\"webln.signMessage\",\"origin\":\"https://shop.example\"}",
               "unsupported", NULL);
  expect_error(&e, "od", "{\"id\":\"od\",\"method\":\"webln.getInfo\",\"origin\":\"http://evil.example\"}",
               "origin_denied", NULL);
  expect_error(&e, "no", "{\"id\":\"no\",\"method\":\"webln.sendPayment\"}", "origin_denied", NULL);
  g_print("ok host-side validation (too_large, amounts, invoice shape, unsupported, origin)\n");

  /* Any other caller asserting an origin is refused by the agent. */
  g_autoptr(GError) d1 = direct_call(&e, "GetInfoFor", g_variant_new("(s)", "https://shop.example"), NULL);
  CHECK(d1);
  g_autofree gchar *d1n = g_dbus_error_get_remote_error(d1);
  CHECK(g_strcmp0(d1n, "org.nostr.Wallet1.Error.Denied") == 0);
  g_autoptr(GError) d2 = direct_call(&e, "PayInvoiceFor",
                                     g_variant_new("(ssu)", "https://shop.example", COFFEE, (guint32)0), NULL);
  CHECK(d2);
  g_autofree gchar *d2n = g_dbus_error_get_remote_error(d2);
  /* expired invoice is rejected before identification, which is fine: no
   * information about the site leaks either way */
  CHECK(g_strcmp0(d2n, "org.nostr.Wallet1.Error.InvalidArgs") == 0);
  g_autoptr(GError) d3 = direct_call(&e, "MakeInvoiceFor",
                                     g_variant_new("(susu)", "https://shop.example", (guint32)1000, "", (guint32)0), NULL);
  CHECK(d3);
  g_autofree gchar *d3n = g_dbus_error_get_remote_error(d3);
  CHECK(g_strcmp0(d3n, "org.nostr.Wallet1.Error.Denied") == 0);
  CHECK(log_contains(&e, "only the browser bridge may act for a web origin"));
  g_autoptr(GError) d4 = direct_call(&e, "GetBalanceFor", g_variant_new("(s)", "exe:/usr/bin/impostor"), NULL);
  CHECK(d4);
  g_autofree gchar *d4n = g_dbus_error_get_remote_error(d4);
  CHECK(g_strcmp0(d4n, "org.nostr.Wallet1.Error.InvalidArgs") == 0);
  GVariant *plain = NULL;
  g_autoptr(GError) d5 = direct_call(&e, "GetInfo", NULL, &plain);
  CHECK(!d5 && plain);
  gboolean paired = TRUE;
  g_autoptr(GVariant) dict = g_variant_get_child_value(plain, 0);
  CHECK(g_variant_lookup(dict, "paired", "b", &paired) && !paired);
  g_variant_unref(plain);
  g_print("ok non-bridge caller: *For -> Denied, bad origin -> InvalidArgs, plain GetInfo unaffected\n");

  /* agent gone */
  stop_agent(&e);
  r = call(&e, "{\"id\":\"s2\",\"method\":\"webln.status\"}");
  CHECK(!json_object_get_boolean_member(result_obj(r), "available"));
  CHECK(g_strcmp0(json_object_get_string_member(result_obj(r), "reason"), "wallet_unavailable") == 0);
  json_node_unref(r);
  expect_error(&e, "gone", "{\"id\":\"gone\",\"method\":\"webln.getInfo\",\"origin\":\"https://shop.example\"}",
               "wallet_unavailable", NULL);
  g_print("ok agent stopped -> status available:false, wallet_unavailable\n");

  /* stdin closed right after a request (scripted client): the host still
   * answers what is in flight before exiting */
  send_frame(&e, "{\"id\":\"last\",\"method\":\"webln.getBalance\",\"origin\":\"https://shop.example\"}");
  CHECK(g_output_stream_close(e.to_host, NULL, NULL));
  r = recv_frame(&e);
  assert_error(r, "last", "wallet_unavailable", NULL);
  json_node_unref(r);
  g_print("ok stdin EOF with a request in flight -> answered, then exit\n");

  teardown(&e);
  g_print("PASS nostr-signer-webext-host webln e2e\n");
  return 0;
}
