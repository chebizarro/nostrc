#include <glib.h>

#include "../src/util/mute_filter.h"
#include <nostr-gobject-1.0/gnostr-mute-list.h>

static void
test_pubkey_mute(void)
{
  GNostrMuteList *ml = gnostr_mute_list_get_default();
  g_assert_nonnull(ml);

  gnostr_mute_list_load_from_json(ml, "{\"kind\":10000,\"tags\":[]}");
  gnostr_mute_list_add_pubkey(ml, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", FALSE);

  g_assert_true(gnostr_mute_filter_should_hide_fields(
    ml,
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "hello world",
    NULL));
}

static void
test_word_mute(void)
{
  GNostrMuteList *ml = gnostr_mute_list_get_default();
  g_assert_nonnull(ml);

  gnostr_mute_list_load_from_json(ml, "{\"kind\":10000,\"tags\":[]}");
  gnostr_mute_list_add_word(ml, "spoiler", FALSE);

  g_assert_true(gnostr_mute_filter_should_hide_fields(
    ml,
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "contains spoiler text",
    NULL));
}

static void
test_hashtag_mute(void)
{
  GNostrMuteList *ml = gnostr_mute_list_get_default();
  char **hashtags = g_new0(char *, 2);
  hashtags[0] = g_strdup("bitcoin");
  g_assert_nonnull(ml);

  gnostr_mute_list_load_from_json(ml, "{\"kind\":10000,\"tags\":[]}");
  gnostr_mute_list_add_hashtag(ml, "bitcoin", FALSE);

  g_assert_true(gnostr_mute_filter_should_hide_fields(
    ml,
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    "price post",
    hashtags));

  g_strfreev(hashtags);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/mute-filter/pubkey", test_pubkey_mute);
  g_test_add_func("/gnostr/mute-filter/word", test_word_mute);
  g_test_add_func("/gnostr/mute-filter/hashtag", test_hashtag_mute);

  return g_test_run();
}
