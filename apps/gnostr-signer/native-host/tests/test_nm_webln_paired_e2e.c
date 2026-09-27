/* test_nm_webln_paired_e2e — WebLN through the real browser bridge and the
 * real nostr-wallet-agent, PAIRED with a NIP-47 wallet (nostrc-4qbt).
 *
 * Processes, all on a private GTestDBus bus, no network:
 *   nwa-fixture-wallet   wallet service + relay on ws://127.0.0.1:<port>
 *                        (gnome/nostr-wallet-agent/tests), prints the pairing
 *   nostr-wallet-agent   headless, ephemeral, paired through its
 *                        test-build-only NOSTR_WALLET_AGENT_TEST_PAIR_URI, told
 *                        to trust the build-tree host as the browser bridge
 *   nostr-signer-webext-host  driven with native-messaging frames exactly as
 *                        the browser would
 *
 * Per-site state is seeded in the agent's budgets.json before it starts
 * (headless, the agent cannot ask): https://shop.example may read and has a
 * 50 sat/day budget; https://reader.example has neither.
 *
 * Covered: status paired; getInfo mapping (alias, methods); getBalance with a
 * read grant, and without one (headless: rejected); makeInvoice ->
 * {paymentRequest, rHash}; sendPayment of that invoice auto-paid from the
 * site budget -> preimage whose sha256 is rHash, the wallet debited and the
 * spend ledgered under the origin; a payment over the remaining budget ->
 * budget_exceeded + the BudgetExceeded signal naming the origin, never
 * reaching the wallet; a site without a budget -> rejected.
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
#ifndef NMH_FIXTURE_WALLET_PATH
#error "NMH_FIXTURE_WALLET_PATH must point at nwa-fixture-wallet"
#endif

static const gchar *agent_log_path;
static gchar *fixture_out;

static void fail_dump(void) {
  g_autofree gchar *log = NULL;
  if (agent_log_path && g_file_get_contents(agent_log_path, &log, NULL, NULL))
    g_printerr("---- nostr-wallet-agent log ----\n%s---- end ----\n", log);
  if (fixture_out) g_printerr("---- fixture wallet ----\n%s---- end ----\n", fixture_out);
}
#define CHECK(c) do { if (!(c)) { g_printerr("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fail_dump(); exit(1); } } while (0)

typedef struct {
  GTestDBus *tbus;
  GDBusConnection *bus;
  GSubprocess *fixture, *agent, *host;
  GDataInputStream *fx_out;
  GOutputStream *to_host;
  GInputStream *from_host;
  gchar *tmpdir, *agent_log, *budgets;
  GString *fx_lines;
  gchar *budget_app;
  guint64 budget_requested;
} E2E;

/* ---- fixture wallet ---- */

static gchar *fx_read_line(E2E *e) {
  g_autoptr(GError) err = NULL;
  gchar *line = g_data_input_stream_read_line_utf8(e->fx_out, NULL, NULL, &err);
  CHECK(line);
  g_string_append_printf(e->fx_lines, "%s\n", line);
  g_free(fixture_out);
  fixture_out = g_strdup(e->fx_lines->str);
  return line;
}

/* The fixture prints one REQUEST line per wallet request it answered. */
static guint fx_count(E2E *e, const gchar *needle) {
  guint n = 0;
  for (const gchar *p = e->fx_lines->str; (p = strstr(p, needle)); p++) n++;
  return n;
}

/* ---- host frames ---- */

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
  CHECK(json_parser_load_from_data(p, buf, len, NULL));
  g_print("  <- %s\n", buf);
  return json_node_ref(json_parser_get_root(p));
}

static JsonNode *call(E2E *e, const gchar *json) {
  g_print("  -> %s\n", json);
  send_frame(e, json);
  return recv_frame(e);
}

static JsonObject *result_obj(JsonNode *n) {
  JsonObject *o = json_node_get_object(n);
  if (!json_object_has_member(o, "result")) {
    g_autofree gchar *s = json_to_string(n, FALSE);
    g_printerr("expected a result, got %s\n", s);
    fail_dump();
    exit(1);
  }
  return json_object_get_object_member(o, "result");
}

static void expect_error(E2E *e, const gchar *frame, const gchar *code) {
  g_autoptr(JsonNode) r = call(e, frame);
  JsonObject *o = json_node_get_object(r);
  CHECK(json_object_has_member(o, "error"));
  const gchar *got = json_object_get_string_member(json_object_get_object_member(o, "error"), "code");
  if (g_strcmp0(got, code) != 0) {
    g_printerr("want error %s, got %s\n", code, got);
    fail_dump();
    exit(1);
  }
}

static gint64 get_balance_sats(E2E *e, const gchar *origin) {
  g_autofree gchar *f = g_strdup_printf("{\"id\":\"b\",\"method\":\"webln.getBalance\",\"origin\":\"%s\"}", origin);
  g_autoptr(JsonNode) r = call(e, f);
  JsonObject *res = result_obj(r);
  CHECK(g_strcmp0(json_object_get_string_member(res, "currency"), "sats") == 0);
  return json_object_get_int_member(res, "balance");
}

/* makeInvoice through the bridge; returns the invoice, *hash = rHash. */
static gchar *make_invoice(E2E *e, const gchar *origin, guint sats, gchar **hash) {
  g_autofree gchar *f = g_strdup_printf("{\"id\":\"mi\",\"method\":\"webln.makeInvoice\",\"origin\":\"%s\","
                                        "\"params\":{\"amount\":%u,\"defaultMemo\":\"e2e\"}}", origin, sats);
  g_autoptr(JsonNode) r = call(e, f);
  JsonObject *res = result_obj(r);
  *hash = g_strdup(json_object_get_string_member(res, "rHash"));
  CHECK(*hash && strlen(*hash) == 64);
  return g_strdup(json_object_get_string_member(res, "paymentRequest"));
}

static void on_budget_exceeded(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                               const gchar *signal, GVariant *params, gpointer ud) {
  (void)c; (void)sender; (void)path; (void)iface; (void)signal;
  E2E *e = ud;
  guint64 remaining = 0;
  g_free(e->budget_app);
  g_variant_get(params, "(stt)", &e->budget_app, &e->budget_requested, &remaining);
}

/* ---- setup ---- */

static void on_name(GDBusConnection *c, const gchar *n, const gchar *o, gpointer ud) {
  (void)c; (void)n; (void)o;
  *(gboolean *)ud = TRUE;
}

static void wait_for_name(GDBusConnection *bus, const gchar *name) {
  gboolean up = FALSE;
  guint w = g_bus_watch_name_on_connection(bus, name, G_BUS_NAME_WATCHER_FLAGS_NONE, on_name, NULL, &up, NULL);
  gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (!up && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
  g_bus_unwatch_name(w);
  CHECK(up);
}

static void seed_budgets(E2E *e) {
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree gchar *day = g_date_time_format(now, "%Y-%m-%d");
  g_autofree gchar *dir = g_build_filename(g_getenv("XDG_STATE_HOME"), "nostr-wallet", NULL);
  CHECK(g_mkdir_with_parents(dir, 0700) == 0);
  e->budgets = g_build_filename(dir, "budgets.json", NULL);
  g_autofree gchar *doc = g_strdup_printf(
    "{\"version\":1,\"apps\":{\"https://shop.example\":{\"limit_msat_per_day\":50000,\"allow_read\":true,"
    "\"day\":\"%s\",\"spent_msat\":0}}}", day);
  CHECK(g_file_set_contents(e->budgets, doc, -1, NULL));
  CHECK(g_chmod(e->budgets, 0600) == 0);
}

static void setup(E2E *e) {
  memset(e, 0, sizeof *e);
  e->fx_lines = g_string_new(NULL);
  e->tmpdir = g_dir_make_tmp("nmh_webln_paired_XXXXXX", NULL);
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
  seed_budgets(e);

  g_autoptr(GError) err = NULL;
  e->fixture = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                NMH_FIXTURE_WALLET_PATH, "--balance", "100000000", NULL);
  CHECK(e->fixture);
  e->fx_out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(e->fixture));
  g_autofree gchar *uri = NULL;
  for (;;) {
    g_autofree gchar *line = fx_read_line(e);
    if (g_str_has_prefix(line, "URI ")) uri = g_strdup(line + 4);
    if (g_str_equal(line, "READY")) break;
  }
  CHECK(uri);

  e->tbus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(e->tbus);
  e->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  CHECK(e->bus);
  g_dbus_connection_signal_subscribe(e->bus, NULL, "org.nostr.Wallet1", "BudgetExceeded", "/org/nostr/Wallet1",
                                     NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_budget_exceeded, e, NULL);

  g_autofree gchar *host_real = realpath(NMH_HOST_PATH, NULL);
  CHECK(host_real);
  e->agent_log = g_build_filename(e->tmpdir, "agent.log", NULL);
  agent_log_path = e->agent_log;
  g_autoptr(GSubprocessLauncher) al = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
  g_subprocess_launcher_set_stderr_file_path(al, e->agent_log);
  g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_EPHEMERAL", "1", TRUE);
  g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_HEADLESS", "1", TRUE);
  g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_TEST_PAIR_URI", uri, TRUE);
  g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_ORIGIN_BRIDGES", host_real, TRUE);
  g_subprocess_launcher_setenv(al, "G_MESSAGES_DEBUG", "all", TRUE);
  e->agent = g_subprocess_launcher_spawn(al, &err, NMH_AGENT_PATH, "--gapplication-service", NULL);
  CHECK(e->agent);
  wait_for_name(e->bus, "org.nostr.Wallet1");

  g_autoptr(GSubprocessLauncher) hl = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  e->host = g_subprocess_launcher_spawn(hl, &err, host_real,
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
  g_subprocess_send_signal(e->agent, SIGTERM);
  g_subprocess_wait(e->agent, NULL, NULL);
  g_object_unref(e->agent);
  /* Not stdin EOF: later children (the test bus daemon) may hold the pipe. */
  g_subprocess_send_signal(e->fixture, SIGTERM);
  g_subprocess_wait(e->fixture, NULL, NULL);
  g_object_unref(e->fx_out);
  g_object_unref(e->fixture);
  g_object_unref(e->bus);
  g_test_dbus_down(e->tbus);
  g_object_unref(e->tbus);
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", e->tmpdir);
  (void)!system(cmd);
  agent_log_path = NULL;
}

/* Drain fixture stdout lines that arrived so far (REQUEST … lines). */
static void fx_drain(E2E *e, guint want_requests_total) {
  while (fx_count(e, "REQUEST ") < want_requests_total) g_free(fx_read_line(e));
}

int main(void) {
  E2E e;
  setup(&e);
  guint reqs = 0;

  g_autoptr(JsonNode) st = call(&e, "{\"id\":\"s\",\"method\":\"webln.status\"}");
  CHECK(json_object_get_boolean_member(result_obj(st), "available"));
  CHECK(json_object_get_boolean_member(result_obj(st), "paired"));
  g_print("ok webln.status -> paired\n");

  g_autoptr(JsonNode) gi = call(&e, "{\"id\":\"gi\",\"method\":\"webln.getInfo\",\"origin\":\"https://shop.example\"}");
  JsonObject *info = result_obj(gi);
  CHECK(g_strcmp0(json_object_get_string_member(json_object_get_object_member(info, "node"), "alias"),
                  "fixture wallet") == 0);
  JsonArray *methods = json_object_get_array_member(info, "methods");
  gboolean has_send = FALSE, has_make = FALSE;
  for (guint i = 0; i < json_array_get_length(methods); i++) {
    has_send |= g_strcmp0(json_array_get_string_element(methods, i), "sendPayment") == 0;
    has_make |= g_strcmp0(json_array_get_string_element(methods, i), "makeInvoice") == 0;
  }
  CHECK(has_send && has_make);
  fx_drain(&e, reqs += 1);
  CHECK(fx_count(&e, "REQUEST get_info") == 1);
  g_print("ok webln.getInfo -> wallet alias + methods (wallet answered get_info)\n");

  CHECK(get_balance_sats(&e, "https://shop.example") == 100000);
  fx_drain(&e, reqs += 1);
  g_print("ok webln.getBalance (site with a read grant) -> 100000 sats\n");

  expect_error(&e, "{\"id\":\"rb\",\"method\":\"webln.getBalance\",\"origin\":\"https://reader.example\"}", "rejected");
  CHECK(fx_count(&e, "REQUEST get_balance") == 1); /* never reached the wallet */
  g_print("ok webln.getBalance (site without a grant, headless) -> rejected, wallet not asked\n");

  g_autofree gchar *hash = NULL;
  g_autofree gchar *inv = make_invoice(&e, "https://shop.example", 21, &hash);
  CHECK(g_str_has_prefix(inv, "lnbc210n1"));
  fx_drain(&e, reqs += 1);
  g_print("ok webln.makeInvoice -> {paymentRequest lnbc210n1…, rHash}\n");

  /* 21 sats + 1 sat fee reserve fits the 50 sat budget: paid without asking */
  g_autofree gchar *pay = g_strdup_printf("{\"id\":\"sp\",\"method\":\"webln.sendPayment\","
                                          "\"origin\":\"https://shop.example\",\"params\":{\"paymentRequest\":\"%s\"}}", inv);
  g_autoptr(JsonNode) pr = call(&e, pay);
  const gchar *preimage = json_object_get_string_member(result_obj(pr), "preimage");
  CHECK(preimage && strlen(preimage) == 64);
  guint8 raw[32];
  for (int i = 0; i < 32; i++)
    raw[i] = (guint8)(g_ascii_xdigit_value(preimage[2 * i]) * 16 + g_ascii_xdigit_value(preimage[2 * i + 1]));
  g_autofree gchar *ph = g_compute_checksum_for_data(G_CHECKSUM_SHA256, raw, 32);
  CHECK(g_str_equal(ph, hash));
  fx_drain(&e, reqs += 1);
  CHECK(fx_count(&e, "REQUEST pay_invoice") == 1);
  CHECK(get_balance_sats(&e, "https://shop.example") == 100000 - 21);
  fx_drain(&e, reqs += 1);
  g_autofree gchar *ledger = NULL;
  CHECK(g_file_get_contents(e.budgets, &ledger, NULL, NULL));
  CHECK(strstr(ledger, "\"spent_msat\" : 21000") || strstr(ledger, "\"spent_msat\":21000"));
  g_print("ok webln.sendPayment within the site budget -> preimage (sha256 = rHash), wallet debited, "
          "21000 msat ledgered for https://shop.example\n");

  /* 40 sats + fee no longer fits the remaining 29: refused before the wallet */
  g_autofree gchar *h2 = NULL;
  g_autofree gchar *inv2 = make_invoice(&e, "https://shop.example", 40, &h2);
  fx_drain(&e, reqs += 1);
  g_autofree gchar *pay2 = g_strdup_printf("{\"id\":\"sp2\",\"method\":\"webln.sendPayment\","
                                           "\"origin\":\"https://shop.example\",\"params\":{\"paymentRequest\":\"%s\"}}", inv2);
  expect_error(&e, pay2, "budget_exceeded");
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!e.budget_app && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
  CHECK(g_strcmp0(e.budget_app, "https://shop.example") == 0);
  CHECK(e.budget_requested == 41000);
  CHECK(fx_count(&e, "REQUEST pay_invoice") == 1);
  g_print("ok webln.sendPayment over the remaining budget -> budget_exceeded, BudgetExceeded(\"https://shop.example\", 41000), "
          "wallet not asked\n");

  /* a site with no budget at all: needs the user, headless -> rejected */
  g_autofree gchar *pay3 = g_strdup_printf("{\"id\":\"sp3\",\"method\":\"webln.sendPayment\","
                                           "\"origin\":\"https://reader.example\",\"params\":{\"paymentRequest\":\"%s\"}}", inv2);
  expect_error(&e, pay3, "rejected");
  CHECK(fx_count(&e, "REQUEST pay_invoice") == 1);
  g_print("ok webln.sendPayment from a site without a budget (headless) -> rejected\n");

  teardown(&e);
  g_print("PASS nostr-signer-webext-host webln paired e2e\n");
  return 0;
}
