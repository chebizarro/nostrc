/* test_dbus.c - the real nostr-wallet-agent on a private bus, paired with
 * tests/nwa-fixture-wallet (a NIP-47 wallet service + relay on 127.0.0.1).
 *
 * SPDX-License-Identifier: MIT
 *
 * Linux only (callers are identified from /proc). Two callers:
 *   self   this test process; its identity is computed the way the agent
 *          does it (cgroup app unit, else exe:<path>) and seeded in
 *          budgets.json where a case needs a grant;
 *   gdbus  the gdbus(1) CLI: an identified app with no grants.
 * The agent runs headless: anything that would need a dialog is refused.
 */
#include "nwa-caller.h"
#include "nwa-test-bolt11.h"

#include <libsoup/soup.h>
#include <time.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#ifndef NWA_AGENT_PATH
#error "NWA_AGENT_PATH"
#endif
#ifndef NWA_FIXTURE_PATH
#error "NWA_FIXTURE_PATH"
#endif

typedef struct {
  GTestDBus *tbus;
  GDBusConnection *bus;
  GSubprocess *fixture, *agent;
  GDataInputStream *fx_out;
  GOutputStream *fx_in;
  GString *fx_lines;
  GCancellable *fx_cancel;
  gboolean fx_reading;
  gchar *tmpdir, *budgets, *agent_log, *uri, *self_id;
} Env;

static Env *cur;

static void
dump(void)
{
  g_autofree gchar *log = NULL;
  if (cur && g_file_get_contents(cur->agent_log, &log, NULL, NULL))
    g_printerr("---- agent log ----\n%s---- fixture ----\n%s----\n", log, cur->fx_lines->str);
}

static gchar *
fx_line(Env *e)
{
  gchar *line = g_data_input_stream_read_line_utf8(e->fx_out, NULL, NULL, NULL);
  if (!line) { dump(); g_error("fixture wallet exited"); }
  g_string_append_printf(e->fx_lines, "%s\n", line);
  return line;
}

static guint
fx_count(Env *e, const gchar *needle)
{
  guint n = 0;
  for (const gchar *p = e->fx_lines->str; (p = strstr(p, needle)); p++) n++;
  return n;
}

static void
fx_command(Env *e, const gchar *cmd)
{
  g_autofree gchar *l = g_strdup_printf("%s\n", cmd);
  g_assert_true(g_output_stream_write_all(e->fx_in, l, strlen(l), NULL, NULL, NULL));
  g_output_stream_flush(e->fx_in, NULL, NULL);
}

/* What the agent will call this process (see nwa-caller.c resolve_from_proc). */
static gchar *
self_identity(void)
{
  g_autofree gchar *cg = NULL;
  if (g_file_get_contents("/proc/self/cgroup", &cg, NULL, NULL)) {
    NwaCallerKind k;
    gchar *id = nwa_caller_parse_cgroup(cg, &k);
    if (id) return id;
  }
  g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  g_assert_nonnull(exe);
  return g_strconcat("exe:", exe, NULL);
}

static void
seed(Env *e, const gchar *apps_json)
{
  g_autofree gchar *doc = g_strdup_printf("{\"version\":1,\"apps\":{%s}}", apps_json);
  g_assert_true(g_file_set_contents(e->budgets, doc, -1, NULL));
  g_chmod(e->budgets, 0600);
}

static void
on_name(GDBusConnection *c, const gchar *n, const gchar *o, gpointer ud)
{
  (void)c; (void)n; (void)o;
  *(gboolean *)ud = TRUE;
}

/* @extra_env: NULL-terminated KEY=VALUE list. */
static void
agent_start(Env *e, gboolean paired, const gchar *const *extra_env)
{
  g_autoptr(GSubprocessLauncher) al = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
  g_subprocess_launcher_set_stderr_file_path(al, e->agent_log);
  g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_EPHEMERAL", "1", TRUE);
  g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_HEADLESS", "1", TRUE);
  g_subprocess_launcher_setenv(al, "G_MESSAGES_DEBUG", "all", TRUE);
  if (paired) g_subprocess_launcher_setenv(al, "NOSTR_WALLET_AGENT_TEST_PAIR_URI", e->uri, TRUE);
  for (guint i = 0; extra_env && extra_env[i]; i++) {
    g_auto(GStrv) kv = g_strsplit(extra_env[i], "=", 2);
    g_subprocess_launcher_setenv(al, kv[0], kv[1], TRUE);
  }
  g_autoptr(GError) err = NULL;
  e->agent = g_subprocess_launcher_spawn(al, &err, NWA_AGENT_PATH, "--gapplication-service", NULL);
  g_assert_no_error(err);
  gboolean up = FALSE;
  guint w = g_bus_watch_name_on_connection(e->bus, "org.nostr.Wallet1", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                           on_name, NULL, &up, NULL);
  gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (!up && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
  g_bus_unwatch_name(w);
  g_assert_true(up);
}

static void
agent_stop(Env *e)
{
  g_subprocess_send_signal(e->agent, SIGTERM);
  g_subprocess_wait(e->agent, NULL, NULL);
  g_clear_object(&e->agent);
}

static void
env_up(Env *e)
{
  memset(e, 0, sizeof *e);
  cur = e;
  e->fx_lines = g_string_new(NULL);
  e->tmpdir = g_dir_make_tmp("nwa_dbus_XXXXXX", NULL);
  static const gchar *const sub[] = { "config", "data", "state", "runtime", "cache" };
  static const gchar *const var[] = { "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
                                      "XDG_RUNTIME_DIR", "XDG_CACHE_HOME" };
  for (gsize i = 0; i < G_N_ELEMENTS(sub); i++) {
    g_autofree gchar *d = g_build_filename(e->tmpdir, sub[i], NULL);
    g_mkdir_with_parents(d, 0700);
    g_setenv(var[i], d, TRUE);
  }
  g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
  g_unsetenv("DISPLAY");
  g_unsetenv("WAYLAND_DISPLAY");
  g_autofree gchar *wdir = g_build_filename(e->tmpdir, "state", "nostr-wallet", NULL);
  g_mkdir_with_parents(wdir, 0700);
  e->budgets = g_build_filename(wdir, "budgets.json", NULL);
  e->agent_log = g_build_filename(e->tmpdir, "agent.log", NULL);
  e->self_id = self_identity();

  g_autoptr(GError) err = NULL;
  e->fixture = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                NWA_FIXTURE_PATH, NULL);
  g_assert_no_error(err);
  e->fx_out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(e->fixture));
  e->fx_in = g_subprocess_get_stdin_pipe(e->fixture);
  for (;;) {
    g_autofree gchar *l = fx_line(e);
    if (g_str_has_prefix(l, "URI ")) e->uri = g_strdup(l + 4);
    if (g_str_equal(l, "READY")) break;
  }

  e->tbus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(e->tbus);
  e->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
}

static void
env_down(Env *e)
{
  if (e->fx_cancel) {
    g_cancellable_cancel(e->fx_cancel);
    while (e->fx_reading) g_main_context_iteration(NULL, TRUE);
    g_clear_object(&e->fx_cancel);
  }
  if (e->agent) agent_stop(e);
  g_subprocess_send_signal(e->fixture, SIGTERM);
  g_subprocess_wait(e->fixture, NULL, NULL);
  g_object_unref(e->fixture);
  g_object_unref(e->fx_out);
  g_object_unref(e->bus);
  g_test_dbus_down(e->tbus);
  g_object_unref(e->tbus);
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", e->tmpdir);
  (void)!system(cmd);
  g_string_free(e->fx_lines, TRUE);
  g_free(e->tmpdir); g_free(e->budgets); g_free(e->agent_log); g_free(e->uri); g_free(e->self_id);
  cur = NULL;
}

/* Call as this process. Returns the reply or NULL with *@err_name set. */
static GVariant *
call_self(Env *e, const gchar *method, GVariant *args, gchar **err_name)
{
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_sync(e->bus, "org.nostr.Wallet1", "/org/nostr/Wallet1",
                                            "org.nostr.Wallet1", method, args, NULL,
                                            G_DBUS_CALL_FLAGS_NONE, 20000, NULL, &err);
  if (err_name) *err_name = err ? g_dbus_error_get_remote_error(err) : NULL;
  if (err) g_message("%s -> %s", method, err->message);
  g_clear_error(&err);
  return r;
}

static void
assert_error(Env *e, const gchar *method, GVariant *args, const gchar *want)
{
  g_autofree gchar *name = NULL;
  g_autoptr(GVariant) r = call_self(e, method, args, &name);
  if (r || g_strcmp0(name, want) != 0) {
    dump();
    g_error("%s: want %s, got %s", method, want, r ? "a reply" : name);
  }
}

/* Call through gdbus(1): a separate, identified, ungranted application.
 * Returns its combined output; *@ok = exit status 0. */
static gchar *
call_gdbus(const gchar *method, const gchar *const *args, gboolean *ok)
{
  g_autoptr(GPtrArray) argv = g_ptr_array_new();
  g_autofree gchar *m = g_strconcat("org.nostr.Wallet1.", method, NULL);
  const gchar *base[] = { "gdbus", "call", "--session", "--dest", "org.nostr.Wallet1", "--object-path",
                          "/org/nostr/Wallet1", "--timeout", "20", "--method", m };
  for (guint i = 0; i < G_N_ELEMENTS(base); i++) g_ptr_array_add(argv, (gpointer)base[i]);
  for (guint i = 0; args && args[i]; i++) g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  g_autoptr(GError) err = NULL;
  g_autoptr(GSubprocess) p = g_subprocess_newv((const gchar *const *)argv->pdata,
                                               G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                               &err);
  g_assert_no_error(err);
  gchar *out = NULL;
  g_assert_true(g_subprocess_communicate_utf8(p, NULL, NULL, &out, NULL, NULL));
  *ok = g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0;
  g_message("gdbus %s -> %s", method, out);
  return out;
}

/* gdbus must be a different application than this process: true unless the
 * test runs inside an app scope both processes would share. */
static gboolean
distinct_callers(Env *e)
{
  if (g_str_has_prefix(e->self_id, "exe:")) return TRUE;
  g_test_skip("test process runs in an app scope; gdbus would share its identity");
  return FALSE;
}

static gchar *
gdbus_identity(void)
{
  g_autofree gchar *path = g_find_program_in_path("gdbus");
  g_assert_nonnull(path);
  g_autofree gchar *real = realpath(path, NULL);
  return g_strconcat("exe:", real, NULL);
}

/* ---- prqu.19: non-interactive reads ---- */

static void
test_non_interactive_reads(void)
{
  Env e;
  env_up(&e);
  if (!distinct_callers(&e)) { env_down(&e); return; }
  g_autofree gchar *grant = g_strdup_printf("\"%s\":{\"allow_read\":true}", e.self_id);
  seed(&e, grant);
  agent_start(&e, TRUE, NULL);

  /* granted caller: answered, by the wallet */
  g_autoptr(GVariant) bal = call_self(&e, "GetBalanceNonInteractive", NULL, NULL);
  g_assert_nonnull(bal);
  guint64 msat = 0;
  g_variant_get(bal, "(t)", &msat);
  g_assert_cmpuint(msat, ==, 100000000);
  g_autoptr(GVariant) info = call_self(&e, "GetInfoNonInteractive", NULL, NULL);
  g_assert_nonnull(info);
  g_autoptr(GVariant) d = g_variant_get_child_value(info, 0);
  const gchar *alias = NULL;
  g_assert_true(g_variant_lookup(d, "alias", "&s", &alias));
  g_assert_cmpstr(alias, ==, "fixture wallet");
  while (fx_count(&e, "REQUEST ") < 2) g_free(fx_line(&e));

  /* ungranted caller: InteractionRequired, nothing reaches the wallet, and
   * the plain method would have needed the user (headless: Denied) */
  gboolean ok = TRUE;
  g_autofree gchar *o1 = call_gdbus("GetBalanceNonInteractive", NULL, &ok);
  g_assert_false(ok);
  g_assert_nonnull(strstr(o1, "org.nostr.Wallet1.Error.InteractionRequired"));
  g_autofree gchar *o2 = call_gdbus("GetInfoNonInteractive", NULL, &ok);
  g_assert_false(ok);
  g_assert_nonnull(strstr(o2, "org.nostr.Wallet1.Error.InteractionRequired"));
  g_autofree gchar *o3 = call_gdbus("GetBalance", NULL, &ok);
  g_assert_false(ok);
  g_assert_nonnull(strstr(o3, "org.nostr.Wallet1.Error.Denied"));
  g_assert_cmpuint(fx_count(&e, "REQUEST "), ==, 2);
  agent_stop(&e);

  /* unpaired: GetInfoNonInteractive = GetInfo ({paired:false}), balance NotPaired */
  agent_start(&e, FALSE, NULL);
  g_autofree gchar *o4 = call_gdbus("GetInfoNonInteractive", NULL, &ok);
  g_assert_true(ok);
  g_assert_nonnull(strstr(o4, "'paired': <false>"));
  assert_error(&e, "GetBalanceNonInteractive", NULL, "org.nostr.Wallet1.Error.NotPaired");
  env_down(&e);
}

/* ---- prqu.20: the settings app grants / revokes read access ---- */

static void
on_apps_changed(GDBusConnection *c, const gchar *s, const gchar *p, const gchar *i, const gchar *sig,
                GVariant *params, gpointer ud)
{
  (void)c; (void)s; (void)p; (void)i; (void)sig; (void)params;
  (*(guint *)ud)++;
}

static gboolean
listed_allow_read(Env *e, const gchar *app, gboolean *found)
{
  g_autoptr(GVariant) r = call_self(e, "ListApps", NULL, NULL);
  g_assert_nonnull(r);
  g_autoptr(GVariant) apps = g_variant_get_child_value(r, 0);
  g_autoptr(GVariant) rec = g_variant_lookup_value(apps, app, G_VARIANT_TYPE_VARDICT);
  *found = rec != NULL;
  gboolean allow = FALSE;
  if (rec) g_assert_true(g_variant_lookup(rec, "allow_read", "b", &allow));
  return allow;
}

static void
wait_count(guint *n, guint want)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (*n < want && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(*n, >=, want);
}

static void
test_settings_grants(void)
{
  Env e;
  env_up(&e);
  if (!distinct_callers(&e)) { env_down(&e); return; }
  g_autofree gchar *gd = gdbus_identity();
  seed(&e, "\"https://shop.example\":{\"allow_read\":true,\"limit_msat_per_day\":21000}");
  g_autofree gchar *self_real = g_file_read_link("/proc/self/exe", NULL);
  g_autofree gchar *env_settings = g_strconcat("NOSTR_WALLET_AGENT_SETTINGS_APPS=", self_real, NULL);
  const gchar *const extra[] = { env_settings, NULL };
  agent_start(&e, TRUE, extra);
  guint changed = 0;
  guint sub = g_dbus_connection_signal_subscribe(e.bus, NULL, "org.nostr.Wallet1", "AppsChanged",
                                                 "/org/nostr/Wallet1", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                 on_apps_changed, &changed, NULL);
  gboolean found = FALSE, ok = TRUE;

  /* the settings app lists every app, and grants without a dialog (headless) */
  g_assert_true(listed_allow_read(&e, "https://shop.example", &found));
  g_assert_true(found);
  listed_allow_read(&e, gd, &found);
  g_assert_false(found);
  g_autoptr(GVariant) g1 = call_self(&e, "SetReadAccess", g_variant_new("(sb)", gd, TRUE), NULL);
  g_assert_nonnull(g1);
  wait_count(&changed, 1);
  g_assert_true(listed_allow_read(&e, gd, &found));
  g_autofree gchar *o1 = call_gdbus("GetBalanceNonInteractive", NULL, &ok);
  g_assert_true(ok);
  g_assert_nonnull(strstr(o1, "uint64 100000000"));

  /* an app may give up its own access, without a dialog ... */
  const gchar *const revoke_own[] = { "''", "false", NULL };
  g_autofree gchar *o2 = call_gdbus("SetReadAccess", revoke_own, &ok);
  g_assert_true(ok);
  wait_count(&changed, 2);
  g_assert_false(listed_allow_read(&e, gd, &found));
  g_autofree gchar *o3 = call_gdbus("GetBalanceNonInteractive", NULL, &ok);
  g_assert_nonnull(strstr(o3, "InteractionRequired"));
  /* ... but not grant it to itself without the user (headless: Denied),
   * list others, or revoke another app's */
  const gchar *const grant_own[] = { "''", "true", NULL };
  g_autofree gchar *o4 = call_gdbus("SetReadAccess", grant_own, &ok);
  g_assert_false(ok);
  g_assert_nonnull(strstr(o4, "org.nostr.Wallet1.Error.Denied"));
  g_autofree gchar *o5 = call_gdbus("ListApps", NULL, &ok);
  g_assert_false(ok);
  g_assert_nonnull(strstr(o5, "org.nostr.Wallet1.Error.Denied"));
  const gchar *const revoke_other[] = { "https://shop.example", "false", NULL };
  g_autofree gchar *o6 = call_gdbus("SetReadAccess", revoke_other, &ok);
  g_assert_false(ok);
  g_assert_nonnull(strstr(o6, "org.nostr.Wallet1.Error.Denied"));
  g_assert_true(listed_allow_read(&e, "https://shop.example", &found));

  /* the settings app revokes another app's access (a site), no dialog */
  g_autoptr(GVariant) g2 = call_self(&e, "SetReadAccess",
                                     g_variant_new("(sb)", "https://shop.example", FALSE), NULL);
  g_assert_nonnull(g2);
  g_assert_false(listed_allow_read(&e, "https://shop.example", &found));
  assert_error(&e, "SetReadAccess", g_variant_new("(sb)", "bad\nid", TRUE),
               "org.nostr.Wallet1.Error.InvalidArgs");
  /* a no-op changes nothing and signals nothing */
  guint before = changed;
  g_autoptr(GVariant) g3 = call_self(&e, "SetReadAccess",
                                     g_variant_new("(sb)", "https://shop.example", FALSE), NULL);
  g_assert_nonnull(g3);
  /* grant admin, not money: may read another app's budget, but a raise
   * still needs the user (headless: Denied) */
  g_autoptr(GVariant) gb = call_self(&e, "GetBudget", g_variant_new("(s)", "https://shop.example"), NULL);
  g_assert_nonnull(gb);
  guint32 lim = 0;
  g_variant_get(gb, "(ut)", &lim, NULL);
  g_assert_cmpuint(lim, ==, 21000);
  assert_error(&e, "SetBudget", g_variant_new("(su)", "https://shop.example", (guint32)99000000),
               "org.nostr.Wallet1.Error.Denied");
  g_assert_cmpuint(changed, ==, before);
  g_dbus_connection_signal_unsubscribe(e.bus, sub);
  agent_stop(&e);

  /* The same binary is NOT the settings app without the (test-build)
   * override: the installed <bindir>/nostr-settings is, by path + inode. */
  agent_start(&e, TRUE, NULL);
  assert_error(&e, "ListApps", NULL, "org.nostr.Wallet1.Error.Denied");
  assert_error(&e, "SetReadAccess", g_variant_new("(sb)", gd, TRUE), "org.nostr.Wallet1.Error.Denied");
  /* grants persisted across the restart */
  g_autofree gchar *doc = NULL;
  g_assert_true(g_file_get_contents(e.budgets, &doc, NULL, NULL));
  g_assert_nonnull(strstr(doc, "https://shop.example"));
  env_down(&e);
}

static gboolean log_has(Env *e, const gchar *needle);

/* Wait for fixture output without blocking the main loop (the test process
 * may be serving HTTP to the agent meanwhile). */
static void
on_fx_line_async(GObject *src, GAsyncResult *res, gpointer ud)
{
  Env *e = ud;
  gchar *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, NULL);
  if (line) g_string_append_printf(e->fx_lines, "%s\n", line);
  g_free(line);
  e->fx_reading = FALSE;
}

static gboolean
fx_wait(Env *e, const gchar *needle, guint secs)
{
  if (!e->fx_cancel) e->fx_cancel = g_cancellable_new();
  gint64 deadline = g_get_monotonic_time() + (gint64)secs * G_USEC_PER_SEC;
  while (!strstr(e->fx_lines->str, needle) && g_get_monotonic_time() < deadline) {
    if (!e->fx_reading) {
      e->fx_reading = TRUE;
      g_data_input_stream_read_line_async(e->fx_out, G_PRIORITY_DEFAULT, e->fx_cancel, on_fx_line_async, e);
    }
    g_main_context_iteration(NULL, FALSE);
    g_usleep(2000);
  }
  return strstr(e->fx_lines->str, needle) != NULL;
}

/* ---- prqu.8: LNURL-pay / Lightning address links ---- */

typedef struct {
  gchar   *base;       /* http://127.0.0.1:<port> */
  gchar   *metadata;
  guint64  last_amount;
  gchar   *last_comment;
} Lnurl;

static void
lnurl_serve(SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *q, gpointer data)
{
  (void)srv;
  Lnurl *l = data;
  g_autofree gchar *body = NULL;
  if (g_str_has_prefix(path, "/.well-known/lnurlp/")) {
    const gchar *user = path + strlen("/.well-known/lnurlp/");
    g_autofree gchar *host = g_strdup(l->base + strlen("http://"));
    g_free(l->metadata);
    l->metadata = g_strdup_printf("[[\"text/plain\",\"Coffee for %s\"],[\"text/identifier\",\"%s@%s\"]]",
                                  user, user, host);
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tag"); json_builder_add_string_value(b, "payRequest");
    json_builder_set_member_name(b, "callback");
    g_autofree gchar *cb = g_strdup_printf("%s/cb/%s", l->base, user);
    json_builder_add_string_value(b, cb);
    json_builder_set_member_name(b, "minSendable"); json_builder_add_int_value(b, 21000);
    json_builder_set_member_name(b, "maxSendable"); json_builder_add_int_value(b, 100000);
    json_builder_set_member_name(b, "commentAllowed"); json_builder_add_int_value(b, 40);
    json_builder_set_member_name(b, "metadata"); json_builder_add_string_value(b, l->metadata);
    json_builder_end_object(b);
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    body = json_to_string(root, FALSE);
  } else if (g_str_has_prefix(path, "/cb/")) {
    const gchar *amt = q ? g_hash_table_lookup(q, "amount") : NULL;
    l->last_amount = amt ? g_ascii_strtoull(amt, NULL, 10) : 0;
    g_free(l->last_comment);
    l->last_comment = g_strdup(q ? g_hash_table_lookup(q, "comment") : NULL);
    /* "mallory" answers with an invoice that commits to something else */
    const gchar *committed = g_str_equal(path, "/cb/mallory") ? "[[\"text/plain\",\"something else\"]]" : l->metadata;
    g_autofree gchar *h = g_compute_checksum_for_string(G_CHECKSUM_SHA256, committed, -1);
    g_autofree gchar *inv = nwa_test_bolt11_mint(l->last_amount, NULL, h,
      "5555555555555555555555555555555555555555555555555555555555555555", (gint64)time(NULL), 0);
    body = g_strdup_printf("{\"pr\":\"%s\",\"routes\":[]}", inv);
  } else {
    soup_server_message_set_status(msg, 404, NULL);
    return;
  }
  soup_server_message_set_status(msg, 200, NULL);
  soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, body, strlen(body));
}

static void
test_lnurl_links(void)
{
  Env e;
  env_up(&e);
  Lnurl l = { 0 };
  SoupServer *srv = soup_server_new(NULL, NULL);
  soup_server_add_handler(srv, NULL, lnurl_serve, &l, NULL);
  g_assert_true(soup_server_listen_local(srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  GSList *uris = soup_server_get_uris(srv);
  l.base = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  const gchar *const accept[] = { "NOSTR_WALLET_AGENT_TEST_ANSWER=accept", NULL };
  agent_start(&e, TRUE, accept);

  /* lightning:user@host -> .well-known/lnurlp -> dialog (min amount, a
   * comment) -> callback -> invoice committing to the metadata -> paid */
  g_autofree gchar *link = g_strdup_printf("lightning:alice@%s", l.base + strlen("http://"));
  g_autoptr(GVariant) r = call_self(&e, "OpenUri", g_variant_new("(s)", link), NULL);
  g_assert_nonnull(r);
  if (!fx_wait(&e, "REQUEST pay_invoice", 20)) { dump(); g_error("LNURL payment never reached the wallet"); }
  g_assert_cmpuint(l.last_amount, ==, 21000);
  g_assert_cmpstr(l.last_comment, ==, "sent from a test");
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!log_has(&e, "Paid 21 sats") && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, FALSE);
  g_assert_true(log_has(&e, "answering LNURL dialog"));
  g_assert_true(log_has(&e, "Coffee for alice"));
  g_assert_true(log_has(&e, "Paid 21 sats"));

  /* an invoice that does not commit to what the user approved is refused */
  g_autofree gchar *bad = g_strdup_printf("lightning:mallory@%s", l.base + strlen("http://"));
  g_autoptr(GVariant) r2 = call_self(&e, "OpenUri", g_variant_new("(s)", bad), NULL);
  g_assert_nonnull(r2);
  deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!log_has(&e, "does not commit to the payment details") && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(log_has(&e, "does not commit to the payment details"));
  g_assert_cmpuint(fx_count(&e, "REQUEST pay_invoice"), ==, 1);

  agent_stop(&e);
  g_object_unref(srv);
  g_free(l.base); g_free(l.metadata); g_free(l.last_comment);
  env_down(&e);
}

/* ---- prqu.12: agent-generated keys (nostr+walletauth) ---- */

typedef struct {
  guint n;
  gboolean ok;
  gchar *msg;
} Finished;

static void
on_wa_finished(GDBusConnection *c, const gchar *s, const gchar *p, const gchar *i, const gchar *sig,
               GVariant *params, gpointer ud)
{
  (void)c; (void)s; (void)p; (void)i; (void)sig;
  Finished *f = ud;
  g_free(f->msg);
  g_variant_get(params, "(bs)", &f->ok, &f->msg);
  f->n++;
}

static gchar *
begin_auth(Env *e, const gchar *relay)
{
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  const gchar *const rl[] = { relay, NULL };
  g_variant_builder_add(&b, "{sv}", "relays", g_variant_new_strv(rl, -1));
  g_variant_builder_add(&b, "{sv}", "name", g_variant_new_string("test_dbus"));
  g_autofree gchar *err = NULL;
  g_autoptr(GVariant) r = call_self(e, "BeginWalletAuth", g_variant_new("(a{sv})", &b), &err);
  if (!r) { dump(); g_error("BeginWalletAuth: %s", err); }
  gchar *uri = NULL;
  g_variant_get(r, "(s)", &uri);
  return uri;
}

static gchar *
uri_client_key(const gchar *uri)
{
  const gchar *pk = uri + strlen("nostr+walletauth://");
  return g_strndup(pk, 64);
}

static gchar *
info_string(Env *e, const gchar *key)
{
  g_autoptr(GVariant) r = call_self(e, "GetInfo", NULL, NULL);
  g_assert_nonnull(r);
  g_autoptr(GVariant) d = g_variant_get_child_value(r, 0);
  gchar *v = NULL;
  g_variant_lookup(d, key, "s", &v);
  return v;
}

static gboolean
log_has(Env *e, const gchar *needle)
{
  g_autofree gchar *log = NULL;
  return g_file_get_contents(e->agent_log, &log, NULL, NULL) && strstr(log, needle);
}

static void
wait_finished(Finished *f, guint n)
{
  gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
  while (f->n < n && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
  if (f->n < n) { dump(); g_error("no WalletAuthFinished"); }
}

static gchar *
fixture_relay(Env *e)
{
  const gchar *r = strstr(e->fx_lines->str, "RELAY ");
  g_assert_nonnull(r);
  return g_strndup(r + 6, strcspn(r + 6, "\n"));
}

static void
test_wallet_auth(void)
{
  Env e;
  env_up(&e);
  if (!distinct_callers(&e)) { env_down(&e); return; }
  g_autofree gchar *relay = fixture_relay(&e);
  g_autofree gchar *grant = g_strdup_printf("\"%s\":{\"allow_read\":true}", e.self_id);
  seed(&e, grant);
  const gchar *const accept[] = { "NOSTR_WALLET_AGENT_TEST_ANSWER=accept", NULL };
  agent_start(&e, TRUE, accept);
  Finished fin = { 0 };
  guint sub = g_dbus_connection_signal_subscribe(e.bus, NULL, "org.nostr.Wallet1", "WalletAuthFinished",
                                                 "/org/nostr/Wallet1", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                 on_wa_finished, &fin, NULL);
  g_autofree gchar *old_client = info_string(&e, "client_pubkey");
  g_autofree gchar *wallet_pk = info_string(&e, "wallet_pubkey");

  /* 1. the wallet approves (echoing state): verified with get_info on the
   *    new key, confirmed by the user, stored, made active */
  g_autofree gchar *uri = begin_auth(&e, relay);
  g_assert_true(g_str_has_prefix(uri, "nostr+walletauth://"));
  g_assert_null(strstr(uri, "secret"));
  g_assert_nonnull(strstr(uri, "&state="));
  g_autofree gchar *new_client = uri_client_key(uri);
  g_assert_cmpstr(new_client, !=, old_client);
  g_autofree gchar *cmd = g_strconcat("AUTH ", uri, NULL);
  fx_command(&e, cmd);
  wait_finished(&fin, 1);
  g_assert_true(fin.ok);
  g_autofree gchar *req = g_strdup_printf("REQUEST get_info %s", new_client);
  while (!strstr(e.fx_lines->str, req)) g_free(fx_line(&e)); /* the staged check */
  g_autofree gchar *now_client = info_string(&e, "client_pubkey");
  g_assert_cmpstr(now_client, ==, new_client);
  g_autofree gchar *now_wallet = info_string(&e, "wallet_pubkey");
  g_assert_cmpstr(now_wallet, ==, wallet_pk);
  g_assert_true(log_has(&e, "answering dialog \"Connect Wallet\": accept"));
  g_assert_true(log_has(&e, "Check that your wallet app now lists this connection"));
  g_assert_true(log_has(&e, "This replaces the wallet currently connected"));
  g_autoptr(GVariant) bal = call_self(&e, "GetBalance", NULL, NULL);
  g_assert_nonnull(bal); /* the wallet serves the agent's own key */
  agent_stop(&e);

  /* 2. Alby-style answer without state, user declines: nothing changes */
  const gchar *const deny[] = { "NOSTR_WALLET_AGENT_TEST_ANSWER=deny", NULL };
  agent_start(&e, TRUE, deny);
  g_autofree gchar *uri2 = begin_auth(&e, relay);
  g_autofree gchar *cmd2 = g_strconcat("AUTH-NOSTATE ", uri2, NULL);
  fx_command(&e, cmd2);
  wait_finished(&fin, 2);
  g_assert_false(fin.ok);
  g_assert_cmpstr(fin.msg, ==, "declined");
  g_assert_true(log_has(&e, "did not prove that it answered this particular request"));
  g_autofree gchar *still = info_string(&e, "client_pubkey");
  g_assert_cmpstr(still, ==, old_client); /* test pairing, untouched */

  /* 3. an impostor races the wallet (both unconfirmed): conflict, nothing
   *    connected, and new requests refused for a while */
  g_autofree gchar *uri3 = begin_auth(&e, relay);
  g_autofree gchar *imp = g_strdup("1111111111111111111111111111111111111111111111111111111111111111");
  g_autofree gchar *cmd3a = g_strdup_printf("AUTH-AS %s %s", imp, uri3);
  g_autofree gchar *cmd3b = g_strconcat("AUTH-NOSTATE ", uri3, NULL);
  fx_command(&e, cmd3a);
  fx_command(&e, cmd3b);
  wait_finished(&fin, 3);
  g_assert_false(fin.ok);
  g_assert_nonnull(strstr(fin.msg, "two different wallets"));
  assert_error(&e, "BeginWalletAuth", g_variant_new_parsed("({'name': <'again'>},)"),
               "org.nostr.Wallet1.Error.RateLimited");
  agent_stop(&e);

  /* 4. one request at a time: others cannot supersede or cancel it */
  agent_start(&e, TRUE, deny);
  g_autofree gchar *uri4 = begin_auth(&e, relay);
  gboolean ok = TRUE;
  g_autofree gchar *rl = g_strdup_printf("{'relays': <['%s']>}", relay);
  const gchar *const gargs[] = { rl, NULL };
  g_autofree gchar *o1 = call_gdbus("BeginWalletAuth", gargs, &ok);
  g_assert_false(ok);
  g_assert_nonnull(strstr(o1, "RateLimited"));
  g_autofree gchar *o2 = call_gdbus("CancelWalletAuth", NULL, &ok);
  g_assert_false(ok);
  g_assert_nonnull(strstr(o2, "Denied"));
  g_autoptr(GVariant) cr = call_self(&e, "CancelWalletAuth", NULL, NULL);
  g_assert_nonnull(cr);
  wait_finished(&fin, 4);
  g_assert_cmpstr(fin.msg, ==, "cancelled");
  agent_stop(&e);

  /* 5. headless: refused up front (the final confirmation needs a display) */
  agent_start(&e, TRUE, NULL);
  assert_error(&e, "BeginWalletAuth", g_variant_new_parsed("(@a{sv} {},)"), "org.nostr.Wallet1.Error.Denied");
  g_dbus_connection_signal_unsubscribe(e.bus, sub);
  g_free(fin.msg);
  env_down(&e);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/dbus/wallet-auth", test_wallet_auth);
  g_test_add_func("/dbus/lnurl-links", test_lnurl_links);
  g_test_add_func("/dbus/non-interactive-reads", test_non_interactive_reads);
  g_test_add_func("/dbus/settings-grants", test_settings_grants);
  return g_test_run();
}
