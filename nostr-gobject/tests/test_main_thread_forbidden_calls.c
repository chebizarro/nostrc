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

/* ── Test: Heartbeat is not starved during operations ─────────────── */

#define HEARTBEAT_CYCLES 50
#define HEARTBEAT_GAP_US (MAX_MAIN_THREAD_OP_MS * G_TIME_SPAN_MILLISECOND)

typedef void (*HeartbeatWork)(gpointer data, guint cycle);

typedef struct {
    guint heartbeat_count;
    guint missed_count;
    gint64 last_beat_us;
    gint64 max_gap_us;
    guint work_count;
    gint64 work_us[HEARTBEAT_CYCLES];
    gint64 gap_us[HEARTBEAT_CYCLES];
    HeartbeatWork work;
    gpointer work_data;
    GMainLoop *loop;
} HeartbeatData;

static void
heartbeat_record_at(HeartbeatData *hb, gint64 now)
{
    gint64 gap = now - hb->last_beat_us;
    hb->max_gap_us = MAX(hb->max_gap_us, gap);
    /* Preserve the original strict >50ms boundary and two-gap allowance. */
    if (gap > HEARTBEAT_GAP_US)
        hb->missed_count++;
    hb->gap_us[hb->heartbeat_count++] = gap;
    hb->last_beat_us = now;
}

static void
heartbeat_do_work(HeartbeatData *hb)
{
    g_assert_true(g_thread_self() == main_thread);
    gint64 start = g_get_monotonic_time();
    hb->work(hb->work_data, hb->work_count);
    hb->work_us[hb->work_count++] = g_get_monotonic_time() - start;
}

static gboolean
heartbeat_cb(gpointer user_data)
{
    HeartbeatData *hb = user_data;
    g_assert_true(g_thread_self() == main_thread);
    heartbeat_record_at(hb, g_get_monotonic_time());
    if (hb->work_count == HEARTBEAT_CYCLES) {
        g_main_loop_quit(hb->loop);
        return G_SOURCE_REMOVE;
    }
    heartbeat_do_work(hb);
    return G_SOURCE_CONTINUE;
}

static void
run_heartbeat(HeartbeatWork work, gpointer data)
{
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    HeartbeatData hb = {.work = work, .work_data = data, .loop = loop};
    g_timeout_add(10, heartbeat_cb, &hb);

    /* Anchor BEFORE the first operation, not at the first callback. Each
     * heartbeat observes the preceding cycle and dispatches the next one.
     * The final callback observes cycle 50 before ending the measurement. */
    hb.last_beat_us = g_get_monotonic_time();
    heartbeat_do_work(&hb);
    g_main_loop_run(loop);

    g_test_message("Heartbeat: count=%u, missed=%u, max_gap=%.1f ms, cycles=%u",
                   hb.heartbeat_count, hb.missed_count,
                   hb.max_gap_us / 1000.0, hb.work_count);
    for (guint i = 0; i < hb.heartbeat_count; i++) {
        g_test_message("cycle=%u operation=%.3f ms observed_gap=%.3f ms%s",
                       i + 1, hb.work_us[i] / 1000.0, hb.gap_us[i] / 1000.0,
                       hb.gap_us[i] > HEARTBEAT_GAP_US ? " MISSED" : "");
    }
    if (g_test_subprocess()) {
        /* GLib suppresses g_test_message() in trapped children. Keep the
         * negative control's measured initial/final evidence on stderr. */
        g_printerr("blocking control: first_missed=%u second_missed=%u third_missed=%u missed=%u cycles=%u\n",
                   hb.gap_us[0] > HEARTBEAT_GAP_US,
                   hb.gap_us[1] > HEARTBEAT_GAP_US,
                   hb.gap_us[2] > HEARTBEAT_GAP_US,
                   hb.missed_count, hb.work_count);
    }
    g_assert_cmpuint(hb.work_count, ==, HEARTBEAT_CYCLES);
    g_assert_cmpuint(hb.heartbeat_count, ==, HEARTBEAT_CYCLES);
    g_assert_cmpuint(hb.heartbeat_count, >, 5);
    g_assert_cmpuint(hb.missed_count, <=, 2);
}

static void
store_cycle(gpointer data, guint cycle G_GNUC_UNUSED)
{
    GNostrStore *store = data;
    guint64 sub_id = gnostr_store_subscribe(store, "{\"kinds\":[1],\"limit\":10}");
    g_assert_cmpuint(sub_id, >, 0);
    guint64 keys[10];
    gnostr_store_poll_notes(store, sub_id, keys, 10);
    gnostr_store_unsubscribe(store, sub_id);
}

static void
test_heartbeat_not_starved(void)
{
    setup();
    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    run_heartbeat(store_cycle, store);
    teardown();
}

static void
blocking_cycle(gpointer data G_GNUC_UNUSED, guint cycle)
{
    if (cycle >= 3)
        return;
    /* Deliberately monopolize the main thread; no sleep/readiness surrogate.
     * Include cycle 1 to prove that the formerly blind initial delay counts. */
    gint64 deadline = g_get_monotonic_time() + HEARTBEAT_GAP_US + 10000;
    while (g_get_monotonic_time() < deadline) { }
}

static void
test_heartbeat_detects_blocking(void)
{
    if (g_test_subprocess()) {
        main_thread = g_thread_self();
        run_heartbeat(blocking_cycle, NULL);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_TIME_SPAN_SECOND, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*blocking control: first_missed=1 second_missed=1 third_missed=1 missed=* cycles=50*");
    g_test_trap_assert_stderr("*assertion failed (hb.missed_count <= 2)*");
}

static void
test_heartbeat_gap_boundary(void)
{
    HeartbeatData hb = {.last_beat_us = 1000000};
    heartbeat_record_at(&hb, 1500000); /* Initial 500ms must not be invisible. */
    g_assert_cmpuint(hb.missed_count, ==, 1);
    heartbeat_record_at(&hb, 1550000); /* Exactly 50ms is not a missed beat. */
    g_assert_cmpuint(hb.missed_count, ==, 1);
    heartbeat_record_at(&hb, 1600001);
    g_assert_cmpuint(hb.missed_count, ==, 2);
    heartbeat_record_at(&hb, 1650002);
    g_assert_cmpuint(hb.missed_count, ==, 3);
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

    g_test_add_func("/nostr-gobject/latency/heartbeat-detects-blocking",
                    test_heartbeat_detects_blocking);
    g_test_add_func("/nostr-gobject/latency/heartbeat-gap-boundary",
                    test_heartbeat_gap_boundary);

    return g_test_run();
}
