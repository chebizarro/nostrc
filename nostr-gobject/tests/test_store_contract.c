/**
 * test_store_contract.c — GNostrStore interface contract tests
 *
 * Verifies that GNostrNdbStore correctly implements the GNostrStore
 * interface contract: save, query, subscribe/poll, and lifecycle.
 *
 * Uses the testkit for temporary NDB instances and event fixtures.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/nostr_store.h>

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

/* ── Test: NDB store can be created and implements GNostrStore ─────── */
static void
test_ndb_store_implements_interface(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    g_assert_nonnull(store);
    g_assert_true(GNOSTR_IS_STORE(store));
    g_assert_true(GNOSTR_IS_NDB_STORE(store));

    teardown();
}

/* ── Test: Subscribe returns a valid subscription ID ──────────────── */
static void
test_store_subscribe_returns_id(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    const char *filter = "{\"kinds\":[1],\"limit\":10}";

    guint64 sub_id = gnostr_store_subscribe(GNOSTR_STORE(store), filter);
    /* Sub IDs should be > 0 on success */
    g_assert_cmpuint(sub_id, >, 0);

    /* Cleanup */
    gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_id);

    teardown();
}

/* ── Test: Poll on empty database returns 0 keys ──────────────────── */
static void
test_store_poll_empty(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    const char *filter = "{\"kinds\":[1],\"limit\":10}";

    guint64 sub_id = gnostr_store_subscribe(GNOSTR_STORE(store), filter);
    g_assert_cmpuint(sub_id, >, 0);

    guint64 keys[10];
    gint n = gnostr_store_poll_notes(GNOSTR_STORE(store), sub_id, keys, 10);
    g_assert_cmpint(n, ==, 0);

    gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_id);

    teardown();
}

/* ── Test: Store object can be created and destroyed in a loop ─────── */
static void
test_store_lifecycle_loop(void)
{
    setup();

    for (int i = 0; i < 50; i++) {
        GNostrNdbStore *store = gnostr_ndb_store_new();
        g_assert_nonnull(store);
        g_assert_true(GNOSTR_IS_STORE(store));
        g_object_unref(store);
    }

    teardown();
}

/* ── Test: Store finalization (weak ref verification) ─────────────── */
static void
test_store_finalizes_cleanly(void)
{
    setup();

    GNostrNdbStore *store = gnostr_ndb_store_new();
    GnTestPointerWatch *watch = gn_test_watch_object(G_OBJECT(store), "GNostrNdbStore");

    gn_test_assert_not_finalized(watch);
    g_object_unref(store);
    gn_test_assert_finalized(watch);

    g_free(watch);
    teardown();
}

/* ── Test: Get note by ID returns NULL for missing event ──────────── */
static void
test_store_get_missing_note(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    GError *error = NULL;

    /* Use a random hex ID that doesn't exist in the DB */
    char *json = gnostr_store_get_note_by_id(
        GNOSTR_STORE(store),
        "0000000000000000000000000000000000000000000000000000000000000000",
        &error);
    g_assert_null(json);
    g_clear_error(&error);

    teardown();
}

/* ── Test: Get profile by pubkey returns NULL for missing profile ──── */
static void
test_store_get_missing_profile(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    GError *error = NULL;

    char *json = gnostr_store_get_profile_by_pubkey(
        GNOSTR_STORE(store),
        "0000000000000000000000000000000000000000000000000000000000000000",
        &error);
    g_assert_null(json);
    g_clear_error(&error);

    teardown();
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gobject/store/implements-interface",
                    test_ndb_store_implements_interface);
    g_test_add_func("/nostr-gobject/store/subscribe-returns-id",
                    test_store_subscribe_returns_id);
    g_test_add_func("/nostr-gobject/store/poll-empty",
                    test_store_poll_empty);
    g_test_add_func("/nostr-gobject/store/lifecycle-loop",
                    test_store_lifecycle_loop);
    g_test_add_func("/nostr-gobject/store/finalizes-cleanly",
                    test_store_finalizes_cleanly);
    g_test_add_func("/nostr-gobject/store/get-missing-note",
                    test_store_get_missing_note);
    g_test_add_func("/nostr-gobject/store/get-missing-profile",
                    test_store_get_missing_profile);

    return g_test_run();
}
