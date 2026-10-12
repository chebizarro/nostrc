/* gh-profile-guard.h — never let a test touch the owner's real profile
 * (nostrc-ttvxv).
 *
 * On 2026-10-10 a test-key account (secret key 1) got an encrypted store under
 * the developer's real ~/.local/share/groundhog and a StoreKey item in the
 * login Keychain. Two independent rules now refuse that, before any file or
 * Keychain item is created:
 *
 *  1. Under test (NOSTRC_TEST_ISOLATION set and not "0"; every Groundhog CTest
 *     entry sets it, and children inherit it) nothing may use the real user's
 *     data home (<passwd home>/.local/share, also the default of
 *     g_get_user_data_dir()) or the user's default Keychain.
 *  2. Always: the well-known test accounts — the public keys of secret keys
 *     1..4, which anyone can sign for — never get a store in the real data
 *     home or an item in the default Keychain.
 *
 * "Real" comes from the password database, not $HOME, so a test that moves
 * HOME is still checked against the owner's actual home. Header-only so the
 * store, store-key and NIP-46 credential libraries share one copy. */
#ifndef GH_PROFILE_GUARD_H
#define GH_PROFILE_GUARD_H

#include <glib.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GH_PROFILE_GUARD_ENV "NOSTRC_TEST_ISOLATION"

static inline gboolean
gh_profile_guard_under_test(void)
{
  const gchar *value = g_getenv(GH_PROFILE_GUARD_ENV);
  return value && *value && g_strcmp0(value, "0") != 0;
}

/* x-only public keys of the secret keys 1, 2, 3 and 4 (k·G). */
static inline gboolean
gh_profile_guard_is_test_account(const gchar *account_pubkey)
{
  static const gchar *const test_accounts[] = {
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
    "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5",
    "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9",
    "e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd13",
  };
  if (!account_pubkey)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(test_accounts); i++)
    if (g_ascii_strcasecmp(account_pubkey, test_accounts[i]) == 0)
      return TRUE;
  return FALSE;
}

/* Lexically normalised absolute path (no ".", "..", or repeated "/"), with
 * symlinks resolved when the path exists. */
static inline gchar *
gh_profile_guard_canonical(const gchar *path)
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
gh_profile_guard_is_within(const gchar *path, const gchar *root)
{
  gsize n = strlen(root);
  return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* The pure rule, with the owner's home passed in (unit-testable). TRUE means
 * data_dir must be refused for account_pubkey (NULL: any account). */
static inline gboolean
gh_profile_guard_refuses_data_dir_for(const gchar *real_home, const gchar *data_dir,
                                      const gchar *account_pubkey, gboolean under_test)
{
  if (!real_home || !*real_home || !data_dir || !g_path_is_absolute(data_dir))
    return FALSE;
  if (!under_test && !gh_profile_guard_is_test_account(account_pubkey))
    return FALSE;
  g_autofree gchar *share = g_build_filename(real_home, ".local", "share", NULL);
  g_autofree gchar *real_share = gh_profile_guard_canonical(share);
  g_autofree gchar *dir = gh_profile_guard_canonical(data_dir);
  return gh_profile_guard_is_within(dir, real_share) ||
         gh_profile_guard_is_within(dir, share);
}

static inline const gchar *
gh_profile_guard_real_home(void)
{
  struct passwd *pw = getpwuid(getuid());
  return pw ? pw->pw_dir : NULL;
}

static inline gboolean
gh_profile_guard_refuses_data_dir(const gchar *data_dir, const gchar *account_pubkey)
{
  return gh_profile_guard_refuses_data_dir_for(gh_profile_guard_real_home(), data_dir,
                                               account_pubkey,
                                               gh_profile_guard_under_test());
}

/* The user's default Keychain (no explicit test keychain) for an account. */
static inline gboolean
gh_profile_guard_refuses_default_keychain(const gchar *account_pubkey)
{
  return gh_profile_guard_under_test() || gh_profile_guard_is_test_account(account_pubkey);
}

#endif /* GH_PROFILE_GUARD_H */
