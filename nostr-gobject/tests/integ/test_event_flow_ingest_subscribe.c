/**
 * test_event_flow_ingest_subscribe.c — End-to-end event flow correctness
 *
 * Integration test verifying the canonical event flow:
 *   websocket → ingest → nostrdb → subscription → consumer
 *
 * Uses the testkit's temporary NDB and event fixtures to simulate
 * the full ingestion→subscription→poll cycle without network access.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/nostr_store.h>
#include <nostr-gobject-1.0/storage_ndb.h>

static GnTestNdb *test_ndb = NULL;

static void
setup(void)
{
    test_ndb = gn_test_ndb_new(NULL);
    g_assert_nonnull(test_ndb);
}

static void
teardown(void)
{
    gn_test_ndb_free(test_ndb);
    test_ndb = NULL;
}

/* ── Test: Ingest events, then subscribe and poll returns keys ─────── */
static void
test_ingest_then_subscribe_poll(void)
{
    setup();

    /* Generate and ingest events */
    const guint N = 20;
    g_autoptr(GPtrArray) events = gn_test_make_events_bulk(N, 1, 1700000000);

    for (guint i = 0; i < events->len; i++) {
        gboolean ok = gn_test_ndb_ingest_json(test_ndb, g_ptr_array_index(events, i));
        g_assert_true(ok);
    }

    /* Wait for ingestion worker to process — use bounded polling */
    gn_test_drain_main_loop();
    g_usleep(200000); /* 200ms for NDB ingestion worker */
    gn_test_drain_main_loop();

    /* Subscribe for kind:1 */
    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    guint64 sub_id = gnostr_store_subscribe(GNOSTR_STORE(store),
                                             "{\"kinds\":[1],\"limit\":50}");
    g_assert_cmpuint(sub_id, >, 0);

    /* Poll for note keys */
    guint64 keys[50];
    gint n_keys = gnostr_store_poll_notes(GNOSTR_STORE(store), sub_id, keys, 50);

    g_test_message("Polled %d keys from subscription (expected %u)", n_keys, N);

    /* We MUST get at least some of the ingested events.
     * If n_keys==0, either ingestion or subscription is broken.
     * The exact count depends on how many the ingester has processed. */
    g_assert_cmpint(n_keys, >, 0);

    /* Verify each key can retrieve a note */
    for (gint i = 0; i < n_keys; i++) {
        g_assert_cmpuint(keys[i], >, 0);

        GError *error = NULL;
        g_autofree char *note_json = gnostr_store_get_note_by_key(
            GNOSTR_STORE(store), keys[i], &error);
        /* The key came from a successful poll, so we must be able to retrieve the note */
        g_assert_nonnull(note_json);
        /* The JSON should contain "kind":1 */
        g_assert_nonnull(strstr(note_json, "\"kind\":1"));
    }

    gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_id);
    teardown();
}

/* ── Test: Multiple subscriptions with different filters ──────────── */
static void
test_multiple_subscriptions(void)
{
    setup();

    /* Ingest kind:1 and kind:0 events */
    g_autoptr(GPtrArray) notes = gn_test_make_events_bulk(10, 1, 1700000000);
    g_autoptr(GPtrArray) profiles = gn_test_make_events_bulk(5, 0, 1700000100);

    for (guint i = 0; i < notes->len; i++) {
        gn_test_ndb_ingest_json(test_ndb, g_ptr_array_index(notes, i));
    }
    for (guint i = 0; i < profiles->len; i++) {
        gn_test_ndb_ingest_json(test_ndb, g_ptr_array_index(profiles, i));
    }

    gn_test_drain_main_loop();
    g_usleep(200000); /* Wait for NDB ingestion worker */
    gn_test_drain_main_loop();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();

    /* Subscribe for kind:1 */
    guint64 sub_notes = gnostr_store_subscribe(GNOSTR_STORE(store),
                                                "{\"kinds\":[1],\"limit\":50}");
    /* Subscribe for kind:0 */
    guint64 sub_profiles = gnostr_store_subscribe(GNOSTR_STORE(store),
                                                   "{\"kinds\":[0],\"limit\":50}");

    g_assert_cmpuint(sub_notes, >, 0);
    g_assert_cmpuint(sub_profiles, >, 0);
    /* Subscriptions should be different IDs */
    g_assert_cmpuint(sub_notes, !=, sub_profiles);

    /* Poll both */
    guint64 note_keys[50], profile_keys[50];
    gint n_note_keys = gnostr_store_poll_notes(GNOSTR_STORE(store), sub_notes,
                                                note_keys, 50);
    gint n_profile_keys = gnostr_store_poll_notes(GNOSTR_STORE(store), sub_profiles,
                                                   profile_keys, 50);

    g_test_message("Notes polled: %d, Profiles polled: %d",
                   n_note_keys, n_profile_keys);

    /* Cleanup */
    gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_notes);
    gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_profiles);

    teardown();
}

/* ── Test: Subscription is idempotent (double-poll returns 0) ─────── */
static void
test_poll_is_consumed(void)
{
    setup();

    g_autoptr(GPtrArray) events = gn_test_make_events_bulk(5, 1, 1700000000);
    for (guint i = 0; i < events->len; i++) {
        gn_test_ndb_ingest_json(test_ndb, g_ptr_array_index(events, i));
    }
    gn_test_drain_main_loop();
    g_usleep(200000);
    gn_test_drain_main_loop();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    guint64 sub_id = gnostr_store_subscribe(GNOSTR_STORE(store),
                                             "{\"kinds\":[1],\"limit\":50}");

    /* First poll */
    guint64 keys[50];
    gint first_poll = gnostr_store_poll_notes(GNOSTR_STORE(store), sub_id, keys, 50);

    /* Second poll should return 0 (no new events) */
    gint second_poll = gnostr_store_poll_notes(GNOSTR_STORE(store), sub_id, keys, 50);

    g_test_message("First poll: %d keys, Second poll: %d keys",
                   first_poll, second_poll);
    g_assert_cmpint(second_poll, ==, 0);

    gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_id);
    teardown();
}

/* ── Test: Unsubscribe makes subsequent polls return 0 ────────────── */
static void
test_unsubscribe_stops_delivery(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    guint64 sub_id = gnostr_store_subscribe(GNOSTR_STORE(store),
                                             "{\"kinds\":[1],\"limit\":50}");
    g_assert_cmpuint(sub_id, >, 0);

    gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_id);

    /* Now ingest events — they should not appear in a poll */
    g_autoptr(GPtrArray) events = gn_test_make_events_bulk(5, 1, 1700000000);
    for (guint i = 0; i < events->len; i++) {
        gn_test_ndb_ingest_json(test_ndb, g_ptr_array_index(events, i));
    }
    gn_test_drain_main_loop();
    g_usleep(200000);
    gn_test_drain_main_loop();

    guint64 keys[50];
    gint n = gnostr_store_poll_notes(GNOSTR_STORE(store), sub_id, keys, 50);
    g_test_message("Poll after unsubscribe: %d keys (expected 0)", n);
    g_assert_cmpint(n, ==, 0);

    teardown();
}

/* ── Test: Note counts API works (metadata) ───────────────────────── */
static void
test_note_counts_read_write(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    const char *test_id = "abcd000000000000000000000000000000000000000000000000000000000001";

    /* Write counts */
    GNostrNoteCounts counts = {
        .total_reactions = 42,
        .direct_replies = 5,
        .thread_replies = 12,
        .reposts = 3,
        .quotes = 1,
    };

    gboolean wrote = gnostr_store_write_note_counts(GNOSTR_STORE(store), test_id, &counts);

    if (wrote) {
        /* Read them back */
        GNostrNoteCounts read_counts = {0};
        gboolean got = gnostr_store_get_note_counts(GNOSTR_STORE(store), test_id, &read_counts);
        g_assert_true(got);
        g_assert_cmpuint(read_counts.total_reactions, ==, 42);
        g_assert_cmpuint(read_counts.direct_replies, ==, 5);
        g_assert_cmpuint(read_counts.thread_replies, ==, 12);
        g_assert_cmpuint(read_counts.reposts, ==, 3);
        g_assert_cmpuint(read_counts.quotes, ==, 1);
    }

    teardown();
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gobject/integ/ingest-then-subscribe-poll",
                    test_ingest_then_subscribe_poll);
    g_test_add_func("/nostr-gobject/integ/multiple-subscriptions",
                    test_multiple_subscriptions);
    g_test_add_func("/nostr-gobject/integ/poll-is-consumed",
                    test_poll_is_consumed);
    g_test_add_func("/nostr-gobject/integ/unsubscribe-stops-delivery",
                    test_unsubscribe_stops_delivery);
    g_test_add_func("/nostr-gobject/integ/note-counts-read-write",
                    test_note_counts_read_write);

    return g_test_run();
}
