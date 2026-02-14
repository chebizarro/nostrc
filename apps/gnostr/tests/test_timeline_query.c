/**
 * GnTimelineQuery Unit Tests
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

  GnTimelineQuery *query = gn_timeline_query_new_thread(root_id);

  g_assert_nonnull(query);
  g_assert_cmpuint(query->n_event_ids, ==, 1);
  g_assert_cmpstr(query->event_ids[0], ==, root_id);

  /* Verify JSON output contains #e filter */
  const char *json = gn_timeline_query_to_json(query);
  g_assert_nonnull(json);
  g_assert_true(strstr(json, "\"#e\":[") != NULL);
  g_assert_true(strstr(json, root_id) != NULL);

  /* Should NOT use old hashtag workaround */
  g_assert_null(query->hashtag);

  gn_timeline_query_free(query);
}

/* Test: Builder with event_id generates proper #e filter */
static void test_builder_event_id(void) {
  const char *event_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

  GnTimelineQueryBuilder *builder = gn_timeline_query_builder_new();
  gn_timeline_query_builder_add_kind(builder, 1);
  gn_timeline_query_builder_add_event_id(builder, event_id);
  GnTimelineQuery *query = gn_timeline_query_builder_build(builder);

  g_assert_nonnull(query);
  g_assert_cmpuint(query->n_event_ids, ==, 1);
  g_assert_cmpstr(query->event_ids[0], ==, event_id);

  const char *json = gn_timeline_query_to_json(query);
  g_assert_true(strstr(json, "\"#e\":[") != NULL);
  g_assert_true(strstr(json, event_id) != NULL);

  gn_timeline_query_free(query);
}

/* Test: Multiple event IDs in filter */
static void test_multiple_event_ids(void) {
  const char *id1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *id3 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

  GnTimelineQueryBuilder *builder = gn_timeline_query_builder_new();
  gn_timeline_query_builder_add_kind(builder, 1);
  gn_timeline_query_builder_add_event_id(builder, id1);
  gn_timeline_query_builder_add_event_id(builder, id2);
  gn_timeline_query_builder_add_event_id(builder, id3);
  GnTimelineQuery *query = gn_timeline_query_builder_build(builder);

  g_assert_cmpuint(query->n_event_ids, ==, 3);
  g_assert_cmpstr(query->event_ids[0], ==, id1);
  g_assert_cmpstr(query->event_ids[1], ==, id2);
  g_assert_cmpstr(query->event_ids[2], ==, id3);

  const char *json = gn_timeline_query_to_json(query);
  g_assert_true(strstr(json, id1) != NULL);
  g_assert_true(strstr(json, id2) != NULL);
  g_assert_true(strstr(json, id3) != NULL);

  gn_timeline_query_free(query);
}

/* Test: Query copy preserves event_ids */
static void test_query_copy_event_ids(void) {
  const char *root_id = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

  GnTimelineQuery *original = gn_timeline_query_new_thread(root_id);
  GnTimelineQuery *copy = gn_timeline_query_copy(original);

  g_assert_nonnull(copy);
  g_assert_cmpuint(copy->n_event_ids, ==, 1);
  g_assert_cmpstr(copy->event_ids[0], ==, root_id);

  /* Verify they're independent (deep copy) */
  g_assert_true(copy->event_ids != original->event_ids);
  g_assert_true(copy->event_ids[0] != original->event_ids[0]);

  gn_timeline_query_free(original);
  gn_timeline_query_free(copy);
}

/* Test: Query equality with event_ids */
static void test_query_equal_event_ids(void) {
  const char *root_id = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

  GnTimelineQuery *q1 = gn_timeline_query_new_thread(root_id);
  GnTimelineQuery *q2 = gn_timeline_query_new_thread(root_id);

  g_assert_true(gn_timeline_query_equal(q1, q2));
  g_assert_cmpuint(gn_timeline_query_hash(q1), ==, gn_timeline_query_hash(q2));

  gn_timeline_query_free(q1);
  gn_timeline_query_free(q2);
}

/* Test: Different event_ids are not equal */
static void test_query_not_equal_different_event_ids(void) {
  const char *id1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

  GnTimelineQuery *q1 = gn_timeline_query_new_thread(id1);
  GnTimelineQuery *q2 = gn_timeline_query_new_thread(id2);

  g_assert_false(gn_timeline_query_equal(q1, q2));

  gn_timeline_query_free(q1);
  gn_timeline_query_free(q2);
}

/* Test: Global query (no event_ids) */
static void test_global_query_no_event_ids(void) {
  GnTimelineQuery *query = gn_timeline_query_new_global();

  g_assert_nonnull(query);
  g_assert_cmpuint(query->n_event_ids, ==, 0);
  g_assert_null(query->event_ids);

  const char *json = gn_timeline_query_to_json(query);
  g_assert_null(strstr(json, "\"#e\":"));

  gn_timeline_query_free(query);
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

  return g_test_run();
}
