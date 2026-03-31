#include <glib.h>

#include "../src/ui/gnostr-mute-row-data.h"

static void
test_user_binding_keeps_hex_canonical(void)
{
  static const char *hex = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  GnostrMuteRowBinding *binding = gnostr_mute_row_binding_new(hex, GNOSTR_MUTE_ROW_USER);

  g_assert_nonnull(binding);
  g_assert_cmpstr(binding->canonical_value, ==, hex);
  g_assert_nonnull(binding->display_value);
  g_assert_true(g_str_has_prefix(binding->display_value, "npub1") ||
                g_strcmp0(binding->display_value, hex) == 0);

  gnostr_mute_row_binding_free(binding);
}

static void
test_hashtag_binding_keeps_raw_tag(void)
{
  GnostrMuteRowBinding *binding = gnostr_mute_row_binding_new("bitcoin", GNOSTR_MUTE_ROW_HASHTAG);

  g_assert_nonnull(binding);
  g_assert_cmpstr(binding->canonical_value, ==, "bitcoin");
  g_assert_cmpstr(binding->display_value, ==, "#bitcoin");

  gnostr_mute_row_binding_free(binding);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/mute-row-data/user-binding", test_user_binding_keeps_hex_canonical);
  g_test_add_func("/gnostr/mute-row-data/hashtag-binding", test_hashtag_binding_keeps_raw_tag);

  return g_test_run();
}
