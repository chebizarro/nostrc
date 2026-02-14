/**
 * test_lifecycle_leaks.c — Object lifecycle and leak detection tests
 *
 * Verifies that GObject instances from nostr-gobject are properly
 * finalized after use. Uses weak references to detect leaks.
 *
 * Run under ASan+LSAN for comprehensive leak detection:
 *   G_DEBUG=fatal-warnings,gc-friendly G_SLICE=always-malloc ./test_lifecycle_leaks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/nostr_store.h>
#include <nostr-gobject-1.0/nostr_event.h>

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

/* ── Test: GNostrEvent create/destroy cycle doesn't leak ──────────── */
static void
test_event_lifecycle_no_leak(void)
{
    for (int i = 0; i < 100; i++) {
        GNostrEvent *event = gnostr_event_new();
        GnTestPointerWatch *watch = gn_test_watch_object(G_OBJECT(event), "GNostrEvent");

        gnostr_event_set_kind(event, 1);
        gnostr_event_set_content(event, "Test content for lifecycle check");
        gnostr_event_set_created_at(event, 1700000000 + i);

        gn_test_assert_not_finalized(watch);
        g_object_unref(event);
        gn_test_assert_finalized(watch);

        g_free(watch);
    }
}

/* ── Test: GNostrNdbStore create/destroy cycle doesn't leak ───────── */
static void
test_ndb_store_lifecycle_no_leak(void)
{
    setup();

    for (int i = 0; i < 50; i++) {
        GNostrNdbStore *store = gnostr_ndb_store_new();
        GnTestPointerWatch *watch = gn_test_watch_object(G_OBJECT(store), "GNostrNdbStore");

        /* Exercise subscribe/unsubscribe to test internal state cleanup */
        guint64 sub = gnostr_store_subscribe(GNOSTR_STORE(store),
                                              "{\"kinds\":[1],\"limit\":5}");
        if (sub > 0) {
            gnostr_store_unsubscribe(GNOSTR_STORE(store), sub);
        }

        gn_test_assert_not_finalized(watch);
        g_object_unref(store);
        gn_test_assert_finalized(watch);

        g_free(watch);
    }

    teardown();
}

/* ── Test: GNostrEvent with signal connections finalizes cleanly ───── */
static void
test_event_with_signals_no_leak(void)
{
    GNostrEvent *event = gnostr_event_new();
    GnTestPointerWatch *watch = gn_test_watch_object(G_OBJECT(event), "GNostrEvent+signals");

    gint counter = 0;
    gulong handler_id = g_signal_connect_swapped(event, "notify::content",
                                                  G_CALLBACK(g_atomic_int_inc), &counter);
    g_assert_cmpuint(handler_id, >, 0);

    /* Trigger some signals */
    gnostr_event_set_content(event, "first");
    gnostr_event_set_content(event, "second");
    g_assert_cmpint(counter, ==, 2);

    /* Disconnect before unref (good practice) */
    g_signal_handler_disconnect(event, handler_id);

    gn_test_assert_not_finalized(watch);
    g_object_unref(event);
    gn_test_assert_finalized(watch);

    g_free(watch);
}

/* ── Test: Multiple simultaneous objects don't cross-contaminate ───── */
static void
test_multiple_objects_independent_lifecycle(void)
{
    setup();

    GNostrEvent *e1 = gnostr_event_new();
    GNostrEvent *e2 = gnostr_event_new();
    GNostrNdbStore *s1 = gnostr_ndb_store_new();

    GnTestPointerWatch *w1 = gn_test_watch_object(G_OBJECT(e1), "event-1");
    GnTestPointerWatch *w2 = gn_test_watch_object(G_OBJECT(e2), "event-2");
    GnTestPointerWatch *ws = gn_test_watch_object(G_OBJECT(s1), "store-1");

    /* Free in non-creation order */
    g_object_unref(e2);
    gn_test_assert_finalized(w2);
    gn_test_assert_not_finalized(w1);
    gn_test_assert_not_finalized(ws);

    g_object_unref(s1);
    gn_test_assert_finalized(ws);
    gn_test_assert_not_finalized(w1);

    g_object_unref(e1);
    gn_test_assert_finalized(w1);

    g_free(w1);
    g_free(w2);
    g_free(ws);

    teardown();
}

/* ── Test: Rapid create/ref/unref churn ───────────────────────────── */
static void
test_rapid_ref_unref_churn(void)
{
    for (int i = 0; i < 500; i++) {
        GNostrEvent *event = gnostr_event_new();

        /* Add extra refs and unref them */
        g_object_ref(event);
        g_object_ref(event);
        g_object_unref(event);
        g_object_unref(event);

        /* Final unref should finalize */
        GnTestPointerWatch *watch = gn_test_watch_object(G_OBJECT(event), "churn-event");
        g_object_unref(event);
        gn_test_assert_finalized(watch);
        g_free(watch);
    }
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gobject/lifecycle/event-no-leak",
                    test_event_lifecycle_no_leak);
    g_test_add_func("/nostr-gobject/lifecycle/ndb-store-no-leak",
                    test_ndb_store_lifecycle_no_leak);
    g_test_add_func("/nostr-gobject/lifecycle/event-signals-no-leak",
                    test_event_with_signals_no_leak);
    g_test_add_func("/nostr-gobject/lifecycle/multiple-objects-independent",
                    test_multiple_objects_independent_lifecycle);
    g_test_add_func("/nostr-gobject/lifecycle/rapid-ref-unref-churn",
                    test_rapid_ref_unref_churn);

    return g_test_run();
}
