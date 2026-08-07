#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "../src/model/gnostr-timeline-source.h"
#include <nostr-gobject-1.0/gn-ndb-sub-dispatcher.h>
#include <nostr-gobject-1.0/storage_ndb.h>

static char *test_db_dir;

static void
test_scoped_subscription_filter_tracks_query(void)
{
  g_autoptr(GnostrTimelineSource) source = gnostr_timeline_source_new();

  GNostrTimelineQuery *author =
    gnostr_timeline_query_new_for_author(
      "1111111111111111111111111111111111111111111111111111111111111111");
  gnostr_timeline_source_set_query(source, author);
  g_autofree char *author_filter =
    gnostr_timeline_source_testing_dup_filter(source);
  g_assert_nonnull(strstr(author_filter, "\"authors\""));
  g_assert_nonnull(strstr(author_filter,
    "1111111111111111111111111111111111111111111111111111111111111111"));
  gnostr_timeline_query_free(author);

  GNostrTimelineQuery *hashtag =
    gnostr_timeline_query_new_for_hashtag("gnome");
  gnostr_timeline_source_set_query(source, hashtag);
  g_autofree char *hashtag_filter =
    gnostr_timeline_source_testing_dup_filter(source);
  g_assert_nonnull(strstr(hashtag_filter, "\"#t\":[\"gnome\"]"));
  gnostr_timeline_query_free(hashtag);

  GNostrTimelineQuery *search =
    gnostr_timeline_query_new_for_search("timeline performance");
  gnostr_timeline_source_set_query(source, search);
  g_autofree char *search_filter =
    gnostr_timeline_source_testing_dup_filter(source);
  g_assert_nonnull(strstr(search_filter, "\"search\":\"timeline performance\""));
  g_assert_null(strstr(search_filter, "{\"kinds\":[1,6,9735]}"));
  gnostr_timeline_query_free(search);
}

typedef struct {
  gboolean seen;
  guint n_entries;
} RefreshCompletion;

static void
on_refresh_completion(GnostrTimelineSource *source G_GNUC_UNUSED,
                      GnostrTimelineBatch *batch,
                      gpointer user_data)
{
  RefreshCompletion *completion = user_data;
  if (gnostr_timeline_batch_get_kind(batch) != GNOSTR_TIMELINE_BATCH_REFRESH)
    return;
  completion->seen = TRUE;
  completion->n_entries = gnostr_timeline_batch_get_n_entries(batch);
}

static void
test_empty_refresh_is_emitted_as_completion(void)
{
  g_autoptr(GnostrTimelineSource) source = gnostr_timeline_source_new();
  RefreshCompletion completion = {0};
  g_signal_connect(source, "batch", G_CALLBACK(on_refresh_completion),
                   &completion);

  gnostr_timeline_source_refresh_async(source);
  gint64 deadline = g_get_monotonic_time() + (5 * G_TIME_SPAN_SECOND);
  while (!completion.seen && g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
  }

  g_assert_true(completion.seen);
  g_assert_cmpuint(completion.n_entries, ==, 0);
}

static void
test_live_key_queue_is_deduplicated_and_bounded(void)
{
  g_autoptr(GnostrTimelineSource) source = gnostr_timeline_source_new();
  guint capacity = gnostr_timeline_source_testing_get_queue_capacity();
  guint n_keys = capacity + 50;
  g_autofree uint64_t *keys = g_new(uint64_t, n_keys);

  for (guint i = 0; i < n_keys; i++)
    keys[i] = i + 1;
  gnostr_timeline_source_testing_enqueue_live_keys(source, keys, n_keys);
  gnostr_timeline_source_testing_enqueue_live_keys(source,
                                                    keys + n_keys - 10,
                                                    10);

  g_assert_cmpuint(
    gnostr_timeline_source_testing_get_live_pending_count(source),
    ==, capacity);
  g_assert_cmpuint(
    gnostr_timeline_source_testing_get_live_dropped_count(source),
    ==, 50);
}

static guint
recompute_count(const char *filter)
{
  void *txn = NULL;
  g_assert_cmpint(storage_ndb_begin_query(&txn, NULL), ==, 0);
  char **results = NULL;
  int count = 0;
  g_assert_cmpint(storage_ndb_query(txn, filter, &results, &count, NULL), ==, 0);
  storage_ndb_free_results(results, count);
  storage_ndb_end_query(txn);
  return (guint)count;
}

static gboolean
wait_for_note_meta(const char *event_id,
                   StorageNdbNoteCounts *out)
{
  gint64 deadline = g_get_monotonic_time() + (5 * G_TIME_SPAN_SECOND);
  while (g_get_monotonic_time() < deadline) {
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
      gboolean found = storage_ndb_read_note_counts_hex(txn, event_id, out);
      storage_ndb_end_query(txn);
      if (found && out->total_reactions == 2 && out->direct_replies == 1)
        return TRUE;
    }
    g_usleep(10000);
  }
  return FALSE;
}

static void
test_local_ingest_populates_note_meta_equivalent_to_recompute(void)
{
  static const char *root_id =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  static const char *sig =
    "1111111111111111111111111111111111111111111111111111111111111111"
    "1111111111111111111111111111111111111111111111111111111111111111";
  static const char *pubkey =
    "2222222222222222222222222222222222222222222222222222222222222222";

  g_autofree char *root = g_strdup_printf(
    "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":100,"
    "\"kind\":1,\"tags\":[],\"content\":\"root\",\"sig\":\"%s\"}",
    root_id, pubkey, sig);
  g_autofree char *reaction_one = g_strdup_printf(
    "{\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
    "\"pubkey\":\"%s\",\"created_at\":101,\"kind\":7,"
    "\"tags\":[[\"e\",\"%s\"]],\"content\":\"+\",\"sig\":\"%s\"}",
    pubkey, root_id, sig);
  g_autofree char *reaction_two = g_strdup_printf(
    "{\"id\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
    "\"pubkey\":\"%s\",\"created_at\":102,\"kind\":7,"
    "\"tags\":[[\"e\",\"%s\"]],\"content\":\"❤️\",\"sig\":\"%s\"}",
    pubkey, root_id, sig);
  g_autofree char *reply = g_strdup_printf(
    "{\"id\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\","
    "\"pubkey\":\"%s\",\"created_at\":103,\"kind\":1,"
    "\"tags\":[[\"e\",\"%s\",\"\",\"root\"]],"
    "\"content\":\"reply\",\"sig\":\"%s\"}",
    pubkey, root_id, sig);

  g_assert_cmpint(storage_ndb_ingest_event_json(root, NULL), ==, 0);
  g_assert_cmpint(storage_ndb_ingest_event_json(reaction_one, NULL), ==, 0);
  g_assert_cmpint(storage_ndb_ingest_event_json(reaction_two, NULL), ==, 0);
  g_assert_cmpint(storage_ndb_ingest_event_json(reply, NULL), ==, 0);

  StorageNdbNoteCounts counts = {0};
  g_assert_true(wait_for_note_meta(root_id, &counts));
  g_assert_cmpuint(counts.total_reactions, ==, 2);
  g_assert_cmpuint(counts.direct_replies, ==, 1);
  g_assert_cmpuint(counts.reposts, ==, 0);

  g_autofree char *reaction_filter = g_strdup_printf(
    "{\"kinds\":[7],\"#e\":[\"%s\"]}", root_id);
  g_autofree char *reply_filter = g_strdup_printf(
    "{\"kinds\":[1],\"#e\":[\"%s\"]}", root_id);
  g_assert_cmpuint(counts.total_reactions, ==,
                   recompute_count(reaction_filter));
  g_assert_cmpuint(counts.direct_replies, ==,
                   recompute_count(reply_filter));
}

int
main(int argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_setenv("NOSTR_ALLOW_UNSAFE_INGEST", "1", TRUE);
  gn_ndb_dispatcher_init();

  g_autoptr(GError) error = NULL;
  test_db_dir = g_dir_make_tmp("gnostr-timeline-source-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(test_db_dir);
  g_assert_true(storage_ndb_init(
    test_db_dir,
    "{\"mapsize\":67108864,\"ingester_threads\":1,\"ingest_skip_validation\":1}",
    &error));
  g_assert_no_error(error);

  g_test_add_func("/gnostr/timeline-source/scoped-subscription-filter",
                  test_scoped_subscription_filter_tracks_query);
  g_test_add_func("/gnostr/timeline-source/empty-refresh-completion",
                  test_empty_refresh_is_emitted_as_completion);
  g_test_add_func("/gnostr/timeline-source/coalesced-queue-bound",
                  test_live_key_queue_is_deduplicated_and_bounded);
  g_test_add_func("/gnostr/timeline-source/note-meta-local-ingest-equivalence",
                  test_local_ingest_populates_note_meta_equivalent_to_recompute);

  int result = g_test_run();
  storage_ndb_shutdown();
  g_free(test_db_dir);
  return result;
}
