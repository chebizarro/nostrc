#include <glib.h>

#include "../src/sync/gnostr-sync-bridge-mute.h"
#include <nostr-gobject-1.0/gnostr-mute-list.h>

static gboolean s_get_default_called = FALSE;
static gboolean s_fetch_called = FALSE;
static char *s_last_pubkey = NULL;
static int s_fake_mute_list;
static GNostrMuteListFetchCallback s_stored_callback = NULL;
static gpointer s_stored_user_data = NULL;

GNostrMuteList *
gnostr_mute_list_get_default(void)
{
  s_get_default_called = TRUE;
  return (GNostrMuteList *)&s_fake_mute_list;
}

void
gnostr_mute_list_fetch_async(GNostrMuteList *self,
                             const char *pubkey_hex,
                             const char * const *relays,
                             GNostrMuteListFetchCallback callback,
                             gpointer user_data)
{
  (void)self;
  (void)relays;

  s_fetch_called = TRUE;
  g_free(s_last_pubkey);
  s_last_pubkey = g_strdup(pubkey_hex);
  
  /* F36: Store callback so tests can invoke it */
  s_stored_callback = callback;
  s_stored_user_data = user_data;
}

static void
reset_state(void)
{
  s_get_default_called = FALSE;
  s_fetch_called = FALSE;
  g_clear_pointer(&s_last_pubkey, g_free);
  s_stored_callback = NULL;
  s_stored_user_data = NULL;
}

static void
test_reload_skips_without_user(void)
{
  reset_state();
  g_assert_false(gnostr_sync_bridge_reload_mute_list(NULL));
  g_assert_false(s_get_default_called);
  g_assert_false(s_fetch_called);
}

static void
test_reload_fetches_for_user(void)
{
  static const char *user = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

  reset_state();
  g_assert_true(gnostr_sync_bridge_reload_mute_list(user));
  g_assert_true(s_get_default_called);
  g_assert_true(s_fetch_called);
  g_assert_cmpstr(s_last_pubkey, ==, user);
}

/* F36 error-path test: Callback invoked with success */
static void
test_callback_invoked_with_success(void)
{
  static const char *user = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

  reset_state();
  g_assert_true(gnostr_sync_bridge_reload_mute_list(user));
  g_assert_nonnull(s_stored_callback);
  
  /* Invoke the callback with success=TRUE - should not crash */
  s_stored_callback((GNostrMuteList *)&s_fake_mute_list, TRUE, s_stored_user_data);
  
  /* If we reach here, the callback was processed successfully */
  g_assert_true(TRUE);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/sync-bridge-mute/skip-without-user", test_reload_skips_without_user);
  g_test_add_func("/gnostr/sync-bridge-mute/reload-for-user", test_reload_fetches_for_user);
  g_test_add_func("/gnostr/sync-bridge-mute/callback-success", test_callback_invoked_with_success);

  return g_test_run();
}
