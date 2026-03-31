#include <glib.h>

#include "../src/sync/gnostr-sync-bridge.h"

static void
test_set_switch_clear_sequence(void)
{
  static const char *user_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  static const char *user_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

  gnostr_sync_bridge_set_user_pubkey(user_a);
  g_assert_cmpstr(gnostr_sync_bridge_get_user_pubkey(), ==, user_a);

  gnostr_sync_bridge_set_user_pubkey(user_b);
  g_assert_cmpstr(gnostr_sync_bridge_get_user_pubkey(), ==, user_b);

  gnostr_sync_bridge_set_user_pubkey(NULL);
  g_assert_null(gnostr_sync_bridge_get_user_pubkey());
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/sync-bridge/user-state/set-switch-clear", test_set_switch_clear_sequence);

  return g_test_run();
}
