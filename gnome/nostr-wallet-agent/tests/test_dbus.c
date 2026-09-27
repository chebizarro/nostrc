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

static G_GNUC_UNUSED void
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

/* ---- prqu.19: non-interactive reads ---- */

static void
test_non_interactive_reads(void)
{
  Env e;
  env_up(&e);
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

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/dbus/non-interactive-reads", test_non_interactive_reads);
  return g_test_run();
}
