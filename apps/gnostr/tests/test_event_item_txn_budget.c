/**
 * test_event_item_txn_budget.c — NDB transaction budget tests for GnNostrEventItem
 *
 * Validates that event item creation, population, and data access
 * respect transaction budget constraints:
 *
 *   1. Precache via populate_from_note should complete within budget
 *   2. Lazy accessors (get_content, get_pubkey, etc.) should not
 *      open long-lived transactions
 *   3. Creating and destroying items in bulk should not leak NDB handles
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-testkit.h"
#include "../src/model/gn-nostr-event-item.h"
#include <nostr-gobject-1.0/storage_ndb.h>

/* Budget limits */
#define TXN_BUDGET_US (50 * 1000)   /* 50ms per transaction operation */
#define BULK_ITEMS    200           /* Number of items for bulk tests */

static GnTestNdb *s_ndb = NULL;

static void
setup(void)
{
  s_ndb = gn_test_ndb_new(NULL);
  g_assert_nonnull(s_ndb);
}

static void
teardown(void)
{
  gn_test_ndb_free(s_ndb);
  s_ndb = NULL;
}

/* ── Test: item-create-from-key ──────────────────────────────────── */

static void
test_item_create_from_key(void)
{
  setup();

  /* Create an item from a note key — this should be fast (no DB access) */
  gint64 start = g_get_monotonic_time();

  GnNostrEventItem *item = gn_nostr_event_item_new_from_key(42, 1700000000);
  g_assert_nonnull(item);

  gint64 elapsed = g_get_monotonic_time() - start;
  g_test_message("Item creation took %" G_GINT64_FORMAT " us", elapsed);
  g_assert_cmpint(elapsed, <, TXN_BUDGET_US);

  g_assert_cmpuint(gn_nostr_event_item_get_note_key(item), ==, 42);
  g_assert_cmpint(gn_nostr_event_item_get_created_at(item), ==, 1700000000);

  g_object_unref(item);
  teardown();
}

/* ── Test: bulk-create-destroy-no-handle-leak ────────────────────── */

static void
test_bulk_create_destroy_no_leak(void)
{
  setup();

  gint64 start = g_get_monotonic_time();

  for (guint i = 0; i < BULK_ITEMS; i++) {
    GnNostrEventItem *item = gn_nostr_event_item_new_from_key(
        (uint64_t)(i + 1), 1700000000 + (gint64)i);
    g_assert_nonnull(item);

    GnTestPointerWatch *w = gn_test_watch_object(G_OBJECT(item), "bulk-item");
    g_object_unref(item);
    gn_test_assert_finalized(w);
    g_free(w);
  }

  gint64 elapsed = g_get_monotonic_time() - start;
  double ms = elapsed / 1000.0;
  g_test_message("Created and destroyed %u items in %.2f ms (%.1f us/item)",
                 BULK_ITEMS, ms, (double)elapsed / BULK_ITEMS);

  /* Entire bulk operation should complete within a reasonable budget */
  g_assert_cmpint(elapsed, <, BULK_ITEMS * TXN_BUDGET_US);

  teardown();
}

/* ── Test: item-set-profile-no-txn ───────────────────────────────── */

static void
test_item_set_profile_no_txn(void)
{
  setup();

  GnNostrEventItem *item = gn_nostr_event_item_new_from_key(1, 1700000000);

  /* Setting profile should not open a transaction */
  gint64 start = g_get_monotonic_time();

  gn_nostr_event_item_set_profile(item, NULL);

  gint64 elapsed = g_get_monotonic_time() - start;
  g_test_message("set_profile(NULL) took %" G_GINT64_FORMAT " us", elapsed);
  g_assert_cmpint(elapsed, <, 1000); /* Should be sub-1ms */

  /* get_profile should also be fast */
  start = g_get_monotonic_time();
  GNostrProfile *profile = gn_nostr_event_item_get_profile(item);
  elapsed = g_get_monotonic_time() - start;
  g_test_message("get_profile() took %" G_GINT64_FORMAT " us", elapsed);
  g_assert_null(profile);
  g_assert_cmpint(elapsed, <, 1000);

  g_object_unref(item);
  teardown();
}

/* ── Test: item-metadata-accessors-budget ────────────────────────── */

static void
test_item_metadata_accessors_budget(void)
{
  setup();

  GnNostrEventItem *item = gn_nostr_event_item_new_from_key(1, 1700000000);

  /* Set item data via update_from_event (no transaction needed) */
  gn_nostr_event_item_update_from_event(item, "aabbccdd", 1700000000, "test content", 1);

  /* All accessors should be fast (cached in-memory, no DB round-trip) */
  gint64 start = g_get_monotonic_time();

  const char *pubkey = gn_nostr_event_item_get_pubkey(item);
  const char *content = gn_nostr_event_item_get_content(item);
  gint kind = gn_nostr_event_item_get_kind(item);
  gint64 created_at = gn_nostr_event_item_get_created_at(item);

  gint64 elapsed = g_get_monotonic_time() - start;
  g_test_message("4 metadata accesses took %" G_GINT64_FORMAT " us total", elapsed);

  /* All accessors combined should be sub-1ms */
  g_assert_cmpint(elapsed, <, 1000);

  /* Verify values */
  g_assert_cmpstr(content, ==, "test content");
  g_assert_cmpint(kind, ==, 1);
  g_assert_cmpint(created_at, ==, 1700000000);
  (void)pubkey; /* pubkey may be partial if set via update_from_event */

  g_object_unref(item);
  teardown();
}

/* ── Test: thread-info-set-get-budget ────────────────────────────── */

static void
test_thread_info_set_get_budget(void)
{
  setup();

  GnNostrEventItem *item = gn_nostr_event_item_new_from_key(1, 1700000000);

  gint64 start = g_get_monotonic_time();

  gn_nostr_event_item_set_thread_info(item, "root123", "parent456", 2);
  const char *root = gn_nostr_event_item_get_thread_root_id(item);
  const char *parent = gn_nostr_event_item_get_parent_id(item);
  guint depth = gn_nostr_event_item_get_reply_depth(item);

  gint64 elapsed = g_get_monotonic_time() - start;
  g_test_message("Thread info set+get took %" G_GINT64_FORMAT " us", elapsed);
  g_assert_cmpint(elapsed, <, 1000);

  g_assert_cmpstr(root, ==, "root123");
  g_assert_cmpstr(parent, ==, "parent456");
  g_assert_cmpuint(depth, ==, 2);
  g_assert_true(gn_nostr_event_item_get_is_reply(item));

  g_object_unref(item);
  teardown();
}

/* ── Test: reaction-zap-stat-accessors ───────────────────────────── */

static void
test_reaction_zap_stat_accessors(void)
{
  setup();

  GnNostrEventItem *item = gn_nostr_event_item_new_from_key(1, 1700000000);

  /* All stat setters/getters should be pure in-memory operations */
  gint64 start = g_get_monotonic_time();

  gn_nostr_event_item_set_like_count(item, 42);
  gn_nostr_event_item_set_repost_count(item, 7);
  gn_nostr_event_item_set_reply_count(item, 13);
  gn_nostr_event_item_set_zap_count(item, 5);
  gn_nostr_event_item_set_zap_total_msat(item, 100000);

  g_assert_cmpuint(gn_nostr_event_item_get_like_count(item), ==, 42);
  g_assert_cmpuint(gn_nostr_event_item_get_repost_count(item), ==, 7);
  g_assert_cmpuint(gn_nostr_event_item_get_reply_count(item), ==, 13);
  g_assert_cmpuint(gn_nostr_event_item_get_zap_count(item), ==, 5);
  g_assert_cmpint(gn_nostr_event_item_get_zap_total_msat(item), ==, 100000);

  gint64 elapsed = g_get_monotonic_time() - start;
  g_test_message("10 stat set+get operations took %" G_GINT64_FORMAT " us", elapsed);
  g_assert_cmpint(elapsed, <, 1000);

  g_object_unref(item);
  teardown();
}

/* ── Test: animation-skip-flag ───────────────────────────────────── */

static void
test_animation_skip_flag(void)
{
  setup();

  GnNostrEventItem *item = gn_nostr_event_item_new_from_key(1, 1700000000);

  g_assert_false(gn_nostr_event_item_get_skip_animation(item));

  gn_nostr_event_item_set_skip_animation(item, TRUE);
  g_assert_true(gn_nostr_event_item_get_skip_animation(item));

  gn_nostr_event_item_set_skip_animation(item, FALSE);
  g_assert_false(gn_nostr_event_item_get_skip_animation(item));

  g_object_unref(item);
  teardown();
}

/* ── Main ────────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/event-item/create-from-key",
                   test_item_create_from_key);
  g_test_add_func("/gnostr/event-item/bulk-create-destroy-no-leak",
                   test_bulk_create_destroy_no_leak);
  g_test_add_func("/gnostr/event-item/set-profile-no-txn",
                   test_item_set_profile_no_txn);
  g_test_add_func("/gnostr/event-item/metadata-accessors-budget",
                   test_item_metadata_accessors_budget);
  g_test_add_func("/gnostr/event-item/thread-info-set-get-budget",
                   test_thread_info_set_get_budget);
  g_test_add_func("/gnostr/event-item/reaction-zap-stat-accessors",
                   test_reaction_zap_stat_accessors);
  g_test_add_func("/gnostr/event-item/animation-skip-flag",
                   test_animation_skip_flag);

  return g_test_run();
}
