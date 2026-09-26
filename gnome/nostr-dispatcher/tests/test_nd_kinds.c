/* X-Nostr-Kinds / handlers.list key grammar. */
#include "nd-kinds.h"

static void test_tokens(void) {
  NdKindRange r;
  g_assert_true(nd_kind_token_parse("1", &r));
  g_assert_cmpuint(r.lo, ==, 1); g_assert_cmpuint(r.hi, ==, 1); g_assert_false(r.any);
  g_assert_true(nd_kind_token_parse(" 30000-39999 ", &r));
  g_assert_cmpuint(r.lo, ==, 30000); g_assert_cmpuint(r.hi, ==, 39999);
  g_assert_true(nd_kind_token_parse("0", &r));
  g_assert_cmpuint(r.lo, ==, 0);
  g_assert_true(nd_kind_token_parse("65535", &r));
  g_assert_true(nd_kind_token_parse("*", &r));
  g_assert_true(r.any);
  g_assert_true(nd_kind_token_parse("7 - 9", &r));
  g_assert_cmpuint(r.lo, ==, 7); g_assert_cmpuint(r.hi, ==, 9);

  const char *bad[] = {"", " ", "-1", "65536", "abc", "1x", "9-7", "1-", "-5",
                       "+1", "1.5", "0x10", "1-2-3", "**", NULL};
  for (int i = 0; bad[i]; i++) {
    g_test_message("bad token '%s'", bad[i]);
    g_assert_false(nd_kind_token_parse(bad[i], &r));
  }
}

static void test_spec_and_best(void) {
  /* Example from the bead, plus junk that must be skipped, not fatal. */
  g_autoptr(GArray) s = nd_kind_spec_parse("1;6;7;30023;30000-39999;bogus;;99999;");
  g_assert_cmpuint(s->len, ==, 5);
  guint32 w = 0;
  g_assert_true(nd_kind_spec_best(s, 30023, &w));
  g_assert_cmpuint(w, ==, 1);                /* exact beats the range */
  g_assert_true(nd_kind_spec_best(s, 30024, &w));
  g_assert_cmpuint(w, ==, 10000);            /* only the range */
  g_assert_true(nd_kind_spec_best(s, 7, &w));
  g_assert_false(nd_kind_spec_best(s, 2, &w));
  g_assert_false(nd_kind_spec_best(s, 40000, &w));
  g_assert_false(nd_kind_spec_has_any(s));

  g_autoptr(GArray) any = nd_kind_spec_parse("1;*;");
  g_assert_true(nd_kind_spec_has_any(any));
  g_assert_false(nd_kind_spec_best(any, 2, &w)); /* `*` never a best match */

  g_autoptr(GArray) nested = nd_kind_spec_parse("0-65535;30000-39999;30020-30029");
  g_assert_true(nd_kind_spec_best(nested, 30023, &w));
  g_assert_cmpuint(w, ==, 10);

  g_autoptr(GArray) empty = nd_kind_spec_parse("");
  g_assert_cmpuint(empty->len, ==, 0);
  g_autoptr(GArray) null = nd_kind_spec_parse(NULL);
  g_assert_cmpuint(null->len, ==, 0);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nd/kinds/tokens", test_tokens);
  g_test_add_func("/nd/kinds/spec", test_spec_and_best);
  return g_test_run();
}
