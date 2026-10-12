/* nostrc-profile-guard.h — application code refuses the owner's real profile
 * and key stores under test (nostrc-ttvxv, nostrc-wqn0u).
 *
 * Every CTest entry of Groundhog, Grotto, GNostr, nip55l and Signet runs with
 * NOSTRC_TEST_ISOLATION=1 and HOME / XDG_* in a private per-test directory
 * (cmake/NostrcTestIsolation.cmake). That keeps well-behaved tests away from
 * the owner's data; these checks make the application code itself fail
 * closed when a test is not well behaved:
 *
 *  - paths: under test, nothing may be read or written in the real user's
 *    ~/.local/share, ~/.config, ~/.cache, ~/.local/state or ~/Library.
 *  - the macOS default (login) Keychain: never under test. Tests that need a
 *    Keychain use a throwaway keychain file addressed by reference.
 *  - the Secret Service (and anything else reached over the session bus that
 *    holds keys): under test only on a private test bus, i.e. when
 *    DBUS_SESSION_BUS_ADDRESS equals NOSTRC_TEST_BUS_ADDRESS, which the test
 *    bus helpers export when they bring their bus up.
 *
 * "Real" comes from the password database, not $HOME, so a test that moves
 * HOME is still checked against the owner's actual home. Header-only, GLib
 * and libc only; the *_for() variants are pure so the rules are testable. */
#ifndef NOSTRC_PROFILE_GUARD_H
#define NOSTRC_PROFILE_GUARD_H

#include <glib.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NOSTRC_PROFILE_GUARD_ENV "NOSTRC_TEST_ISOLATION"
#define NOSTRC_PROFILE_GUARD_TEST_BUS_ENV "NOSTRC_TEST_BUS_ADDRESS"

static inline gboolean
nostrc_profile_guard_under_test(void)
{
  const gchar *value = g_getenv(NOSTRC_PROFILE_GUARD_ENV);
  return value && *value && g_strcmp0(value, "0") != 0;
}

static inline const gchar *
nostrc_profile_guard_real_home(void)
{
  struct passwd *pw = getpwuid(getuid());
  return pw ? pw->pw_dir : NULL;
}

/* Lexically normalised absolute path, symlinks resolved when it exists. */
static inline gchar *
nostrc_profile_guard_canonical(const gchar *path)
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
nostrc_profile_guard_is_within(const gchar *path, const gchar *root)
{
  gsize n = strlen(root);
  return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* Pure: is path (or below) one of the real user's profile directories? */
static inline gboolean
nostrc_profile_guard_is_real_profile_for(const gchar *real_home, const gchar *path)
{
  if (!real_home || !*real_home || !path || !*path)
    return FALSE;
  static const gchar *const profile_dirs[] = {
    ".local/share", ".config", ".cache", ".local/state", "Library",
  };
  g_autofree gchar *lexical = g_canonicalize_filename(path, "/");
  g_autofree gchar *resolved = nostrc_profile_guard_canonical(path);
  for (guint i = 0; i < G_N_ELEMENTS(profile_dirs); i++) {
    g_autofree gchar *dir = g_build_filename(real_home, profile_dirs[i], NULL);
    g_autofree gchar *dir_c = nostrc_profile_guard_canonical(dir);
    if (nostrc_profile_guard_is_within(lexical, dir) ||
        nostrc_profile_guard_is_within(resolved, dir_c))
      return TRUE;
  }
  return FALSE;
}

/* TRUE: under test, and path is in the real profile; do not touch it. */
static inline gboolean
nostrc_profile_guard_refuses_path(const gchar *path)
{
  return nostrc_profile_guard_under_test() &&
         nostrc_profile_guard_is_real_profile_for(nostrc_profile_guard_real_home(), path);
}

/* Pure: may key-holding services on this session bus be used? Off test,
 * always; under test only on the private bus the test brought up. */
static inline gboolean
nostrc_profile_guard_session_bus_allowed_for(gboolean under_test, const gchar *session_address,
                                             const gchar *test_bus_address)
{
  if (!under_test)
    return TRUE;
  return session_address && *session_address && test_bus_address &&
         g_strcmp0(session_address, test_bus_address) == 0;
}

/* TRUE: under test, and the session bus (its Secret Service, its signer
 * daemon) is not a private test bus; do not use it for keys. */
static inline gboolean
nostrc_profile_guard_refuses_session_keyring(void)
{
  return !nostrc_profile_guard_session_bus_allowed_for(
      nostrc_profile_guard_under_test(), g_getenv("DBUS_SESSION_BUS_ADDRESS"),
      g_getenv(NOSTRC_PROFILE_GUARD_TEST_BUS_ENV));
}

/* TRUE: under test; the user's default (login) Keychain is off limits. */
static inline gboolean
nostrc_profile_guard_refuses_default_keychain(void)
{
  return nostrc_profile_guard_under_test();
}

/* NULL when this process's XDG data, config, cache and state directories are
 * fine to use; otherwise why not (free with g_free). For process start-up. */
static inline gchar *
nostrc_profile_guard_user_dirs_violation(void)
{
  if (!nostrc_profile_guard_under_test())
    return NULL;
  const gchar *dirs[][2] = {
    { "data", g_get_user_data_dir() },   { "config", g_get_user_config_dir() },
    { "cache", g_get_user_cache_dir() }, { "state", g_get_user_state_dir() },
  };
  for (guint i = 0; i < G_N_ELEMENTS(dirs); i++)
    if (nostrc_profile_guard_refuses_path(dirs[i][1]))
      return g_strdup_printf("the user %s directory %s is the real profile", dirs[i][0],
                             dirs[i][1]);
  return NULL;
}

/* Start-up check for programs: abort under test when the XDG directories
 * are the real profile. */
static inline void
nostrc_profile_guard_assert_user_dirs(const gchar *program)
{
  g_autofree gchar *why = nostrc_profile_guard_user_dirs_violation();
  if (why)
    g_error("%s: %s; refusing to run against it under test (NOSTRC_TEST_ISOLATION)", program,
            why);
}

/* Pure: may a GSettings backend (its type name) be used? Under test only the
 * memory backend and the keyfile backend (which writes below
 * XDG_CONFIG_HOME, a private directory under test): dconf writes through
 * the session bus to the owner's database, NSUserDefaults to ~/Library. */
static inline gboolean
nostrc_profile_guard_settings_backend_allowed_for(gboolean under_test, const gchar *type_name)
{
  return !under_test || g_strcmp0(type_name, "GMemorySettingsBackend") == 0 ||
         g_strcmp0(type_name, "GKeyfileSettingsBackend") == 0;
}

#endif /* NOSTRC_PROFILE_GUARD_H */
