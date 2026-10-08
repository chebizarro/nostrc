/* MLS proof, KeyPackage publication/lookup and Welcome with Alice backed by
 * a permission-checking NIP-46 bunker instead of the local test signer. */
#define GH_TEST_REMOTE_MLS 1
#include "mls-world.h"

static gboolean
remote_proof_and_key_package(gpointer data)
{
  World *w = data;
  gboolean proof = FALSE, package = FALSE;
  for (guint i = 0; i < w->remote_signed_kinds->len; i++) {
    gint kind = g_array_index(w->remote_signed_kinds, gint, i);
    proof |= kind == 450;
    package |= kind == 30443;
  }
  return proof && package;
}

static void
test_remote_mls_proof_package_and_receive(void)
{
  world_remote_key = ALICE;
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  spin_until(key_package_published, &w.apps[ALICE], "remote KeyPackage publication");
  spin_until(key_package_published, &w.apps[BOB], "peer KeyPackage publication");
#if GH_MLS_SERVICE_ACCOUNT_PROOF
  spin_until(remote_proof_and_key_package, &w, "remote proof and KeyPackage signatures");
  g_assert_cmpint(gh_mls_service_get_identity_state(w.apps[ALICE].service), ==,
                  GH_MLS_IDENTITY_ENROLLED);
#endif
  accept_contact(&w.apps[ALICE], BOB);
  const guint invitees[] = { BOB };
  GhMlsGroup *alice_group = create_group(&w.apps[ALICE], "remote signer MLS",
                                           invitees, G_N_ELEMENTS(invitees));
  g_assert_nonnull(alice_group); /* Alice looked up and consumed Bob's KeyPackage. */
  GhMlsGroup *bob_group = join(&w.apps[BOB], ALICE);
  g_assert_nonnull(bob_group); /* Bob received the Welcome. */
  send_text(&w.apps[ALICE], alice_group, "from remote signer");
  wait_message(&w.apps[BOB], gh_mls_group_get_room_id(bob_group), "from remote signer");
  world_down(&w);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  mls_world_init();
  g_test_add_func("/groundhog/mls/remote-proof-keypackage-receive",
                  test_remote_mls_proof_package_and_receive);
  int status = g_test_run();
  mls_world_finish();
  return status;
}
