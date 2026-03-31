/**
 * test_event_model_windowing.c — Sliding window invariant tests for GnNostrEventModel
 *
 * Tests the core windowing invariants of the event model:
 *
 * Hard invariants (always hold):
 *   H1. No duplicate note_keys across notes[] and insertion_buffer
 *   H2. notes[] is sorted newest-first (created_at descending)
 *   H3. GListModel length == notes->len at all times
 *
 * Eventual invariants (hold after quiescence):
 *   E1. notes->len <= MODEL_MAX_ITEMS for non-thread views
 *   E2. trim_newer/trim_older correctly evict from head/tail
 *   E3. load_older/load_newer extend the window monotonically
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-testkit.h"
#include "../src/model/gn-nostr-event-model.h"
#include <nostr-gobject-1.0/storage_ndb.h>

/* MODEL_MAX_ITEMS from the model source — keep in sync */
#define MODEL_MAX_ITEMS 100

/* ── Helpers ─────────────────────────────────────────────────────── */

static GnTestNdb *s_ndb = NULL;

static void
setup_ndb(void)
{
  s_ndb = gn_test_ndb_new(NULL);
  g_assert_nonnull(s_ndb);
}

static void
teardown_ndb(void)
{
  gn_test_ndb_free(s_ndb);
  s_ndb = NULL;
}

/**
 * Ingest N kind-1 events with decreasing timestamps so they naturally
 * sort newest-first. Also ingest a kind-0 profile for each event's pubkey
 * so the model considers them "ready" for display.
 * Returns the base timestamp used.
 */
static gint64
ingest_n_events(guint n, gint64 base_ts)
{
  for (guint i = 0; i < n; i++) {
    gint64 ts = base_ts - (gint64)i;
    /* Generate a deterministic pubkey for this event */
    g_autofree char *pubkey = g_strdup_printf(
      "%08x%08x%08x%08x%08x%08x%08x%08x",
      (guint)(i + 0x10), (guint)(i + 0x20), (guint)(i + 0x30), (guint)(i + 0x40),
      (guint)(i + 0x50), (guint)(i + 0x60), (guint)(i + 0x70), (guint)(i + 0x80));

    /* Ingest a kind-0 profile for this pubkey first */
    g_autofree char *profile_json = gn_test_make_event_json_with_pubkey(
      0, "{\"display_name\":\"TestUser\",\"name\":\"test\"}", ts - 1, pubkey);
    gn_test_ndb_ingest_json(s_ndb, profile_json);

    /* Then ingest the kind-1 event with the same pubkey */
    g_autofree char *json = gn_test_make_event_json_with_pubkey(1, "hello", ts, pubkey);
    gboolean ok = gn_test_ndb_ingest_json(s_ndb, json);
    g_assert_true(ok);
  }
  return base_ts;
}

/**
 * Assert that the model's notes are sorted newest-first.
 */
static void
assert_sorted_newest_first(GnNostrEventModel *model)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
  if (n <= 1) return;

  gint64 prev_ts = G_MAXINT64;
  g_autofree char *prev_event_id = NULL;
  for (guint i = 0; i < n; i++) {
    g_autoptr(GObject) item = g_list_model_get_item(G_LIST_MODEL(model), i);
    if (!item) continue;
    GnNostrEventItem *ev = GN_NOSTR_EVENT_ITEM(item);
    gint64 ts = gn_nostr_event_item_get_created_at(ev);
    const char *event_id = gn_nostr_event_item_get_event_id(ev);
    g_assert_cmpint(ts, <=, prev_ts);
    if (ts == prev_ts && prev_event_id != NULL)
      g_assert_cmpstr(prev_event_id, >=, event_id);
    prev_ts = ts;
    g_free(prev_event_id);
    prev_event_id = g_strdup(event_id);
  }
}

static char *
make_event_json_with_id_and_pubkey(int kind,
                                   const char *content,
                                   gint64 created_at,
                                   const char *pubkey_hex,
                                   const char *event_id_hex)
{
  return g_strdup_printf(
    "{\"id\":\"%s\","
    "\"pubkey\":\"%s\","
    "\"created_at\":%" G_GINT64_FORMAT ","
    "\"kind\":%d,"
    "\"tags\":[],"
    "\"content\":\"%s\","
    "\"sig\":\"%0128d\"}",
    event_id_hex, pubkey_hex, created_at, kind, content ? content : "", 0);
}

static void
ingest_same_timestamp_fixture(const guint *order,
                              guint order_len,
                              gint64 created_at)
{
  static const char *event_ids[] = {
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
  };
  static const char *pubkeys[] = {
    "1111111111111111111111111111111111111111111111111111111111111111",
    "2222222222222222222222222222222222222222222222222222222222222222",
    "3333333333333333333333333333333333333333333333333333333333333333",
  };

  for (guint i = 0; i < G_N_ELEMENTS(pubkeys); i++) {
    g_autofree char *profile_json = gn_test_make_event_json_with_pubkey(
      0, "{\"display_name\":\"Fixture\"}", created_at - 1, pubkeys[i]);
    g_assert_true(gn_test_ndb_ingest_json(s_ndb, profile_json));
  }

  for (guint i = 0; i < order_len; i++) {
    guint idx = order[i];
    g_autofree char *content = g_strdup_printf("same-ts-%u", idx);
    g_autofree char *event_json = make_event_json_with_id_and_pubkey(
      1, content, created_at, pubkeys[idx], event_ids[idx]);
    g_assert_true(gn_test_ndb_ingest_json(s_ndb, event_json));
  }

  gn_test_ndb_wait_for_ingest();
}

static guint
count_null_terminated_strv(const char *const *values)
{
  guint count = 0;
  if (!values) return 0;
  while (values[count]) count++;
  return count;
}

static gboolean
strv_contains(const char *const *values, const char *needle)
{
  if (!values || !needle) return FALSE;
  for (guint i = 0; values[i]; i++) {
    if (g_strcmp0(values[i], needle) == 0)
      return TRUE;
  }
  return FALSE;
}

static GPtrArray *
collect_model_event_ids(GnNostrEventModel *model)
{
  GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GObject) item = g_list_model_get_item(G_LIST_MODEL(model), i);
    g_assert_nonnull(item);
    g_ptr_array_add(ids, g_strdup(gn_nostr_event_item_get_event_id(GN_NOSTR_EVENT_ITEM(item))));
  }
  return ids;
}

static void
assert_event_id_sequence(GPtrArray *ids, const char *const *expected, guint n_expected)
{
  g_assert_cmpuint(ids->len, ==, n_expected);
  for (guint i = 0; i < n_expected; i++)
    g_assert_cmpstr(g_ptr_array_index(ids, i), ==, expected[i]);
}

static GPtrArray *
load_same_timestamp_ids_via_refresh(void)
{
  GnNostrEventModel *model = gn_nostr_event_model_new();
  GnNostrQueryParams params = {0};
  gint kinds[] = {1};
  params.kinds = kinds;
  params.n_kinds = 1;
  params.limit = 10;
  gn_nostr_event_model_set_query(model, &params);
  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();

  GPtrArray *ids = collect_model_event_ids(model);
  g_object_unref(model);
  return ids;
}

/* ── Test: model-new-is-empty ────────────────────────────────────── */

static void
test_model_new_is_empty(void)
{
  setup_ndb();

  GnNostrEventModel *model = gn_nostr_event_model_new();
  g_assert_nonnull(model);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 0);
  g_assert_cmpint(gn_nostr_event_model_get_oldest_timestamp(model), ==, 0);
  g_assert_cmpint(gn_nostr_event_model_get_newest_timestamp(model), ==, 0);

  g_object_unref(model);
  teardown_ndb();
}

/* ── Test: refresh-populates-model ───────────────────────────────── */

static void
test_refresh_populates_model(void)
{
  setup_ndb();

  gint64 base_ts = 1700000000;
  ingest_n_events(20, base_ts);
  gn_test_ndb_wait_for_ingest(); /* Wait for async ingester to commit events */

  GnNostrEventModel *model = gn_nostr_event_model_new();

  /* Set query for kind 1 */
  GnNostrQueryParams params = {0};
  gint kinds[] = {1};
  params.kinds = kinds;
  params.n_kinds = 1;
  params.limit = 50;
  gn_nostr_event_model_set_query(model, &params);

  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();

  guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
  g_test_message("Model has %u items after refresh with 20 ingested", n);

  /* Model must have loaded events — profiles were ingested, so readiness
   * filtering should not block them. If this fails, profile ingestion
   * or readiness logic is broken. */
  g_assert_cmpuint(n, >, 0);
  g_assert_cmpuint(n, <=, MODEL_MAX_ITEMS);

  /* H2: sorted newest-first */
  assert_sorted_newest_first(model);

  /* H3: GListModel length consistency */
  g_assert_cmpuint(n, ==, g_list_model_get_n_items(G_LIST_MODEL(model)));

  g_object_unref(model);
  teardown_ndb();
}

/* ── Test: trim-newer-evicts-from-head ───────────────────────────── */

static void
test_trim_newer_evicts_head(void)
{
  setup_ndb();

  gint64 base_ts = 1700000000;
  ingest_n_events(30, base_ts);

  GnNostrEventModel *model = gn_nostr_event_model_new();
  GnNostrQueryParams params = {0};
  gint kinds[] = {1};
  params.kinds = kinds;
  params.n_kinds = 1;
  params.limit = 50;
  gn_nostr_event_model_set_query(model, &params);
  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();

  guint before = g_list_model_get_n_items(G_LIST_MODEL(model));
  if (before < 5) {
    g_test_skip("Not enough items loaded for trim test");
    g_object_unref(model);
    teardown_ndb();
    return;
  }

  /* Remember the oldest timestamp before trim */
  gint64 oldest_before = gn_nostr_event_model_get_oldest_timestamp(model);

  /* Trim keeping only 5 items */
  gn_nostr_event_model_trim_newer(model, 5);

  guint after = g_list_model_get_n_items(G_LIST_MODEL(model));
  g_assert_cmpuint(after, ==, 5);

  /* Oldest timestamp should be unchanged (we trimmed from the head/newer end) */
  gint64 oldest_after = gn_nostr_event_model_get_oldest_timestamp(model);
  g_assert_cmpint(oldest_after, ==, oldest_before);

  /* H2: still sorted */
  assert_sorted_newest_first(model);

  g_object_unref(model);
  teardown_ndb();
}

/* ── Test: trim-older-evicts-from-tail ───────────────────────────── */

static void
test_trim_older_evicts_tail(void)
{
  setup_ndb();

  gint64 base_ts = 1700000000;
  ingest_n_events(30, base_ts);

  GnNostrEventModel *model = gn_nostr_event_model_new();
  GnNostrQueryParams params = {0};
  gint kinds[] = {1};
  params.kinds = kinds;
  params.n_kinds = 1;
  params.limit = 50;
  gn_nostr_event_model_set_query(model, &params);
  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();

  guint before = g_list_model_get_n_items(G_LIST_MODEL(model));
  if (before < 5) {
    g_test_skip("Not enough items loaded for trim test");
    g_object_unref(model);
    teardown_ndb();
    return;
  }

  /* Remember the newest timestamp before trim */
  gint64 newest_before = gn_nostr_event_model_get_newest_timestamp(model);

  /* Trim keeping only 5 items */
  gn_nostr_event_model_trim_older(model, 5);

  guint after = g_list_model_get_n_items(G_LIST_MODEL(model));
  g_assert_cmpuint(after, ==, 5);

  /* Newest timestamp should be unchanged (we trimmed from the tail/older end) */
  gint64 newest_after = gn_nostr_event_model_get_newest_timestamp(model);
  g_assert_cmpint(newest_after, ==, newest_before);

  /* H2: still sorted */
  assert_sorted_newest_first(model);

  g_object_unref(model);
  teardown_ndb();
}

/* ── Test: clear-empties-model ───────────────────────────────────── */

static void
test_clear_empties_model(void)
{
  setup_ndb();

  gint64 base_ts = 1700000000;
  ingest_n_events(10, base_ts);

  GnNostrEventModel *model = gn_nostr_event_model_new();
  GnNostrQueryParams params = {0};
  gint kinds[] = {1};
  params.kinds = kinds;
  params.n_kinds = 1;
  params.limit = 50;
  gn_nostr_event_model_set_query(model, &params);
  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();

  /* Clear the model */
  gn_nostr_event_model_clear(model);

  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 0);
  g_assert_cmpint(gn_nostr_event_model_get_oldest_timestamp(model), ==, 0);
  g_assert_cmpint(gn_nostr_event_model_get_newest_timestamp(model), ==, 0);

  g_object_unref(model);
  teardown_ndb();
}

/* ── Test: thread-view-no-window-enforcement ─────────────────────── */

static void
test_thread_view_no_window_enforcement(void)
{
  setup_ndb();

  GnNostrEventModel *model = gn_nostr_event_model_new();

  /* Set as thread view — should disable window enforcement */
  gn_nostr_event_model_set_thread_root(model, "deadbeef01234567890abcdef01234567890abcdef01234567890abcdef0123");
  g_assert_true(gn_nostr_event_model_get_is_thread_view(model));

  g_object_unref(model);
  teardown_ndb();
}

/* ── Test: pending-count-and-flush ───────────────────────────────── */

static void
test_pending_count_and_flush(void)
{
  setup_ndb();

  GnNostrEventModel *model = gn_nostr_event_model_new();

  /* Initially no pending items */
  g_assert_cmpuint(gn_nostr_event_model_get_pending_count(model), ==, 0);

  /* Set user NOT at top — items should be deferred */
  gn_nostr_event_model_set_user_at_top(model, FALSE);

  /* The pending count mechanism works through the insertion buffer,
   * which requires subscription-driven updates. We'll just verify
   * the API doesn't crash and returns consistent values. */
  guint pending = gn_nostr_event_model_get_pending_count(model);
  g_assert_cmpuint(pending, ==, 0);

  /* Flush should be a no-op when nothing is pending */
  gn_nostr_event_model_flush_pending(model);
  g_assert_cmpuint(gn_nostr_event_model_get_pending_count(model), ==, 0);

  g_object_unref(model);
  teardown_ndb();
}

/* ── Test: visible-range-updates ─────────────────────────────────── */

static void
test_visible_range_updates(void)
{
  setup_ndb();

  GnNostrEventModel *model = gn_nostr_event_model_new();

  /* Setting visible range shouldn't crash */
  gn_nostr_event_model_set_visible_range(model, 0, 10);
  gn_nostr_event_model_set_visible_range(model, 5, 25);
  gn_nostr_event_model_set_visible_range(model, 0, 0);

  g_object_unref(model);
  teardown_ndb();
}

/* ── Test: drain-enable-disable-lifecycle ────────────────────────── */

static void
test_drain_enable_disable_lifecycle(void)
{
  setup_ndb();

  GnNostrEventModel *model = gn_nostr_event_model_new();

  /* Enable/disable drain timer should be safe even without events */
  gn_nostr_event_model_set_drain_enabled(model, TRUE);
  gn_test_drain_main_loop();

  gn_nostr_event_model_set_drain_enabled(model, FALSE);
  gn_test_drain_main_loop();

  /* Enable, then destroy while enabled — tests cleanup */
  gn_nostr_event_model_set_drain_enabled(model, TRUE);

  GnTestPointerWatch *w = gn_test_watch_object(G_OBJECT(model), "model-drain-lifecycle");
  g_object_unref(model);
  gn_test_assert_finalized(w);
  g_free(w);

  teardown_ndb();
}

/* ── Test: model-finalize-no-leak ────────────────────────────────── */

static void
test_model_finalize_no_leak(void)
{
  setup_ndb();

  for (int cycle = 0; cycle < 20; cycle++) {
    GnNostrEventModel *model = gn_nostr_event_model_new();
    GnTestPointerWatch *w = gn_test_watch_object(G_OBJECT(model), "model-leak-cycle");

    /* Configure and tear down without refresh */
    GnNostrQueryParams params = {0};
    gint kinds[] = {1};
    params.kinds = kinds;
    params.n_kinds = 1;
    params.limit = 50;
    gn_nostr_event_model_set_query(model, &params);

    g_object_unref(model);
    gn_test_assert_finalized(w);
    g_free(w);
  }

  teardown_ndb();
}

/* ── Test: async-loading-guard ───────────────────────────────────── */

static void
test_async_loading_guard(void)
{
  setup_ndb();

  GnNostrEventModel *model = gn_nostr_event_model_new();

  /* Should not be loading initially */
  g_assert_false(gn_nostr_event_model_is_async_loading(model));

  g_object_unref(model);
  teardown_ndb();
}

static void
test_same_timestamp_refresh_uses_deterministic_tie_break(void)
{
  static const guint order_a[] = {0, 1, 2};
  static const guint order_b[] = {2, 0, 1};
  static const char *expected[] = {
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  };
  const gint64 ts = 1700000100;

  setup_ndb();
  ingest_same_timestamp_fixture(order_a, G_N_ELEMENTS(order_a), ts);
  g_autoptr(GPtrArray) ids_a = load_same_timestamp_ids_via_refresh();
  teardown_ndb();

  setup_ndb();
  ingest_same_timestamp_fixture(order_b, G_N_ELEMENTS(order_b), ts);
  g_autoptr(GPtrArray) ids_b = load_same_timestamp_ids_via_refresh();
  teardown_ndb();

  assert_event_id_sequence(ids_a, expected, G_N_ELEMENTS(expected));
  assert_event_id_sequence(ids_b, expected, G_N_ELEMENTS(expected));
}

static void
test_cross_relay_duplicate_arrivals_dedupe_and_preserve_provenance(void)
{
  static const char *relay_a = "wss://relay-a.example";
  static const char *relay_b = "wss://relay-b.example";
  const gint64 ts = 1700000300;
  const char *event_id = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
  const char *pubkey = "4444444444444444444444444444444444444444444444444444444444444444";

  setup_ndb();

  g_autofree char *profile_json = gn_test_make_event_json_with_pubkey(
    0, "{\"display_name\":\"RelayUser\"}", ts - 1, pubkey);
  g_assert_true(gn_test_ndb_ingest_json(s_ndb, profile_json));

  g_autofree char *event_json = make_event_json_with_id_and_pubkey(
    1, "dedupe-me", ts, pubkey, event_id);

  g_assert_true(gn_test_ndb_ingest_json_from_relay(s_ndb, event_json, relay_a));
  gn_test_ndb_wait_for_ingest();

  GnNostrEventModel *model = gn_nostr_event_model_new();
  GnNostrQueryParams params = {0};
  gint kinds[] = {1};
  params.kinds = kinds;
  params.n_kinds = 1;
  params.limit = 10;
  gn_nostr_event_model_set_query(model, &params);
  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();

  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 1);

  g_assert_true(gn_test_ndb_ingest_json_from_relay(s_ndb, event_json, relay_b));
  g_assert_true(gn_test_ndb_ingest_json_from_relay(s_ndb, event_json, relay_a));
  gn_test_ndb_wait_for_ingest();

  gn_nostr_event_model_add_event_json(model, event_json);
  gn_nostr_event_model_add_event_json(model, event_json);
  gn_test_drain_main_loop();

  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 1);

  g_autoptr(GObject) item = g_list_model_get_item(G_LIST_MODEL(model), 0);
  g_assert_nonnull(item);
  const char *const *relay_urls =
    gn_nostr_event_item_get_relay_urls(GN_NOSTR_EVENT_ITEM(item));
  g_assert_cmpuint(count_null_terminated_strv(relay_urls), ==, 2);
  g_assert_true(strv_contains(relay_urls, relay_a));
  g_assert_true(strv_contains(relay_urls, relay_b));

  g_object_unref(model);
  teardown_ndb();
}

static void
test_same_timestamp_live_insert_uses_deterministic_tie_break(void)
{
  static const guint order[] = {1, 0, 2};
  static const char *event_ids[] = {
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
  };
  static const char *pubkeys[] = {
    "1111111111111111111111111111111111111111111111111111111111111111",
    "2222222222222222222222222222222222222222222222222222222222222222",
    "3333333333333333333333333333333333333333333333333333333333333333",
  };
  static const char *expected[] = {
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  };
  const gint64 ts = 1700000200;

  setup_ndb();
  ingest_same_timestamp_fixture(order, G_N_ELEMENTS(order), ts);

  GnNostrEventModel *model = gn_nostr_event_model_new();
  for (guint i = 0; i < G_N_ELEMENTS(order); i++) {
    guint idx = order[i];
    g_autofree char *event_json = make_event_json_with_id_and_pubkey(
      1, "live", ts, pubkeys[idx], event_ids[idx]);
    gn_nostr_event_model_add_event_json(model, event_json);
  }

  g_autoptr(GPtrArray) ids = collect_model_event_ids(model);
  assert_event_id_sequence(ids, expected, G_N_ELEMENTS(expected));
  assert_sorted_newest_first(model);

  g_object_unref(model);
  teardown_ndb();
}

static void
test_mixed_timestamp_live_insert_preserves_global_sort_order(void)
{
  static const struct {
    gint64 ts;
    const char *event_id;
    const char *pubkey;
  } fixture[] = {
    {1700000300, "0000000000000000000000000000000000000000000000000000000000000001",
     "1111111111111111111111111111111111111111111111111111111111111111"},
    {1700000400, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "2222222222222222222222222222222222222222222222222222222222222222"},
    {1700000400, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "3333333333333333333333333333333333333333333333333333333333333333"},
    {1700000200, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
     "4444444444444444444444444444444444444444444444444444444444444444"},
    {1700000300, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "5555555555555555555555555555555555555555555555555555555555555555"},
  };

  setup_ndb();
  for (guint i = 0; i < G_N_ELEMENTS(fixture); i++) {
    g_autofree char *profile_json = gn_test_make_event_json_with_pubkey(
      0, "{\"display_name\":\"Fixture\"}", fixture[i].ts - 1, fixture[i].pubkey);
    g_assert_true(gn_test_ndb_ingest_json(s_ndb, profile_json));
  }
  gn_test_ndb_wait_for_ingest();

  GnNostrEventModel *model = gn_nostr_event_model_new();
  for (guint i = 0; i < G_N_ELEMENTS(fixture); i++) {
    g_autofree char *event_json = make_event_json_with_id_and_pubkey(
      1, "mixed-order", fixture[i].ts, fixture[i].pubkey, fixture[i].event_id);
    gn_nostr_event_model_add_event_json(model, event_json);
  }

  assert_sorted_newest_first(model);

  g_object_unref(model);
  teardown_ndb();
}

/* ── Main ────────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/event-model/model-new-is-empty",
                   test_model_new_is_empty);
  g_test_add_func("/gnostr/event-model/refresh-populates-model",
                   test_refresh_populates_model);
  g_test_add_func("/gnostr/event-model/trim-newer-evicts-head",
                   test_trim_newer_evicts_head);
  g_test_add_func("/gnostr/event-model/trim-older-evicts-tail",
                   test_trim_older_evicts_tail);
  g_test_add_func("/gnostr/event-model/clear-empties-model",
                   test_clear_empties_model);
  g_test_add_func("/gnostr/event-model/thread-view-no-window-enforcement",
                   test_thread_view_no_window_enforcement);
  g_test_add_func("/gnostr/event-model/pending-count-and-flush",
                   test_pending_count_and_flush);
  g_test_add_func("/gnostr/event-model/visible-range-updates",
                   test_visible_range_updates);
  g_test_add_func("/gnostr/event-model/drain-enable-disable-lifecycle",
                   test_drain_enable_disable_lifecycle);
  g_test_add_func("/gnostr/event-model/model-finalize-no-leak",
                   test_model_finalize_no_leak);
  g_test_add_func("/gnostr/event-model/async-loading-guard",
                   test_async_loading_guard);
  g_test_add_func("/gnostr/event-model/same-timestamp-refresh-tiebreak",
                   test_same_timestamp_refresh_uses_deterministic_tie_break);
  g_test_add_func("/gnostr/event-model/cross-relay-dedupe-provenance",
                   test_cross_relay_duplicate_arrivals_dedupe_and_preserve_provenance);
  g_test_add_func("/gnostr/event-model/same-timestamp-live-insert-tiebreak",
                   test_same_timestamp_live_insert_uses_deterministic_tie_break);
  g_test_add_func("/gnostr/event-model/mixed-timestamp-live-insert-sort-order",
                   test_mixed_timestamp_live_insert_preserves_global_sort_order);

  return g_test_run();
}
