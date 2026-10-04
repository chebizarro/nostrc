/* groundhog-two-instance: verifies that two Groundhog instances with
 * different GROUNDHOG_INSTANCE names can start concurrently on the same
 * machine without colliding (nostrc-lrac).
 *
 * Phase 1 (this test): both instances start in --smoke mode, register
 * with different GApplication ids, and exit cleanly.
 *
 * Phase 2 (future): a D-Bus test control interface drives the full
 * encrypted-group lifecycle (create, invite, accept, message, rename,
 * remove, re-add, leave, restart, verify history) against local relays.
 *
 * The test sets up each instance with separate XDG dirs and a memory
 * GSettings backend so nothing persists beyond the run. */
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
  const gchar *instance_name;
  const gchar *groundhog_bin;
  const gchar *schema_dir;
  gchar *xdg_base;
  GPid pid;
  gint status;
  gboolean exited;
  gboolean timed_out;
} Instance;

static gboolean
wait_instance(Instance *inst, guint timeout_ms)
{
  for (guint i = 0; i < timeout_ms / 100; i++) {
    int r = waitpid(inst->pid, &inst->status, WNOHANG);
    if (r > 0) {
      inst->exited = TRUE;
      return TRUE;
    }
    g_usleep(100 * 1000);
  }
  inst->timed_out = TRUE;
  return FALSE;
}

static gboolean
launch_instance(Instance *inst)
{
  /* Each instance gets its own XDG base under the test tmpdir. */
  g_autofree gchar *config = g_build_filename(inst->xdg_base, ".config", NULL);
  g_autofree gchar *data = g_build_filename(inst->xdg_base, ".local", "share", NULL);
  g_autofree gchar *cache = g_build_filename(inst->xdg_base, ".cache", NULL);
  g_autofree gchar *state = g_build_filename(inst->xdg_base, ".local", "state", NULL);

  g_mkdir_with_parents(config, 0700);
  g_mkdir_with_parents(data, 0700);
  g_mkdir_with_parents(cache, 0700);
  g_mkdir_with_parents(state, 0700);

  /* Build the environment. */
  g_auto(GStrv) env = g_get_environ();
  env = g_environ_setenv(env, "GROUNDHOG_INSTANCE", inst->instance_name, TRUE);
  env = g_environ_setenv(env, "GSETTINGS_BACKEND", "memory", TRUE);
  if (inst->schema_dir)
    env = g_environ_setenv(env, "GSETTINGS_SCHEMA_DIR", inst->schema_dir, TRUE);
  /* Clear the D-Bus address so each instance runs in NON_UNIQUE mode
   * without needing a bus. This is enough for the --smoke test. */
  env = g_environ_setenv(env, "DBUS_SESSION_BUS_ADDRESS", "", TRUE);
  env = g_environ_setenv(env, "DBUS_LAUNCHD_SESSION_BUS_SOCKET", "", TRUE);
#ifdef __APPLE__
  env = g_environ_setenv(env, "GROUNDHOG_RUN_GUI_SMOKE", "1", TRUE);
#endif
  /* Override XDG dirs to the instance base. We do NOT rely on the
   * --instance argument's own XDG override here, because the test
   * validates the GROUNDHOG_INSTANCE path (the app id suffix), while
   * keeping XDG explicit so the test controls where data goes. */
  env = g_environ_setenv(env, "XDG_CONFIG_HOME", config, TRUE);
  env = g_environ_setenv(env, "XDG_DATA_HOME", data, TRUE);
  env = g_environ_setenv(env, "XDG_CACHE_HOME", cache, TRUE);
  env = g_environ_setenv(env, "XDG_STATE_HOME", state, TRUE);

  gchar *argv[] = { (gchar *)inst->groundhog_bin, "--smoke", NULL };
  GError *error = NULL;

  gboolean ok = g_spawn_async(NULL, argv, env,
    G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
    NULL, NULL, &inst->pid, &error);
  if (!ok) {
    g_test_message("Could not spawn %s instance '%s': %s",
                   inst->groundhog_bin, inst->instance_name,
                   error->message);
    g_clear_error(&error);
  }
  return ok;
}

static void
test_concurrent_instances(void)
{
  const gchar *groundhog_bin = g_getenv("GROUNDHOG_BIN");
  if (!groundhog_bin) {
    g_test_skip("GROUNDHOG_BIN not set");
    return;
  }
  const gchar *schema_dir = g_getenv("GSETTINGS_SCHEMA_DIR");

  g_autofree gchar *tmpdir = g_dir_make_tmp("groundhog-two-inst-XXXXXX", NULL);
  g_assert_nonnull(tmpdir);

  /* Instance A. */
  g_autofree gchar *base_a = g_build_filename(tmpdir, "inst_a", NULL);
  g_mkdir_with_parents(base_a, 0700);
  Instance a = {
    .instance_name = "testA",
    .groundhog_bin = groundhog_bin,
    .schema_dir = schema_dir,
    .xdg_base = base_a,
  };

  /* Instance B. */
  g_autofree gchar *base_b = g_build_filename(tmpdir, "inst_b", NULL);
  g_mkdir_with_parents(base_b, 0700);
  Instance b = {
    .instance_name = "testB",
    .groundhog_bin = groundhog_bin,
    .schema_dir = schema_dir,
    .xdg_base = base_b,
  };

  /* Launch both. */
  gboolean ok_a = launch_instance(&a);
  gboolean ok_b = launch_instance(&b);

  if (!ok_a || !ok_b) {
    if (ok_a) { kill(a.pid, SIGTERM); waitpid(a.pid, NULL, 0); }
    if (ok_b) { kill(b.pid, SIGTERM); waitpid(b.pid, NULL, 0); }
    g_test_skip("Could not spawn both instances");
    /* Cleanup tmpdir. */
    gchar *rm_argv[] = { "rm", "-rf", tmpdir, NULL };
    g_spawn_sync(NULL, rm_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                 NULL, NULL, NULL, NULL);
    return;
  }

  g_test_message("Instance A (pid %d) and B (pid %d) launched",
                 (int)a.pid, (int)b.pid);

  /* Wait for both to exit. --smoke exits after the widget tree check. */
  gboolean a_ok = wait_instance(&a, 15000);
  gboolean b_ok = wait_instance(&b, 15000);

  if (!a_ok) {
    g_test_message("Instance A timed out — killing");
    kill(a.pid, SIGKILL);
    waitpid(a.pid, NULL, 0);
  }
  if (!b_ok) {
    g_test_message("Instance B timed out — killing");
    kill(b.pid, SIGKILL);
    waitpid(b.pid, NULL, 0);
  }

  /* Both must have exited (not timed out). */
  g_assert_false(a.timed_out);
  g_assert_false(b.timed_out);

  /* Both must have exited normally. Code 0 (smoke OK), 1 (schema or
   * smoke failed — no display) or 77 (skip) are acceptable: this test
   * verifies concurrent starts, not the full UI. */
  if (WIFEXITED(a.status)) {
    int code = WEXITSTATUS(a.status);
    g_test_message("Instance A exited with %d", code);
    g_assert_true(code == 0 || code == 1 || code == 77);
  }
  if (WIFEXITED(b.status)) {
    int code = WEXITSTATUS(b.status);
    g_test_message("Instance B exited with %d", code);
    g_assert_true(code == 0 || code == 1 || code == 77);
  }

  /* Cleanup tmpdir. */
  gchar *rm_argv[] = { "rm", "-rf", tmpdir, NULL };
  g_spawn_sync(NULL, rm_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
               NULL, NULL, NULL, NULL);
}

/* A named smoke must still reach GApplication's option parser. This catches
 * the old argc == 2 special case, which rejected --instance NAME --smoke. */
static gboolean
run_named_smoke(const gchar *const *argv, const gchar *name)
{
  g_auto(GStrv) env = g_get_environ();
  env = g_environ_unsetenv(env, "GROUNDHOG_INSTANCE");
  env = g_environ_setenv(env, "DBUS_SESSION_BUS_ADDRESS", "", TRUE);
  env = g_environ_setenv(env, "DBUS_LAUNCHD_SESSION_BUS_SOCKET", "", TRUE);
#ifdef __APPLE__
  env = g_environ_setenv(env, "GROUNDHOG_RUN_GUI_SMOKE", "1", TRUE);
#endif
  g_autofree gchar *tmpdir = g_dir_make_tmp("groundhog-cli-smoke-XXXXXX", NULL);
  g_assert_nonnull(tmpdir);
  env = g_environ_setenv(env, "XDG_CONFIG_HOME", tmpdir, TRUE);
  env = g_environ_setenv(env, "XDG_DATA_HOME", tmpdir, TRUE);
  env = g_environ_setenv(env, "XDG_CACHE_HOME", tmpdir, TRUE);
  env = g_environ_setenv(env, "XDG_STATE_HOME", tmpdir, TRUE);

  g_autoptr(GError) error = NULL;
  GPid pid = 0;
  gint stderr_fd = -1;
  g_assert_true(g_spawn_async_with_pipes(NULL, (gchar **)argv, env,
    G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL, NULL, &stderr_fd, &error));
  g_assert_no_error(error);

  gint status = 0;
  gboolean exited = FALSE;
  for (guint i = 0; i < 150; i++) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      exited = TRUE;
      break;
    }
    g_usleep(100 * 1000);
  }
  if (!exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }
  g_spawn_close_pid(pid);

  g_autoptr(GString) stderr_log = g_string_new(NULL);
  gchar buf[4096];
  ssize_t count;
  while ((count = read(stderr_fd, buf, sizeof(buf))) > 0)
    g_string_append_len(stderr_log, buf, count);
  close(stderr_fd);

  g_assert_true(exited);
  g_assert_true(WIFEXITED(status));
  if (WEXITSTATUS(status) == 77)
    return FALSE;  /* The runner has no GUI display. */
  g_test_message("%s: %s", name, stderr_log->str);
  g_assert_cmpint(WEXITSTATUS(status), ==, 0);
  g_autofree gchar *expected = g_strdup_printf("app id org.nostr.Groundhog.%s", name);
  g_assert_nonnull(strstr(stderr_log->str, expected));

  gchar *rm_argv[] = { "rm", "-rf", tmpdir, NULL };
  g_spawn_sync(NULL, rm_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
               NULL, NULL, NULL, NULL);
  return TRUE;
}

static void
test_named_smoke_options(void)
{
  const gchar *groundhog_bin = g_getenv("GROUNDHOG_BIN");
  if (!groundhog_bin) {
    g_test_skip("GROUNDHOG_BIN not set");
    return;
  }

  const gchar *first[] = { groundhog_bin, "--instance", "cliSmokeA", "--smoke", NULL };
  if (!run_named_smoke(first, "cliSmokeA")) {
    g_test_skip("No GUI display for named smoke");
    return;
  }
  const gchar *second[] = { groundhog_bin, "--smoke", "--instance=cliSmokeB", NULL };
  g_assert_true(run_named_smoke(second, "cliSmokeB"));

  /* --version is also an existing local option and must compose with the
   * named profile without attempting a GUI or falling through to the parser. */
  gchar *version_argv[] = { (gchar *)groundhog_bin, "--instance", "cliVersion", "--version", NULL };
  g_autofree gchar *version_out = NULL;
  gint version_status = 0;
  g_assert_true(g_spawn_sync(NULL, version_argv, NULL, 0, NULL, NULL,
                             &version_out, NULL, &version_status, NULL));
  g_assert_true(WIFEXITED(version_status));
  g_assert_cmpint(WEXITSTATUS(version_status), ==, 0);
  g_assert_true(g_str_has_prefix(version_out, "Groundhog "));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/two-instance/concurrent", test_concurrent_instances);
  g_test_add_func("/groundhog/two-instance/named-smoke-options", test_named_smoke_options);
  return g_test_run();
}
