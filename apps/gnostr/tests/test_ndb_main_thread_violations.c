/**
 * test_ndb_main_thread_violations.c — Detect NDB transactions on the main thread
 *
 * This is the CORE architectural test. It exercises real code paths
 * (GnNostrEventModel, GnNostrEventItem, NoteCardFactory) and detects
 * when any NDB read transaction is opened on the GTK main thread.
 *
 * Every NDB transaction on the main thread is a potential source of:
 *   - UI stalls (usleep in retry path, LMDB contention)
 *   - Latency during scroll (synchronous data fetch during bind)
 *   - Cascading segfaults (stale data after transaction ends)
 *
 * The test uses storage_ndb's GNOSTR_TESTING instrumentation which
 * records each begin_query/begin_query_retry call made on the marked
 * main thread. Tests assert zero violations after exercising each path.
 *
 * MUST be compiled with -DGNOSTR_TESTING for instrumentation to activate.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <glib.h>
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include "model/gn-nostr-event-model.h"
#include "model/gn-nostr-event-item.h"

/* ── Test fixture ─────────────────────────────────────────────────── */

typedef struct {
    GnTestNdb *ndb;
    GPtrArray *pubkeys;
} ViolationFixture;

static void
fixture_setup(ViolationFixture *f, gconstpointer data G_GNUC_UNUSED)
{
    f->ndb = gn_test_ndb_new(NULL);
    g_assert_nonnull(f->ndb);

    /* Ingest a realistic corpus with profiles */
    f->pubkeys = gn_test_ingest_realistic_corpus(f->ndb, 200, 20);
    g_assert_nonnull(f->pubkeys);

    /* Mark this thread as the main thread for violation detection */
    gn_test_mark_main_thread();
    gn_test_reset_ndb_violations();
}

static void
fixture_teardown(ViolationFixture *f, gconstpointer data G_GNUC_UNUSED)
{
    gn_test_clear_main_thread();
    g_ptr_array_unref(f->pubkeys);
    gn_test_ndb_free(f->ndb);
}

/* ── Test: Event item lazy load triggers NDB txn on main thread ───── */

/**
 * This test exercises the EXACT crash/latency path:
 *
 * 1. GnNostrEventItem is created with a note_key
 * 2. get_content() is called (from GTK factory bind)
 * 3. This calls ensure_note_loaded() → storage_ndb_begin_query()
 * 4. Under instrumentation, this records a main-thread violation
 *
 * The test is EXPECTED TO FIND VIOLATIONS in the current code.
 * When the violations are fixed (by moving NDB access to worker threads),
 * the test will pass with zero violations.
 *
 * This gives an LLM (or human developer) a deterministic signal:
 * "fix the code until this test reports zero violations."
 */
static void
test_event_item_lazy_load_violations(ViolationFixture *f,
                                      gconstpointer data G_GNUC_UNUSED)
{
    gn_test_reset_ndb_violations();

    /* Create event items from note keys and access their properties.
     * In the real app, this happens during GtkListView factory_bind. */
    for (int i = 1; i <= 10; i++) {
        GnNostrEventItem *item = gn_nostr_event_item_new_from_key(
            (uint64_t)i, 1700000000 - i);

        if (item) {
            /* These calls trigger lazy NDB loading on the main thread */
            const char *content = gn_nostr_event_item_get_content(item);
            const char *pubkey = gn_nostr_event_item_get_pubkey(item);
            const char *event_id = gn_nostr_event_item_get_event_id(item);
            (void)content; (void)pubkey; (void)event_id;

            g_object_unref(item);
        }
    }

    unsigned violations = gn_test_get_ndb_violation_count();
    g_test_message("Event item lazy load: %u main-thread NDB violations", violations);

    /* Report violations with full diagnostic output.
     * This will FAIL in the current codebase (which is the point —
     * it tells the developer exactly what to fix). */
    gn_test_assert_no_ndb_violations("during event item lazy load");
}

/* ── Test: Model refresh triggers NDB txn on main thread ──────────── */

static void
test_model_refresh_violations(ViolationFixture *f,
                               gconstpointer data G_GNUC_UNUSED)
{
    gn_test_reset_ndb_violations();

    /* Create a model and refresh it — this queries NDB for matching events */
    GnNostrEventModel *model = gn_nostr_event_model_new();
    g_assert_nonnull(model);

    GnNostrQueryParams params = {
        .kinds = (gint[]){1},
        .n_kinds = 1,
        .limit = 50,
    };
    gn_nostr_event_model_set_query(model, &params);
    gn_nostr_event_model_refresh(model);

    /* Drain main loop to process any async callbacks */
    gn_test_drain_main_loop();

    unsigned violations = gn_test_get_ndb_violation_count();
    g_test_message("Model refresh: %u main-thread NDB violations", violations);

    g_object_unref(model);

    gn_test_assert_no_ndb_violations("during model refresh");
}

/* ── Test: Model iteration (getitem) triggers NDB txn on main thread ─ */

static void
test_model_iteration_violations(ViolationFixture *f,
                                 gconstpointer data G_GNUC_UNUSED)
{
    /* First, create and populate model WITHOUT violation tracking
     * (model creation itself may need NDB access) */
    gn_test_clear_main_thread();

    GnNostrEventModel *model = gn_nostr_event_model_new();
    GnNostrQueryParams params = {
        .kinds = (gint[]){1},
        .n_kinds = 1,
        .limit = 50,
    };
    gn_nostr_event_model_set_query(model, &params);
    gn_nostr_event_model_refresh(model);
    gn_test_drain_main_loop();

    guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
    g_test_message("Model has %u items", n);

    /* NOW enable violation tracking and iterate */
    gn_test_mark_main_thread();
    gn_test_reset_ndb_violations();

    for (guint i = 0; i < n && i < 20; i++) {
        g_autoptr(GnNostrEventItem) item = g_list_model_get_item(
            G_LIST_MODEL(model), i);
        if (!item) continue;

        /* Access properties that trigger lazy NDB loads */
        const char *content = gn_nostr_event_item_get_content(item);
        const char *pubkey = gn_nostr_event_item_get_pubkey(item);
        gint kind = gn_nostr_event_item_get_kind(item);
        const char *tags = gn_nostr_event_item_get_tags_json(item);
        const char * const *hashtags = gn_nostr_event_item_get_hashtags(item);
        (void)content; (void)pubkey; (void)kind; (void)tags; (void)hashtags;
    }

    unsigned violations = gn_test_get_ndb_violation_count();
    g_test_message("Model iteration: %u main-thread NDB violations "
                   "(from %u items)", violations, MIN(n, 20));

    g_object_unref(model);

    gn_test_assert_no_ndb_violations("during model item property access");
}

/* ── Test: storage_ndb convenience functions on main thread ────────── */

static void
test_convenience_api_violations(ViolationFixture *f,
                                 gconstpointer data G_GNUC_UNUSED)
{
    gn_test_reset_ndb_violations();

    /* These convenience functions manage their own transactions internally.
     * They should NOT be called from the main thread. */

    /* storage_ndb_count_reactions — called during metadata batch */
    const char *fake_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    storage_ndb_count_reactions(fake_id);

    /* storage_ndb_is_profile_stale — called during profile service checks */
    const char *fake_pk = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    storage_ndb_is_profile_stale(fake_pk, 0);

    /* storage_ndb_is_event_expired — called during NIP-40 checks */
    storage_ndb_is_event_expired(1);

    unsigned violations = gn_test_get_ndb_violation_count();
    g_test_message("Convenience API: %u main-thread NDB violations", violations);

    gn_test_assert_no_ndb_violations("during convenience API calls on main thread");
}

/* ── Test: Batch metadata queries on main thread ──────────────────── */

static void
test_batch_metadata_violations(ViolationFixture *f,
                                gconstpointer data G_GNUC_UNUSED)
{
    gn_test_reset_ndb_violations();

    /* These batch APIs are called from on_metadata_batch_done callbacks.
     * Even though the batch GTask runs off-thread, the result callback
     * runs on the main thread and may trigger follow-up NDB queries. */
    const char *ids[] = {
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        NULL
    };

    GHashTable *reactions = storage_ndb_count_reactions_batch(ids, 3);
    if (reactions) g_hash_table_unref(reactions);

    GHashTable *reposts = storage_ndb_count_reposts_batch(ids, 3);
    if (reposts) g_hash_table_unref(reposts);

    GHashTable *zaps = storage_ndb_get_zap_stats_batch(ids, 3);
    if (zaps) g_hash_table_unref(zaps);

    GHashTable *replies = storage_ndb_count_replies_batch(ids, 3);
    if (replies) g_hash_table_unref(replies);

    unsigned violations = gn_test_get_ndb_violation_count();
    g_test_message("Batch metadata: %u main-thread NDB violations", violations);

    gn_test_assert_no_ndb_violations("during batch metadata queries on main thread");
}

/* ── Main ─────────────────────────────────────────────────────────── */
int
main(int argc, char *argv[])
{
    /* Use g_test_init, not gtk_test_init — these tests don't need GTK
     * display, just GLib + NDB */
    g_test_init(&argc, &argv, NULL);

    g_test_add("/gnostr/ndb-violations/event-item-lazy-load",
               ViolationFixture, NULL,
               fixture_setup, test_event_item_lazy_load_violations,
               fixture_teardown);

    g_test_add("/gnostr/ndb-violations/model-refresh",
               ViolationFixture, NULL,
               fixture_setup, test_model_refresh_violations,
               fixture_teardown);

    g_test_add("/gnostr/ndb-violations/model-iteration",
               ViolationFixture, NULL,
               fixture_setup, test_model_iteration_violations,
               fixture_teardown);

    g_test_add("/gnostr/ndb-violations/convenience-api",
               ViolationFixture, NULL,
               fixture_setup, test_convenience_api_violations,
               fixture_teardown);

    g_test_add("/gnostr/ndb-violations/batch-metadata",
               ViolationFixture, NULL,
               fixture_setup, test_batch_metadata_violations,
               fixture_teardown);

    return g_test_run();
}
