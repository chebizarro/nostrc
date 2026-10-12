/* nostrc-test-isolation.h — fail closed when a test runs against the owner's
 * real profile (nostrc-ttvxv).
 *
 * The CTest entries that set NOSTRC_TEST_ISOLATION also point HOME and the
 * XDG base directories at a private per-test directory. A test (or a helper
 * such as nostrc_test_bus_new()) calls nostrc_test_isolation_assert(): with
 * NOSTRC_TEST_ISOLATION set, it aborts when HOME is the real user's home from
 * the password database, or when the effective data, config, cache or state
 * directory is (or is below) the real user's default one. Without the
 * variable it does nothing, so suites that have not opted in are unaffected.
 * Header-only; GLib and libc only. */
#ifndef NOSTRC_TEST_ISOLATION_H
#define NOSTRC_TEST_ISOLATION_H

#include <glib.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NOSTRC_TEST_ISOLATION_ENV "NOSTRC_TEST_ISOLATION"
/* Exported by a test bus while it is the session bus; the application key
 * stores accept no other session bus under test (nostrc-profile-guard.h). */
#define NOSTRC_TEST_BUS_ADDRESS_ENV "NOSTRC_TEST_BUS_ADDRESS"

static inline gboolean
nostrc_test_isolation_enabled(void)
{
  const gchar *value = g_getenv(NOSTRC_TEST_ISOLATION_ENV);
  return value && *value && g_strcmp0(value, "0") != 0;
}

static inline gchar *
nostrc_test_isolation_canonical(const gchar *path)
{
  g_autofree gchar *abs = g_canonicalize_filename(path, "/");
  char *real = realpath(abs, NULL);
  if (!real)
    return g_steal_pointer(&abs);
  gchar *out = g_strdup(real);
  free(real);
  return out;
}

static inline gboolean
nostrc_test_isolation_within(const gchar *path, const gchar *root)
{
  g_autofree gchar *p = nostrc_test_isolation_canonical(path);
  g_autofree gchar *r = nostrc_test_isolation_canonical(root);
  gsize n = strlen(r);
  return strncmp(p, r, n) == 0 && (p[n] == '\0' || p[n] == '/');
}

/* NULL when the environment is private; otherwise why it is not. Pure: the
 * real home and the environment are arguments, so it can be tested. */
static inline gchar *
nostrc_test_isolation_violation(const gchar *real_home, const gchar *const *envp)
{
  if (!real_home || !*real_home)
    return g_strdup("the real user's home directory is unknown");
  const gchar *home = g_environ_getenv((gchar **)envp, "HOME");
  if (!home || !*home)
    return g_strdup("HOME is not set");
  g_autofree gchar *home_c = nostrc_test_isolation_canonical(home);
  g_autofree gchar *real_c = nostrc_test_isolation_canonical(real_home);
  if (g_strcmp0(home_c, real_c) == 0)
    return g_strdup_printf("HOME is the real home %s", real_home);
  static const struct { const gchar *var, *rel; } dirs[] = {
    { "XDG_DATA_HOME", ".local/share" }, { "XDG_CONFIG_HOME", ".config" },
    { "XDG_CACHE_HOME", ".cache" },      { "XDG_STATE_HOME", ".local/state" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
    const gchar *value = g_environ_getenv((gchar **)envp, dirs[i].var);
    g_autofree gchar *effective = value && *value ? g_strdup(value)
                                                  : g_build_filename(home, dirs[i].rel, NULL);
    g_autofree gchar *real_dir = g_build_filename(real_home, dirs[i].rel, NULL);
    if (nostrc_test_isolation_within(effective, real_dir))
      return g_strdup_printf("%s resolves to %s, inside the real %s", dirs[i].var, effective,
                             real_dir);
  }
  return NULL;
}

static inline void
nostrc_test_isolation_assert(void)
{
  if (!nostrc_test_isolation_enabled())
    return;
  struct passwd *pw = getpwuid(getuid());
  g_auto(GStrv) envp = g_get_environ();
  g_autofree gchar *why = nostrc_test_isolation_violation(pw ? pw->pw_dir : NULL,
                                                          (const gchar *const *)envp);
  if (why)
    g_error("test isolation (nostrc-ttvxv): %s; refusing to run against the real profile",
            why);
}

#endif /* NOSTRC_TEST_ISOLATION_H */
