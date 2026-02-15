/**
 * test_main_thread_forbidden_calls.c — Main-thread blocking detection
 *
 * Verifies that certain heavy operations (NDB queries, sleeps) are not
 * called from the main GLib thread context. This prevents UI stalling.
 *
 * Strategy:
 * - Records the main thread at test startup
 * - Calls potentially-blocking storage_ndb functions from the main thread
 * - Verifies that the operations complete within acceptable time budgets
 * - Uses timing to detect if any call blocks for too long
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/nostr_store.h>

static GnTestNdb *test_ndb = NULL;
static GThread *main_thread = NULL;

/* Maximum acceptable time for a main-thread-called NDB operation (ms) */
#define MAX_MAIN_THREAD_OP_MS 50

static void
setup(void)
{
    main_thread = g_thread_self();
    test_ndb = gn_test_ndb_new(NULL);
    g_assert_nonnull(test_ndb);
}

static void
teardown(void)
{
    gn_test_ndb_free(test_ndb);
    test_ndb = NULL;
}

/* ── Test: Store operations should complete within time budget ─────── */
static void
test_store_query_within_budget(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();

    /* Ingest some test events first */
    g_autoptr(GPtrArray) events = gn_test_make_events_bulk(100, 1, 1700000000);
    for (guint i = 0; i < events->len; i++) {
        gn_test_ndb_ingest_json(test_ndb, g_ptr_array_index(events, i));
    }

    /* Allow ingestion to complete */
    g_usleep(100000); /* 100ms for ingestion worker */

    /* Time the query operation on the main thread */
    gint64 start = g_get_monotonic_time();

    GError *error = NULL;
    char *json = gnostr_store_get_note_by_id(
        GNOSTR_STORE(store),
        "0000000000000000000000000000000000000000000000000000000000000000",
        &error);
    g_free(json);
    g_clear_error(&error);

    gint64 elapsed_us = g_get_monotonic_time() - start;
    double elapsed_ms = elapsed_us / 1000.0;

    g_test_message("get_note_by_id took %.2f ms (budget: %d ms)",
                   elapsed_ms, MAX_MAIN_THREAD_OP_MS);

    g_assert_cmpfloat(elapsed_ms, <, MAX_MAIN_THREAD_OP_MS);

    teardown();
}

/* ── Test: Subscribe/poll cycle within budget ─────────────────────── */
static void
test_subscribe_poll_within_budget(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();

    gint64 start = g_get_monotonic_time();

    guint64 sub_id = gnostr_store_subscribe(GNOSTR_STORE(store),
                                             "{\"kinds\":[1],\"limit\":50}");
    g_assert_cmpuint(sub_id, >, 0);

    guint64 keys[50];
    gnostr_store_poll_notes(GNOSTR_STORE(store), sub_id, keys, 50);

    gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_id);

    gint64 elapsed_us = g_get_monotonic_time() - start;
    double elapsed_ms = elapsed_us / 1000.0;

    g_test_message("Subscribe+poll+unsubscribe took %.2f ms (budget: %d ms)",
                   elapsed_ms, MAX_MAIN_THREAD_OP_MS);

    g_assert_cmpfloat(elapsed_ms, <, MAX_MAIN_THREAD_OP_MS);

    teardown();
}

/* ── Test: Batch operations within budget ─────────────────────────── */
static void
test_batch_operations_within_budget(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();

    const char *ids[] = {
        "0000000000000000000000000000000000000000000000000000000000000001",
        "0000000000000000000000000000000000000000000000000000000000000002",
        "0000000000000000000000000000000000000000000000000000000000000003",
        "0000000000000000000000000000000000000000000000000000000000000004",
        "0000000000000000000000000000000000000000000000000000000000000005",
        NULL
    };

    gint64 start = g_get_monotonic_time();

    GHashTable *reactions = gnostr_store_count_reactions_batch(
        GNOSTR_STORE(store), ids, 5);
    if (reactions) g_hash_table_unref(reactions);

    GHashTable *zaps = gnostr_store_get_zap_stats_batch(
        GNOSTR_STORE(store), ids, 5);
    if (zaps) g_hash_table_unref(zaps);

    gint64 elapsed_us = g_get_monotonic_time() - start;
    double elapsed_ms = elapsed_us / 1000.0;

    g_test_message("Batch reactions+zaps took %.2f ms (budget: %d ms)",
                   elapsed_ms, MAX_MAIN_THREAD_OP_MS);

    g_assert_cmpfloat(elapsed_ms, <, MAX_MAIN_THREAD_OP_MS);

    teardown();
}

/* ── Test: Heartbeat idle is not starved during operations ────────── */

typedef struct {
    guint heartbeat_count;
    guint missed_count;
    gint64 last_beat_us;
    gint64 max_gap_us;
} HeartbeatData;

static gboolean
heartbeat_cb(gpointer user_data)
{
    HeartbeatData *hb = user_data;
    gint64 now = g_get_monotonic_time();

    if (hb->last_beat_us > 0) {
        gint64 gap = now - hb->last_beat_us;
        if (gap > hb->max_gap_us) {
            hb->max_gap_us = gap;
        }
        /* If gap > 50ms, that's a missed beat indicating main thread stall */
        if (gap > 50000) {
            hb->missed_count++;
        }
    }

    hb->last_beat_us = now;
    hb->heartbeat_count++;
    return G_SOURCE_CONTINUE;
}

static void
test_heartbeat_not_starved(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();

    /* Install a 10ms heartbeat */
    HeartbeatData hb = {0};
    guint hb_id = g_timeout_add(10, heartbeat_cb, &hb);

    /* Do work that should NOT starve the heartbeat */
    for (int i = 0; i < 50; i++) {
        guint64 sub_id = gnostr_store_subscribe(GNOSTR_STORE(store),
                                                 "{\"kinds\":[1],\"limit\":10}");
        guint64 keys[10];
        gnostr_store_poll_notes(GNOSTR_STORE(store), sub_id, keys, 10);
        gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_id);

        /* Let the heartbeat fire */
        g_main_context_iteration(g_main_context_default(), FALSE);
    }

    /* Give it a few more iterations */
    for (int i = 0; i < 20; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
        g_usleep(5000);
    }

    g_source_remove(hb_id);

    g_test_message("Heartbeat: count=%u, missed=%u, max_gap=%.1f ms",
                   hb.heartbeat_count, hb.missed_count,
                   hb.max_gap_us / 1000.0);

    /* Heartbeat should have fired at least a few times */
    g_assert_cmpuint(hb.heartbeat_count, >, 5);

    /* No more than 2 missed beats (allow some slack for CI variance) */
    g_assert_cmpuint(hb.missed_count, <=, 2);

    teardown();
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gobject/latency/store-query-within-budget",
                    test_store_query_within_budget);
    g_test_add_func("/nostr-gobject/latency/subscribe-poll-within-budget",
                    test_subscribe_poll_within_budget);
    g_test_add_func("/nostr-gobject/latency/batch-operations-within-budget",
                    test_batch_operations_within_budget);
    g_test_add_func("/nostr-gobject/latency/heartbeat-not-starved",
                    test_heartbeat_not_starved);

    return g_test_run();
}
