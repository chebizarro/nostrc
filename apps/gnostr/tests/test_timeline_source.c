#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "../src/model/gnostr-timeline-source.h"
#include "../src/model/gnostr-timeline-hydrator.h"
#include <nostr-event.h>
#include <nostr-tag.h>
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

/* ---- kind-6 reposts resolve their original in the snapshot (nostrc-4yt5v) */

#define REPOST_ORIGINAL_SK "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a"
#define REPOST_REPOSTER_SK "3a3e5e1f0b87d1c0e6a4b3f2d8a9c7e6f5d4c3b2a1908f7e6d5c4b3a29181716"

static char *
signed_event(const char *sk, int kind, gint64 created_at, const char *content,
             const char *e_tag, char **id_out)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, kind);
  nostr_event_set_created_at(ev, created_at);
  nostr_event_set_content(ev, content);
  if (e_tag)
    nostr_event_set_tags(ev, nostr_tags_new(1, nostr_tag_new("e", e_tag, NULL)));
  g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0);
  if (id_out)
    *id_out = nostr_event_get_id(ev);
  char *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  g_assert_nonnull(json);
  return json;
}

static char *
replace_once(const char *haystack, const char *needle, const char *with)
{
  const char *at = strstr(haystack, needle);
  g_assert_nonnull(at);
  return g_strdup_printf("%.*s%s%s", (int)(at - haystack), haystack, with,
                         at + strlen(needle));
}

static void
on_refresh_batch(GnostrTimelineSource *source G_GNUC_UNUSED,
                 GnostrTimelineBatch *batch,
                 gpointer user_data)
{
  GnostrTimelineBatch **out = user_data;
  if (gnostr_timeline_batch_get_kind(batch) == GNOSTR_TIMELINE_BATCH_REFRESH && !*out)
    *out = g_object_ref(batch);
}

static void
on_ids_stored(uint64_t subid G_GNUC_UNUSED,
              const uint64_t *note_keys G_GNUC_UNUSED,
              guint n_keys,
              gpointer user_data)
{
  *(guint *)user_data += n_keys;
}

static gboolean
on_refresh_deadline(gpointer user_data)
{
  *(gboolean *)user_data = TRUE;
  return G_SOURCE_REMOVE;
}

static GnostrTimelineItemViewModel *
hydrate_snapshot_entry(GnostrTimelineHydrator *hydrator,
                       GnostrTimelineBatch *batch,
                       const char *event_id)
{
  for (guint i = 0; i < gnostr_timeline_batch_get_n_entries(batch); i++) {
    const GnostrTimelineBatchEntry *entry = gnostr_timeline_batch_get_entry(batch, i);
    char hex[65];
    storage_ndb_hex_encode(entry->event_id, hex);
    if (g_strcmp0(hex, event_id) == 0)
      return gnostr_timeline_hydrator_hydrate_entry(hydrator, entry);
  }
  g_assert_not_reached();
  return NULL;
}

static void
test_snapshot_reposts_resolve_original(void)
{
  /* Original stored locally: the repost carries no embedded JSON. */
  g_autofree char *local_id = NULL;
  g_autofree char *local = signed_event(REPOST_ORIGINAL_SK, 1, 300,
                                        "stored original", NULL, &local_id);
  g_autofree char *repost_local_id = NULL;
  g_autofree char *repost_local = signed_event(REPOST_REPOSTER_SK, 6, 301, "",
                                               local_id, &repost_local_id);

  /* Original embedded and verifying (nostrdb also stores the embedded
   * original on its own; either way the snapshot resolves it). */
  g_autofree char *embedded_id = NULL;
  g_autofree char *embedded = signed_event(REPOST_ORIGINAL_SK, 1, 310,
                                           "embedded original", NULL, &embedded_id);
  g_autofree char *repost_embedded_id = NULL;
  g_autofree char *repost_embedded = signed_event(REPOST_REPOSTER_SK, 6, 311, embedded,
                                                  embedded_id, &repost_embedded_id);

  /* Wait for nostrdb's own commit notification, not a timer. */
  guint stored = 0;
  gboolean timed_out = FALSE;
  g_autofree char *ids_filter = g_strdup_printf(
    "{\"ids\":[\"%s\",\"%s\",\"%s\"]}",
    local_id, repost_local_id, repost_embedded_id);
  uint64_t subid = gn_ndb_subscribe(ids_filter, on_ids_stored, &stored, NULL);
  g_assert_cmpuint(subid, >, 0);
  guint deadline = g_timeout_add_seconds(10, on_refresh_deadline, &timed_out);
  g_assert_cmpint(storage_ndb_ingest_event_json(local, NULL), ==, 0);
  g_assert_cmpint(storage_ndb_ingest_event_json(repost_local, NULL), ==, 0);
  g_assert_cmpint(storage_ndb_ingest_event_json(repost_embedded, NULL), ==, 0);
  while (stored < 3 && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  gn_ndb_unsubscribe(subid);
  g_assert_cmpuint(stored, ==, 3);

  g_autoptr(GnostrTimelineSource) source = gnostr_timeline_source_new();
  GnostrTimelineBatch *batch = NULL;
  g_signal_connect(source, "batch", G_CALLBACK(on_refresh_batch), &batch);
  gnostr_timeline_source_refresh_async(source);
  while (!batch && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!timed_out)
    g_source_remove(deadline);
  g_assert_nonnull(batch);

  g_autoptr(GnostrTimelineHydrator) hydrator = gnostr_timeline_hydrator_new(1);
  g_autofree char *original_pubkey = NULL;
  {
    NostrEvent *ev = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_compact(ev, local, NULL), ==, 1);
    original_pubkey = g_strdup(nostr_event_get_pubkey(ev));
    nostr_event_free(ev);
  }

  g_autoptr(GnostrTimelineItemViewModel) from_ndb =
    hydrate_snapshot_entry(hydrator, batch, repost_local_id);
  g_assert_cmpint(gnostr_timeline_item_view_model_get_repost_state(from_ndb), ==,
                  GNOSTR_TIMELINE_PREVIEW_RESOLVED);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_event_id(from_ndb), ==, local_id);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_pubkey(from_ndb), ==, original_pubkey);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_content(from_ndb), ==, "stored original");
  g_assert_cmpint(gnostr_timeline_item_view_model_get_reposted_created_at(from_ndb), ==, 300);
  g_assert_cmpint(gnostr_timeline_item_view_model_get_reposted_kind(from_ndb), ==, 1);

  g_autoptr(GnostrTimelineItemViewModel) from_embedded =
    hydrate_snapshot_entry(hydrator, batch, repost_embedded_id);
  g_assert_cmpint(gnostr_timeline_item_view_model_get_repost_state(from_embedded), ==,
                  GNOSTR_TIMELINE_PREVIEW_RESOLVED);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_event_id(from_embedded), ==, embedded_id);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_pubkey(from_embedded), ==, original_pubkey);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_content(from_embedded), ==, "embedded original");
  g_assert_cmpint(gnostr_timeline_item_view_model_get_reposted_created_at(from_embedded), ==, 310);

  g_object_unref(batch);
}

/* The embedded fallback on its own: originals that are not in nostrdb (this
 * test DB skips validation, and nostrdb stores a kind 6's embedded original
 * itself, so these are never ingested). Only a verifying kind-1 event whose
 * id is the e-tagged one is trusted. */
static void
test_repost_embedded_original_must_verify(void)
{
  g_autofree char *id = NULL;
  g_autofree char *genuine = signed_event(REPOST_ORIGINAL_SK, 1, 410,
                                          "genuine words", NULL, &id);
  g_autofree char *pubkey = NULL;
  g_autofree char *content = NULL;
  gint64 created_at = 0;
  g_assert_true(gnostr_timeline_source_testing_resolve_repost(id, genuine, &pubkey,
                                                              &content, &created_at));
  g_assert_cmpuint(strlen(pubkey), ==, 64);
  g_assert_cmpstr(content, ==, "genuine words");
  g_assert_cmpint(created_at, ==, 410);
  g_clear_pointer(&pubkey, g_free);
  g_clear_pointer(&content, g_free);

  /* Tampered after signing. */
  g_autofree char *forged = replace_once(genuine, "genuine words", "forged words");
  g_assert_false(gnostr_timeline_source_testing_resolve_repost(id, forged, &pubkey,
                                                               &content, &created_at));
  g_assert_null(pubkey);
  g_assert_null(content);

  /* Valid, but not the event the repost's e tag names. */
  g_autofree char *other_id = NULL;
  g_autofree char *other = signed_event(REPOST_ORIGINAL_SK, 1, 411, "other", NULL, &other_id);
  g_assert_false(gnostr_timeline_source_testing_resolve_repost(id, other, &pubkey,
                                                               &content, &created_at));

  /* Valid and matching, but not a kind-1 note. */
  g_autofree char *meta_id = NULL;
  g_autofree char *meta = signed_event(REPOST_ORIGINAL_SK, 0, 412, "{}", NULL, &meta_id);
  g_assert_false(gnostr_timeline_source_testing_resolve_repost(meta_id, meta, &pubkey,
                                                               &content, &created_at));

  /* Nothing embedded and nothing stored. */
  g_assert_false(gnostr_timeline_source_testing_resolve_repost(id, "", &pubkey,
                                                               &content, &created_at));
  g_assert_null(pubkey);
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
  g_test_add_func("/gnostr/timeline-source/snapshot-reposts-resolve-original",
                  test_snapshot_reposts_resolve_original);
  g_test_add_func("/gnostr/timeline-source/repost-embedded-original-must-verify",
                  test_repost_embedded_original_must_verify);

  int result = g_test_run();
  storage_ndb_shutdown();
  g_free(test_db_dir);
  return result;
}
