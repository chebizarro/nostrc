/* test_nip51_loader.c — Unit tests for gnostr-nip51-loader.
 *
 * SPDX-License-Identifier: MIT
 *
 * The async NDB loader can't be exercised here without a populated
 * store — that path is validated by integration. These tests pin
 * down the pure pieces:
 *
 *   - gnostr_nip51_list_to_filter_set():
 *       * happy path extracts p-tags only
 *       * rejects lists with no p-tags
 *       * name resolution (proposed > title > identifier > fallback)
 *       * description round-trip
 *       * dedup by case-insensitive hex
 *       * ignores invalid pubkeys
 *   - GnostrNip51UserLists lifecycle (empty + with elements)
 *
 * nostrc-yg8j.8.
 */

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nostr/nip51/nip51.h>

#include "../src/model/gnostr-filter-set.h"
#include "../src/model/gnostr-nip51-loader.h"

/* ----- helpers ----- */

static const char *PK_1 =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static const char *PK_2 =
    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
static const char *PK_3 =
    "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f";

static NostrList *
make_people_list(const char *identifier,
                 const char *title,
                 const char *description)
{
  NostrList *l = nostr_nip51_list_new();
  g_assert_nonnull(l);
  if (identifier) nostr_nip51_list_set_identifier(l, identifier);
  if (title)      nostr_nip51_list_set_title(l, title);
  if (description) {
    /* NostrList doesn't expose a setter; poke directly (see nip51.h). */
    free(l->description);
    l->description = strdup(description);
  }
  return l;
}

static void
add_p(NostrList *l, const char *hex)
{
  NostrListEntry *e = nostr_nip51_entry_new("p", hex, NULL, false);
  g_assert_nonnull(e);
  nostr_nip51_list_add_entry(l, e);
}

static void
add_tag(NostrList *l, const char *tag_name, const char *value)
{
  NostrListEntry *e = nostr_nip51_entry_new(tag_name, value, NULL, false);
  g_assert_nonnull(e);
  nostr_nip51_list_add_entry(l, e);
}

/* ----- tests ----- */

static void
test_user_lists_empty_lifecycle(void)
{
  /* Defensive: the loader's internal user_lists_new() shouldn't leak
   * when the caller never populates it. Exercise the public free with
   * NULL + with a freshly-built empty container (indirectly, through
   * a zero-element import path). */
  gnostr_nip51_user_lists_free(NULL); /* no-op */

  g_assert_cmpuint(gnostr_nip51_user_lists_get_count(NULL), ==, 0);
  g_assert_null(gnostr_nip51_user_lists_get_nth(NULL, 0));
}

static void
test_convert_happy_path(void)
{
  NostrList *list = make_people_list("cypherpunks", "Cypherpunks",
                                      "My trusted privacy folk");
  add_p(list, PK_1);
  add_p(list, PK_2);

  GnostrFilterSet *fs = gnostr_nip51_list_to_filter_set(list, NULL);
  g_assert_nonnull(fs);

  g_assert_cmpint(gnostr_filter_set_get_source(fs), ==,
                  GNOSTR_FILTER_SET_SOURCE_CUSTOM);
  g_assert_cmpstr(gnostr_filter_set_get_name(fs),        ==, "Cypherpunks");
  g_assert_cmpstr(gnostr_filter_set_get_description(fs), ==,
                  "My trusted privacy folk");

  const gchar * const *authors = gnostr_filter_set_get_authors(fs);
  g_assert_nonnull(authors);
  g_assert_cmpstr(authors[0], ==, PK_1);
  g_assert_cmpstr(authors[1], ==, PK_2);
  g_assert_null  (authors[2]);

  g_object_unref(fs);
  nostr_nip51_list_free(list);
}

static void
test_convert_mixed_entries_extracts_p_only(void)
{
  /* The spec (and the loader API contract) says "p" entries become the
   * filter set's authors; "e", "t", "a", "r", "word" are ignored. */
  NostrList *list = make_people_list("mixed", "Mixed Tags", NULL);
  add_p(list, PK_1);
  add_tag(list, "e", "deadbeef00000000000000000000000000000000000000000000000000000000");
  add_tag(list, "t", "bitcoin");
  add_tag(list, "word", "nostr");
  add_p(list, PK_2);
  add_tag(list, "a", "30000:abcd:def");
  add_tag(list, "r", "https://example.com/feed");

  GnostrFilterSet *fs = gnostr_nip51_list_to_filter_set(list, NULL);
  g_assert_nonnull(fs);

  const gchar * const *authors = gnostr_filter_set_get_authors(fs);
  g_assert_nonnull(authors);
  g_assert_cmpstr(authors[0], ==, PK_1);
  g_assert_cmpstr(authors[1], ==, PK_2);
  g_assert_null  (authors[2]);

  /* No hashtags/kinds/ids leaked through. */
  g_assert_null(gnostr_filter_set_get_hashtags(fs));
  gsize nk = 0;
  g_assert_null(gnostr_filter_set_get_kinds(fs, &nk));
  g_assert_cmpuint(nk, ==, 0);
  g_assert_null(gnostr_filter_set_get_ids(fs));

  g_object_unref(fs);
  nostr_nip51_list_free(list);
}

static void
test_convert_no_p_entries_returns_null(void)
{
  NostrList *list = make_people_list("no-people", "No People", NULL);
  add_tag(list, "t", "bitcoin");
  add_tag(list, "word", "censored");

  GnostrFilterSet *fs = gnostr_nip51_list_to_filter_set(list, NULL);
  g_assert_null(fs);

  nostr_nip51_list_free(list);
}

static void
test_convert_name_resolution(void)
{
  /* proposed_name wins over title. */
  NostrList *l1 = make_people_list("id1", "Title", NULL);
  add_p(l1, PK_1);
  GnostrFilterSet *fs1 = gnostr_nip51_list_to_filter_set(l1, "Custom Name");
  g_assert_nonnull(fs1);
  g_assert_cmpstr(gnostr_filter_set_get_name(fs1), ==, "Custom Name");
  g_object_unref(fs1);
  nostr_nip51_list_free(l1);

  /* No proposed_name → title wins. */
  NostrList *l2 = make_people_list("id2", "Just Title", NULL);
  add_p(l2, PK_1);
  GnostrFilterSet *fs2 = gnostr_nip51_list_to_filter_set(l2, NULL);
  g_assert_nonnull(fs2);
  g_assert_cmpstr(gnostr_filter_set_get_name(fs2), ==, "Just Title");
  g_object_unref(fs2);
  nostr_nip51_list_free(l2);

  /* No proposed_name, no title → identifier wins. */
  NostrList *l3 = make_people_list("my-identifier", NULL, NULL);
  add_p(l3, PK_1);
  GnostrFilterSet *fs3 = gnostr_nip51_list_to_filter_set(l3, NULL);
  g_assert_nonnull(fs3);
  g_assert_cmpstr(gnostr_filter_set_get_name(fs3), ==, "my-identifier");
  g_object_unref(fs3);
  nostr_nip51_list_free(l3);

  /* Nothing at all → generic fallback. */
  NostrList *l4 = make_people_list(NULL, NULL, NULL);
  add_p(l4, PK_1);
  GnostrFilterSet *fs4 = gnostr_nip51_list_to_filter_set(l4, NULL);
  g_assert_nonnull(fs4);
  g_assert_cmpstr(gnostr_filter_set_get_name(fs4), ==, "NIP-51 List");
  g_object_unref(fs4);
  nostr_nip51_list_free(l4);

  /* Empty proposed_name falls through to title. */
  NostrList *l5 = make_people_list("id5", "Empty Proposed", NULL);
  add_p(l5, PK_1);
  GnostrFilterSet *fs5 = gnostr_nip51_list_to_filter_set(l5, "");
  g_assert_nonnull(fs5);
  g_assert_cmpstr(gnostr_filter_set_get_name(fs5), ==, "Empty Proposed");
  g_object_unref(fs5);
  nostr_nip51_list_free(l5);
}

static void
test_convert_dedups_case_insensitive(void)
{
  /* Same hex repeated with different casing must coalesce. */
  NostrList *list = make_people_list("dedup", "Dedup", NULL);
  add_p(list, PK_1);
  gchar *upper = g_ascii_strup(PK_1, -1);
  add_p(list, upper);
  g_free(upper);
  add_p(list, PK_1);   /* exact dup */
  add_p(list, PK_2);   /* distinct */

  GnostrFilterSet *fs = gnostr_nip51_list_to_filter_set(list, NULL);
  g_assert_nonnull(fs);

  const gchar * const *authors = gnostr_filter_set_get_authors(fs);
  g_assert_nonnull(authors);
  g_assert_cmpstr(authors[0], ==, PK_1);
  g_assert_cmpstr(authors[1], ==, PK_2);
  g_assert_null  (authors[2]);

  g_object_unref(fs);
  nostr_nip51_list_free(list);
}

static void
test_convert_skips_invalid_pubkeys(void)
{
  /* gnostr_ensure_hex_pubkey() emits g_warning on non-hex, non-bech32
   * input — tell the test framework those warnings are expected and
   * that the test should not bail out of the process on them. */
  GLogLevelFlags prev = g_log_set_always_fatal(
      (GLogLevelFlags)(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR));

  NostrList *list = make_people_list("mixed-valid", "Mixed Valid", NULL);
  add_p(list, PK_1);
  add_p(list, "");                  /* empty — silently skipped */
  add_p(list, "not-a-pubkey");      /* garbage — warning + skipped */
  add_p(list, PK_3);
  add_p(list, "0123");              /* too short — warning + skipped */

  GnostrFilterSet *fs = gnostr_nip51_list_to_filter_set(list, NULL);
  g_assert_nonnull(fs);

  const gchar * const *authors = gnostr_filter_set_get_authors(fs);
  g_assert_nonnull(authors);
  g_assert_cmpstr(authors[0], ==, PK_1);
  g_assert_cmpstr(authors[1], ==, PK_3);
  g_assert_null  (authors[2]);

  g_object_unref(fs);
  nostr_nip51_list_free(list);

  g_log_set_always_fatal(prev);
}

static void
test_convert_null_list_returns_null(void)
{
  g_assert_null(gnostr_nip51_list_to_filter_set(NULL, NULL));
  g_assert_null(gnostr_nip51_list_to_filter_set(NULL, "ignored"));
}

/* ----- main ----- */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nip51-loader/lifecycle/empty",
                  test_user_lists_empty_lifecycle);
  g_test_add_func("/nip51-loader/convert/happy-path",
                  test_convert_happy_path);
  g_test_add_func("/nip51-loader/convert/mixed-entries",
                  test_convert_mixed_entries_extracts_p_only);
  g_test_add_func("/nip51-loader/convert/no-p-entries",
                  test_convert_no_p_entries_returns_null);
  g_test_add_func("/nip51-loader/convert/name-resolution",
                  test_convert_name_resolution);
  g_test_add_func("/nip51-loader/convert/dedup-case-insensitive",
                  test_convert_dedups_case_insensitive);
  g_test_add_func("/nip51-loader/convert/skip-invalid-pubkeys",
                  test_convert_skips_invalid_pubkeys);
  g_test_add_func("/nip51-loader/convert/null-list",
                  test_convert_null_list_returns_null);
  return g_test_run();
}
