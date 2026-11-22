#include <glib.h>

#include "../src/sync/gnostr-sync-bridge.h"
#include "../src/util/follow_list.h"
#include "../src/util/pin_list.h"

/* State-only coverage must not turn into a relay/list integration test. */
void gnostr_follow_list_fetch_async(const gchar *pubkey, GCancellable *cancel,
    GnostrFollowListCallback callback, gpointer data) {
  (void)pubkey; (void)cancel; (void)callback; (void)data;
  g_error("user-state fixture attempted a follow-list fetch");
}
GnostrPinList *gnostr_pin_list_get_default(void) {
  g_error("user-state fixture attempted pin-list access");
}
void gnostr_pin_list_fetch_async(GnostrPinList *self, const char *pubkey,
    const char *const *relays, GnostrPinListFetchCallback callback, gpointer data) {
  (void)self; (void)pubkey; (void)relays; (void)callback; (void)data;
  g_error("user-state fixture attempted a pin-list fetch");
}

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
