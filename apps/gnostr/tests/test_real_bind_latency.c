/**
 * test_real_bind_latency.c — Real-component bind latency measurement
 *
 * Unlike test_bind_latency_budget.c which uses GtkStringObject mocks,
 * this test uses REAL GnNostrEventItem objects backed by a REAL NDB.
 *
 * It exercises the actual code paths that run during GtkListView bind:
 *   1. GnNostrEventItem::get_content() → ensure_note_loaded() → NDB txn
 *   2. GnNostrEventItem::get_pubkey() → lazy NDB lookup
 *   3. GnNostrEventItem::get_hashtags() → tag extraction from NDB note
 *
 * The test combines two detection mechanisms:
 *   a) Main-thread NDB violation counter (deterministic: pass/fail)
 *   b) Heartbeat stall detection (timing: informational)
 *
 * The NDB violation counter is the PRIMARY signal — it catches the
 * architectural issue regardless of system speed. The heartbeat is
 * SECONDARY — it measures the user-facing impact.
 *
 * MUST be compiled with -DGNOSTR_TESTING for full instrumentation.
 * Requires Xvfb (or macOS native display) for GTK widget testing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <glib.h>
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include "model/gn-nostr-event-model.h"
#include "model/gn-nostr-event-item.h"

/* ASan relaxation */
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
#  define SANITIZER_SLOWDOWN 10
#else
#  define SANITIZER_SLOWDOWN 1
#endif

#define N_EVENTS       500
#define N_PROFILES     50
#define HEARTBEAT_MS   5
#define MAX_STALL_MS   (100 * SANITIZER_SLOWDOWN)

/* ── Test fixture ─────────────────────────────────────────────────── */

typedef struct {
    GnTestNdb *ndb;
    GPtrArray *pubkeys;
    GnNostrEventModel *model;
} RealBindFixture;

static void
fixture_setup(RealBindFixture *f, gconstpointer data G_GNUC_UNUSED)
{
    f->ndb = gn_test_ndb_new(NULL);
    g_assert_nonnull(f->ndb);

    /* Ingest realistic corpus (varied content, profiles for readiness) */
    f->pubkeys = gn_test_ingest_realistic_corpus(f->ndb, N_EVENTS, N_PROFILES);
    g_assert_nonnull(f->pubkeys);

    /* Create and populate model — do this BEFORE marking main thread
     * since model creation itself uses NDB */
    f->model = gn_nostr_event_model_new();
    g_assert_nonnull(f->model);

    GnNostrQueryParams params = {
        .kinds = (gint[]){1},
        .n_kinds = 1,
        .limit = 100,
    };
    gn_nostr_event_model_set_query(f->model, &params);
    gn_nostr_event_model_refresh(f->model);
    gn_test_drain_main_loop();

    guint n = g_list_model_get_n_items(G_LIST_MODEL(f->model));
    g_test_message("Fixture: model has %u items from %u events, %u profiles",
                   n, N_EVENTS, N_PROFILES);
}

static void
fixture_teardown(RealBindFixture *f, gconstpointer data G_GNUC_UNUSED)
{
    gn_test_clear_main_thread();
    g_clear_object(&f->model);
    g_ptr_array_unref(f->pubkeys);
    gn_test_ndb_free(f->ndb);
}

/* ── Minimal factory that calls real item accessors ───────────────── */

static guint g_bind_count = 0;

static void
real_factory_setup(GtkListItemFactory *f G_GNUC_UNUSED,
                   GtkListItem *li, gpointer ud G_GNUC_UNUSED)
{
    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2));
    GtkLabel *author = GTK_LABEL(gtk_label_new(""));
    GtkLabel *content = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(content, TRUE);
    gtk_label_set_lines(content, 4);
    gtk_label_set_ellipsize(content, PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(GTK_WIDGET(box), -1, 80);
    gtk_box_append(box, GTK_WIDGET(author));
    gtk_box_append(box, GTK_WIDGET(content));
    gtk_list_item_set_child(li, GTK_WIDGET(box));
}

static void
real_factory_bind(GtkListItemFactory *f G_GNUC_UNUSED,
                  GtkListItem *li, gpointer ud G_GNUC_UNUSED)
{
    GtkBox *box = GTK_BOX(gtk_list_item_get_child(li));
    GtkLabel *author_label = GTK_LABEL(gtk_widget_get_first_child(GTK_WIDGET(box)));
    GtkLabel *content_label = GTK_LABEL(
        gtk_widget_get_next_sibling(GTK_WIDGET(author_label)));

    GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(gtk_list_item_get_item(li));
    if (!item) return;

    /* These are the REAL accessors that trigger NDB lazy loading.
     * In the current code, each of these opens an NDB read transaction
     * on the main thread. */
    const char *pubkey = gn_nostr_event_item_get_pubkey(item);
    const char *content = gn_nostr_event_item_get_content(item);
    gint64 created_at = gn_nostr_event_item_get_created_at(item);

    /* Set widget text from real data */
    if (pubkey) {
        g_autofree char *author_text = g_strdup_printf("%.16s... · %"
            G_GINT64_FORMAT, pubkey, created_at);
        gtk_label_set_text(author_label, author_text);
    }
    if (content) {
        gtk_label_set_text(content_label, content);
    }

    g_bind_count++;
}

/* ── Test: Real bind with NDB violation detection ─────────────────── */

static void
test_real_bind_ndb_violations(RealBindFixture *f,
                               gconstpointer data G_GNUC_UNUSED)
{
    g_bind_count = 0;

    /* Enable violation tracking NOW (after model is populated) */
    gn_test_mark_main_thread();
    gn_test_reset_ndb_violations();

    /* Create GtkListView with our real model */
    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
        gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(real_factory_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(real_factory_bind), NULL);

    GtkNoSelection *sel = gtk_no_selection_new(
        G_LIST_MODEL(g_object_ref(f->model)));
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(
        GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory)));

    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(sw, GTK_WIDGET(lv));
    gtk_widget_set_size_request(GTK_WIDGET(sw), 400, 600);

    GtkWindow *win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(win, 400, 600);
    gtk_window_set_child(win, GTK_WIDGET(sw));

    /* Show window — triggers bind callbacks */
    gtk_window_present(win);
    for (int i = 0; i < 200; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    /* Scroll through the list */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(sw);
    double upper = gtk_adjustment_get_upper(vadj);
    double page = gtk_adjustment_get_page_size(vadj);

    for (int step = 0; step < 30 && upper > page; step++) {
        double pos = (upper - page) * step / 30.0;
        gtk_adjustment_set_value(vadj, pos);
        for (int i = 0; i < 5; i++)
            g_main_context_iteration(g_main_context_default(), FALSE);
    }

    unsigned violations = gn_test_get_ndb_violation_count();
    g_test_message("Real bind test: %u binds, %u NDB violations",
                   g_bind_count, violations);

    /* Cleanup */
    gtk_window_destroy(win);
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    /* THIS IS THE KEY ASSERTION:
     * If any NDB transaction was opened on the main thread during bind,
     * it means the UI was blocked by a database operation. */
    gn_test_assert_no_ndb_violations("during real GtkListView bind+scroll");
}

/* ── Test: Real bind with heartbeat stall detection ───────────────── */

static void
test_real_bind_stall_detection(RealBindFixture *f,
                                gconstpointer data G_GNUC_UNUSED)
{
    g_bind_count = 0;

    /* Start heartbeat BEFORE showing window */
    GnTestHeartbeat hb;
    gn_test_heartbeat_start(&hb, HEARTBEAT_MS, MAX_STALL_MS);

    /* Create and show GtkListView */
    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
        gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(real_factory_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(real_factory_bind), NULL);

    GtkNoSelection *sel = gtk_no_selection_new(
        G_LIST_MODEL(g_object_ref(f->model)));
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(
        GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory)));

    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(sw, GTK_WIDGET(lv));
    gtk_widget_set_size_request(GTK_WIDGET(sw), 400, 600);

    GtkWindow *win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(win, 400, 600);
    gtk_window_set_child(win, GTK_WIDGET(sw));

    gtk_window_present(win);
    for (int i = 0; i < 200; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    /* Scroll through the entire list */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(sw);
    double upper = gtk_adjustment_get_upper(vadj);
    double page = gtk_adjustment_get_page_size(vadj);

    for (int step = 0; step < 50 && upper > page; step++) {
        double pos = (upper - page) * step / 50.0;
        gtk_adjustment_set_value(vadj, pos);
        for (int i = 0; i < 5; i++)
            g_main_context_iteration(g_main_context_default(), FALSE);
    }

    gn_test_heartbeat_stop(&hb);

    g_test_message("Real stall detection: %u binds, max_gap=%.1fms, "
                   "missed=%u (threshold=%dms)",
                   g_bind_count, hb.max_gap_us / 1000.0,
                   hb.missed_count, MAX_STALL_MS);

    /* Cleanup */
    gtk_window_destroy(win);
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    /* Heartbeat assertions — informational, but catches gross stalls */
    g_assert_cmpuint(hb.count, >, 0);
    g_assert_cmpuint(hb.missed_count, <=, 5 * SANITIZER_SLOWDOWN);
}

/* ── Main ─────────────────────────────────────────────────────────── */
int
main(int argc, char *argv[])
{
    gtk_test_init(&argc, &argv, NULL);

    g_test_add("/gnostr/real-bind/ndb-violations",
               RealBindFixture, NULL,
               fixture_setup, test_real_bind_ndb_violations,
               fixture_teardown);

    g_test_add("/gnostr/real-bind/stall-detection",
               RealBindFixture, NULL,
               fixture_setup, test_real_bind_stall_detection,
               fixture_teardown);

    return g_test_run();
}
