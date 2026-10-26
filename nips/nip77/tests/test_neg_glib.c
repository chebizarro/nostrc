#include <nostr/nip77/negentropy-glib.h>

static void test_session_factory(void) {
  NostrNegSessionG *session = nostr_neg_session_g_new();
  g_assert_nonnull(session);
  g_assert_true(NOSTR_IS_NEG_SESSION_G(session));
  g_assert_true(g_type_is_a(NOSTR_TYPE_NEG_SESSION_G, G_TYPE_OBJECT));

  gpointer weak_session = session;
  g_object_add_weak_pointer(G_OBJECT(session), &weak_session);
  g_object_unref(session);
  g_assert_null(weak_session);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nip77/glib/session-factory", test_session_factory);
  return g_test_run();
}
