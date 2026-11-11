#ifndef TURN_TEST_KEYCHAIN_H
#define TURN_TEST_KEYCHAIN_H
#ifdef __APPLE__
#include "gh-call-settings-private.h"
#include "../../../../tests/common/nostrc-test-keychain-guard.h"
#include <glib/gstdio.h>

static SecKeychainRef turn_test_keychain;
static gchar *turn_test_dir;
static gchar *turn_test_path;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static void
turn_test_keychain_cleanup(void)
{
  gh_call_turn_password_set_test_keychain(NULL);
  if (turn_test_keychain) {
    g_assert_cmpint(SecKeychainDelete(turn_test_keychain), ==, errSecSuccess);
    CFRelease(turn_test_keychain);
    turn_test_keychain = NULL;
  }
  if (turn_test_path) {
    g_unlink(turn_test_path);
    g_autofree gchar *db = g_strconcat(turn_test_path, "-db", NULL);
    g_unlink(db);
    g_clear_pointer(&turn_test_path, g_free);
  }
  if (turn_test_dir) {
    g_assert_cmpint(g_rmdir(turn_test_dir), ==, 0);
    g_clear_pointer(&turn_test_dir, g_free);
  }
  nostrc_test_keychain_guard_end();
  gchar after[4096];
  nostrc_test_keychain_guard_snapshot(after, sizeof after);
  g_print("Keychain guard after: %s\n", after);
}

static void
turn_test_keychain_setup(void)
{
  nostrc_test_keychain_guard_begin();
  g_print("Keychain guard before: %s\n", nostrc_test_keychain_guard_before);
  atexit(turn_test_keychain_cleanup);
  g_autoptr(GError) error = NULL;
  turn_test_dir = g_dir_make_tmp("gh-fix-turnkc-XXXXXX", &error);
  g_assert_no_error(error);
  turn_test_path = g_build_filename(turn_test_dir, "turn.keychain", NULL);
  g_assert_cmpint(SecKeychainCreate(turn_test_path, 4, "test", FALSE, NULL,
                                   &turn_test_keychain), ==, errSecSuccess);
  g_assert_cmpint(SecKeychainUnlock(turn_test_keychain, 4, "test", TRUE), ==, errSecSuccess);
  gh_call_turn_password_set_test_keychain(turn_test_keychain);
  nostrc_test_keychain_guard_end();
}
#pragma clang diagnostic pop
#endif
#endif
