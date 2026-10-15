/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <nostr-event.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/nostr_json.h>

static gchar *dbdir;
static gint notifications;

static void notify(void *ctx, uint64_t subid)
{
  g_assert_true(ctx == &notifications);
  g_assert_cmpuint(subid, !=, 0);
  g_atomic_int_inc(&notifications);
}

static void setup(gboolean with_notify)
{
  GError *error = NULL;
  dbdir = g_dir_make_tmp("gnostr-ingest-sync-XXXXXX", &error);
  g_assert_no_error(error);
  g_atomic_int_set(&notifications, 0);
  storage_ndb_set_notify_callback(with_notify ? notify : NULL,
                                  with_notify ? &notifications : NULL);
  g_assert_cmpint(storage_ndb_init(dbdir,
      "{\"mapsize\":67108864,\"ingester_threads\":1}", &error), ==, 1);
  g_assert_no_error(error);
}

static void teardown(void)
{
  storage_ndb_shutdown();
  storage_ndb_set_notify_callback(NULL, NULL);
  GDir *dir = g_dir_open(dbdir, 0, NULL);
  g_assert_nonnull(dir);
  const gchar *name;
  while ((name = g_dir_read_name(dir))) {
    g_autofree gchar *path = g_build_filename(dbdir, name, NULL);
    g_assert_cmpint(g_remove(path), ==, 0);
  }
  g_dir_close(dir);
  g_assert_cmpint(g_rmdir(dbdir), ==, 0);
  g_clear_pointer(&dbdir, g_free);
}

static char *make_event(const char *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 1);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_content(event, content);
  g_assert_cmpint(nostr_event_sign(event,
      "1111111111111111111111111111111111111111111111111111111111111111"), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_assert_nonnull(json);
  return json;
}

static void assert_queryable(const char *json)
{
  g_autofree gchar *id = gnostr_json_get_string(json, "id", NULL);
  g_autofree gchar *filter = g_strdup_printf("[{\"ids\":[\"%s\"]}]", id);
  void *txn = NULL;
  g_assert_cmpint(storage_ndb_begin_query(&txn, NULL), ==, 0);
  char **results = NULL;
  int count = 0;
  g_assert_cmpint(storage_ndb_query(txn, filter, &results, &count, NULL), ==, 0);
  g_assert_cmpint(count, ==, 1);
  g_autofree gchar *actual_id = gnostr_json_get_string(results[0], "id", NULL);
  g_assert_cmpstr(actual_id, ==, id);
  for (int i = 0; i < count; i++) g_free(results[i]);
  g_free(results);
  storage_ndb_end_query(txn);
}

static void test_immediate_query(void)
{
  setup(TRUE);
  g_autofree char *json = make_event("sync roundtrip");
  g_autofree gchar *id = gnostr_json_get_string(json, "id", NULL);
  g_autofree gchar *filter = g_strdup_printf("{\"ids\":[\"%s\"]}", id);
  uint64_t subid = storage_ndb_subscribe(filter);
  g_assert_cmpuint(subid, !=, 0);

  g_assert_cmpint(storage_ndb_ingest_event_json_sync(json), ==, 0);
  assert_queryable(json); /* No sleep, retry, or main-context iteration. */
  /* The internal wait must not replace or drain the application's subscription. */
  uint64_t note_key = 0;
  g_assert_cmpint(storage_ndb_poll_notes(subid, &note_key, 1), ==, 1);
  g_assert_cmpuint(note_key, !=, 0);
  g_assert_cmpint(g_atomic_int_get(&notifications), ==, 1);
  storage_ndb_unsubscribe(subid);

  /* nostrdb drops duplicates without another writer notification. */
  g_assert_cmpint(storage_ndb_ingest_event_json_sync(json), ==, 0);
  assert_queryable(json);
  /* A prior wrapper query leaves an idle TLS snapshot: the next ingest must
   * invalidate it so both the commit check and the next query see fresh data. */
  g_autofree char *next = make_event("after cached query");
  g_assert_cmpint(storage_ndb_ingest_event_json_sync(next), ==, 0);
  assert_queryable(next);
  teardown();
}

static gpointer ingest_thread(gpointer json)
{
  return GINT_TO_POINTER(storage_ndb_ingest_event_json_sync(json));
}

static void test_concurrent_duplicate(void)
{
  setup(FALSE);
  g_autofree char *json = make_event("concurrent sync roundtrip");
  GThread *a = g_thread_new("ingest-a", ingest_thread, json);
  GThread *b = g_thread_new("ingest-b", ingest_thread, json);
  g_assert_cmpint(GPOINTER_TO_INT(g_thread_join(a)), ==, 0);
  g_assert_cmpint(GPOINTER_TO_INT(g_thread_join(b)), ==, 0);
  assert_queryable(json);
  teardown();
}

static void test_rejected_event(void)
{
  setup(TRUE);
  g_autofree char *json = make_event("invalid signature");
  char *sig = strstr(json, "\"sig\":\"");
  g_assert_nonnull(sig);
  memset(sig + strlen("\"sig\":\""), '0', 128);
  gint64 start = g_get_monotonic_time();
  g_assert_cmpint(storage_ndb_ingest_event_json_sync(json), !=, 0);
  /* A dropped event must return a bounded failure, never queue-success. */
  g_assert_cmpint(g_get_monotonic_time() - start, <, 10 * G_TIME_SPAN_SECOND);
  g_assert_cmpint(storage_ndb_ingest_event_json_sync(NULL), !=, 0);
  g_assert_cmpint(storage_ndb_ingest_event_json_sync("{\"id\":\"bad\"}"), !=, 0);
  g_autofree char *valid = make_event("after rejected event");
  g_assert_cmpint(storage_ndb_ingest_event_json_sync(valid), ==, 0);
  assert_queryable(valid);
  teardown();
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ndb-ingest-sync/immediate-query-and-duplicate", test_immediate_query);
  g_test_add_func("/ndb-ingest-sync/concurrent-duplicate", test_concurrent_duplicate);
  g_test_add_func("/ndb-ingest-sync/rejected-event-bounded", test_rejected_event);
  return g_test_run();
}
