/**
 * GNostrTimelineQuery Unit Tests
 *
 * Tests for timeline query construction and JSON serialization.
 */

#include <glib.h>
#include <string.h>

/* Include the header we're testing */
#include <nostr-gobject-1.0/gn-timeline-query.h>

/* Test: Thread query generates proper #e tag filter */
static void test_thread_query_event_filter(void) {
  const char *root_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

  GNostrTimelineQuery *query = gnostr_timeline_query_new_thread(root_id);

  g_assert_nonnull(query);
  g_assert_cmpuint(query->n_event_ids, ==, 1);
  g_assert_cmpstr(query->event_ids[0], ==, root_id);

  /* Verify JSON output contains #e filter */
  const char *json = gnostr_timeline_query_to_json(query);
  g_assert_nonnull(json);
  g_assert_true(strstr(json, "\"#e\":[") != NULL);
  g_assert_true(strstr(json, root_id) != NULL);

  /* Should NOT use old hashtag workaround */
  g_assert_null(query->hashtag);

  gnostr_timeline_query_free(query);
}

/* Test: Builder with event_id generates proper #e filter */
static void test_builder_event_id(void) {
  const char *event_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

  GNostrTimelineQueryBuilder *builder = gnostr_timeline_query_builder_new();
  gnostr_timeline_query_builder_add_kind(builder, 1);
  gnostr_timeline_query_builder_add_event_id(builder, event_id);
  GNostrTimelineQuery *query = gnostr_timeline_query_builder_build(builder);

  g_assert_nonnull(query);
  g_assert_cmpuint(query->n_event_ids, ==, 1);
  g_assert_cmpstr(query->event_ids[0], ==, event_id);

  const char *json = gnostr_timeline_query_to_json(query);
  g_assert_true(strstr(json, "\"#e\":[") != NULL);
  g_assert_true(strstr(json, event_id) != NULL);

  gnostr_timeline_query_free(query);
}

/* Test: Multiple event IDs in filter */
static void test_multiple_event_ids(void) {
  const char *id1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *id3 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

  GNostrTimelineQueryBuilder *builder = gnostr_timeline_query_builder_new();
  gnostr_timeline_query_builder_add_kind(builder, 1);
  gnostr_timeline_query_builder_add_event_id(builder, id1);
  gnostr_timeline_query_builder_add_event_id(builder, id2);
  gnostr_timeline_query_builder_add_event_id(builder, id3);
  GNostrTimelineQuery *query = gnostr_timeline_query_builder_build(builder);

  g_assert_cmpuint(query->n_event_ids, ==, 3);
  g_assert_cmpstr(query->event_ids[0], ==, id1);
  g_assert_cmpstr(query->event_ids[1], ==, id2);
  g_assert_cmpstr(query->event_ids[2], ==, id3);

  const char *json = gnostr_timeline_query_to_json(query);
  g_assert_true(strstr(json, id1) != NULL);
  g_assert_true(strstr(json, id2) != NULL);
  g_assert_true(strstr(json, id3) != NULL);

  gnostr_timeline_query_free(query);
}

/* Test: Query copy preserves event_ids */
static void test_query_copy_event_ids(void) {
  const char *root_id = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

  GNostrTimelineQuery *original = gnostr_timeline_query_new_thread(root_id);
  GNostrTimelineQuery *copy = gnostr_timeline_query_copy(original);

  g_assert_nonnull(copy);
  g_assert_cmpuint(copy->n_event_ids, ==, 1);
  g_assert_cmpstr(copy->event_ids[0], ==, root_id);

  /* Verify they're independent (deep copy) */
  g_assert_true(copy->event_ids != original->event_ids);
  g_assert_true(copy->event_ids[0] != original->event_ids[0]);

  gnostr_timeline_query_free(original);
  gnostr_timeline_query_free(copy);
}

/* Test: Query equality with event_ids */
static void test_query_equal_event_ids(void) {
  const char *root_id = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

  GNostrTimelineQuery *q1 = gnostr_timeline_query_new_thread(root_id);
  GNostrTimelineQuery *q2 = gnostr_timeline_query_new_thread(root_id);

  g_assert_true(gnostr_timeline_query_equal(q1, q2));
  g_assert_cmpuint(gnostr_timeline_query_hash(q1), ==, gnostr_timeline_query_hash(q2));

  gnostr_timeline_query_free(q1);
  gnostr_timeline_query_free(q2);
}

/* Test: Different event_ids are not equal */
static void test_query_not_equal_different_event_ids(void) {
  const char *id1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

  GNostrTimelineQuery *q1 = gnostr_timeline_query_new_thread(id1);
  GNostrTimelineQuery *q2 = gnostr_timeline_query_new_thread(id2);

  g_assert_false(gnostr_timeline_query_equal(q1, q2));

  gnostr_timeline_query_free(q1);
  gnostr_timeline_query_free(q2);
}

/* Test: Multi-hashtag builder produces a single `#t` array
 * carrying every tag. nostrc-yg8j.7 */
static void test_multi_hashtag_builder(void) {
  GNostrTimelineQueryBuilder *b = gnostr_timeline_query_builder_new();
  gnostr_timeline_query_builder_add_kind(b, 1);
  gnostr_timeline_query_builder_add_hashtag(b, "bitcoin");
  gnostr_timeline_query_builder_add_hashtag(b, "nostr");
  gnostr_timeline_query_builder_add_hashtag(b, "zaps");
  GNostrTimelineQuery *q = gnostr_timeline_query_builder_build(b);

  g_assert_nonnull(q);
  g_assert_cmpuint(q->n_hashtags, ==, 3);
  g_assert_cmpstr(q->hashtags[0], ==, "bitcoin");
  g_assert_cmpstr(q->hashtags[1], ==, "nostr");
  g_assert_cmpstr(q->hashtags[2], ==, "zaps");
  /* Legacy single-field mirror points at the first tag. */
  g_assert_cmpstr(q->hashtag, ==, "bitcoin");

  const char *json = gnostr_timeline_query_to_json(q);
  g_assert_nonnull(strstr(json, "\"#t\":[\"bitcoin\",\"nostr\",\"zaps\"]"));

  gnostr_timeline_query_free(q);
}

/* Test: set_hashtag() and add_hashtag() are mutually exclusive —
 * whichever is called last wins. */
static void test_hashtag_mutex(void) {
  /* add → set: set wins (single-tag). */
  {
    GNostrTimelineQueryBuilder *b = gnostr_timeline_query_builder_new();
    gnostr_timeline_query_builder_add_hashtag(b, "nostr");
    gnostr_timeline_query_builder_add_hashtag(b, "zaps");
    gnostr_timeline_query_builder_set_hashtag(b, "bitcoin");
    GNostrTimelineQuery *q = gnostr_timeline_query_builder_build(b);
    g_assert_cmpuint(q->n_hashtags, ==, 0);
    g_assert_cmpstr(q->hashtag, ==, "bitcoin");
    const char *json = gnostr_timeline_query_to_json(q);
    g_assert_nonnull(strstr(json, "\"#t\":[\"bitcoin\"]"));
    g_assert_null(strstr(json, "nostr"));
    gnostr_timeline_query_free(q);
  }
  /* set → add: add wins (multi-tag). */
  {
    GNostrTimelineQueryBuilder *b = gnostr_timeline_query_builder_new();
    gnostr_timeline_query_builder_set_hashtag(b, "bitcoin");
    gnostr_timeline_query_builder_add_hashtag(b, "nostr");
    gnostr_timeline_query_builder_add_hashtag(b, "zaps");
    GNostrTimelineQuery *q = gnostr_timeline_query_builder_build(b);
    g_assert_cmpuint(q->n_hashtags, ==, 2);
    g_assert_cmpstr(q->hashtags[0], ==, "nostr");
    g_assert_cmpstr(q->hashtags[1], ==, "zaps");
    const char *json = gnostr_timeline_query_to_json(q);
    g_assert_nonnull(strstr(json, "\"#t\":[\"nostr\",\"zaps\"]"));
    gnostr_timeline_query_free(q);
  }
}

/* Test: copy preserves the full hashtags array. */
static void test_multi_hashtag_copy(void) {
  GNostrTimelineQueryBuilder *b = gnostr_timeline_query_builder_new();
  gnostr_timeline_query_builder_add_hashtag(b, "a");
  gnostr_timeline_query_builder_add_hashtag(b, "b");
  GNostrTimelineQuery *src = gnostr_timeline_query_builder_build(b);

  GNostrTimelineQuery *dup = gnostr_timeline_query_copy(src);
  g_assert_cmpuint(dup->n_hashtags, ==, src->n_hashtags);
  /* Independent allocations (deep copy) and identical content. */
  g_assert_true(dup->hashtags != src->hashtags);
  g_assert_true(dup->hashtags[0] != src->hashtags[0]);
  g_assert_cmpstr(dup->hashtags[0], ==, src->hashtags[0]);
  g_assert_cmpstr(dup->hashtags[1], ==, src->hashtags[1]);
  g_assert_true(gnostr_timeline_query_equal(src, dup));

  gnostr_timeline_query_free(src);
  gnostr_timeline_query_free(dup);
}

/* Test: Global query (no event_ids) */
static void test_global_query_no_event_ids(void) {
  GNostrTimelineQuery *query = gnostr_timeline_query_new_global();

  g_assert_nonnull(query);
  g_assert_cmpuint(query->n_event_ids, ==, 0);
  g_assert_null(query->event_ids);

  const char *json = gnostr_timeline_query_to_json(query);
  g_assert_null(strstr(json, "\"#e\":"));

  gnostr_timeline_query_free(query);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/timeline_query/thread_event_filter", test_thread_query_event_filter);
  g_test_add_func("/timeline_query/builder_event_id", test_builder_event_id);
  g_test_add_func("/timeline_query/multiple_event_ids", test_multiple_event_ids);
  g_test_add_func("/timeline_query/copy_event_ids", test_query_copy_event_ids);
  g_test_add_func("/timeline_query/equal_event_ids", test_query_equal_event_ids);
  g_test_add_func("/timeline_query/not_equal_different_event_ids", test_query_not_equal_different_event_ids);
  g_test_add_func("/timeline_query/global_no_event_ids", test_global_query_no_event_ids);
  g_test_add_func("/timeline_query/multi_hashtag_builder", test_multi_hashtag_builder);
  g_test_add_func("/timeline_query/hashtag_mutex", test_hashtag_mutex);
  g_test_add_func("/timeline_query/multi_hashtag_copy", test_multi_hashtag_copy);

  return g_test_run();
}
