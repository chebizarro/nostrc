/**
 * test_bind_latency_budget.c — Main-thread latency budget enforcement
 *
 * Verifies that GtkListView bind/unbind operations complete within
 * acceptable time budgets, ensuring smooth scrolling UX.
 *
 * Uses a heartbeat idle to detect main-thread stalls during bind churn.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <glib.h>

/* ASan/UBSan relaxation: sanitizer builds are ~5-10x slower.
 * Scale timing budgets accordingly to avoid CI flakes. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) \
    || (defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
#  define SANITIZER_SLOWDOWN 10
#else
#  define SANITIZER_SLOWDOWN 1
#endif

/* Budget: bind loop for N items should not cause any stall > MAX_STALL_MS */
#define N_ITEMS         300
#define HEARTBEAT_MS    5
#define MAX_STALL_MS    (100 * SANITIZER_SLOWDOWN)
#define MAX_TOTAL_MS    (5000 * SANITIZER_SLOWDOWN)
/* Minimum heartbeat iterations we expect in any test — ensures heartbeat actually fired */
#define MIN_HEARTBEATS  3

/* ── Heartbeat tracking ───────────────────────────────────────────── */

typedef struct {
    guint count;
    guint missed;
    gint64 last_us;
    gint64 max_gap_us;
} Heartbeat;

static gboolean
heartbeat_tick(gpointer data)
{
    Heartbeat *hb = data;
    gint64 now = g_get_monotonic_time();
    if (hb->last_us > 0) {
        gint64 gap = now - hb->last_us;
        if (gap > hb->max_gap_us) hb->max_gap_us = gap;
        if (gap > MAX_STALL_MS * 1000) hb->missed++;
    }
    hb->last_us = now;
    hb->count++;
    return G_SOURCE_CONTINUE;
}

/* ── Factory ──────────────────────────────────────────────────────── */

static void
on_setup(GtkListItemFactory *f G_GNUC_UNUSED, GtkListItem *li, gpointer ud G_GNUC_UNUSED)
{
    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2));
    GtkLabel *header = GTK_LABEL(gtk_label_new(""));
    GtkLabel *body = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(body, TRUE);
    gtk_label_set_lines(body, 3);
    gtk_label_set_ellipsize(body, PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(GTK_WIDGET(box), -1, 60);
    gtk_box_append(box, GTK_WIDGET(header));
    gtk_box_append(box, GTK_WIDGET(body));
    gtk_list_item_set_child(li, GTK_WIDGET(box));
}

static guint bind_count = 0;

static void
on_bind(GtkListItemFactory *f G_GNUC_UNUSED, GtkListItem *li, gpointer ud G_GNUC_UNUSED)
{
    GtkBox *box = GTK_BOX(gtk_list_item_get_child(li));
    GtkLabel *header = GTK_LABEL(gtk_widget_get_first_child(GTK_WIDGET(box)));
    GtkLabel *body = GTK_LABEL(gtk_widget_get_next_sibling(GTK_WIDGET(header)));
    GtkStringObject *so = GTK_STRING_OBJECT(gtk_list_item_get_item(li));

    gtk_label_set_text(header, "Author Name · 3m");
    gtk_label_set_text(body, gtk_string_object_get_string(so));
    bind_count++;
}

/* ── Helper: ensure heartbeat fires enough times ─────────────────── */

static void
ensure_heartbeat_warmup(Heartbeat *hb, guint min_count)
{
    guint iters = 0;
    while (hb->count < min_count && iters < 2000) {
        g_main_context_iteration(g_main_context_default(), FALSE);
        g_usleep(1000); /* 1ms */
        iters++;
    }
}

/* ── Test: Bind loop stays within latency budget ──────────────────── */
static void
test_bind_latency_within_budget(void)
{
    bind_count = 0;

    /* Create model */
    GListStore *store = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    for (int i = 0; i < N_ITEMS; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Note %d: Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
            "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.", i);
        GtkStringObject *so = gtk_string_object_new(buf);
        g_list_store_append(store, so);
        g_object_unref(so);
    }

    /* Create factory + list view.
     * GtkNoSelection takes ownership of the model, so we need a ref for ourselves. */
    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
        gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(on_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_bind), NULL);

    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(store));
    /* store ownership transferred to sel — don't unref store separately */
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(
        GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory)));
    /* sel and factory ownership transferred to lv */

    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(sw, GTK_WIDGET(lv));
    gtk_widget_set_size_request(GTK_WIDGET(sw), 400, 600);

    GtkWindow *win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(win, 400, 600);
    gtk_window_set_child(win, GTK_WIDGET(sw));

    /* Start heartbeat BEFORE showing the window */
    Heartbeat hb = {0};
    guint hb_id = g_timeout_add(HEARTBEAT_MS, heartbeat_tick, &hb);

    gint64 total_start = g_get_monotonic_time();

    /* Show window — triggers initial binds */
    gtk_window_present(win);

    /* Iterate until initial binds complete, also warming up the heartbeat */
    for (int i = 0; i < 200; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }

    /* Ensure heartbeat has had a chance to fire at least a few times */
    ensure_heartbeat_warmup(&hb, MIN_HEARTBEATS);

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

    gint64 total_elapsed_us = g_get_monotonic_time() - total_start;
    double total_ms = total_elapsed_us / 1000.0;

    g_source_remove(hb_id);

    g_test_message("Bind latency test results:");
    g_test_message("  Total binds: %u", bind_count);
    g_test_message("  Total time: %.1f ms (budget: %d ms)", total_ms, MAX_TOTAL_MS);
    g_test_message("  Heartbeat count: %u (minimum: %d)", hb.count, MIN_HEARTBEATS);
    g_test_message("  Missed heartbeats (>%dms): %u", MAX_STALL_MS, hb.missed);
    g_test_message("  Max gap: %.1f ms", hb.max_gap_us / 1000.0);

    /* Assertions */
    g_assert_cmpuint(bind_count, >, 0);
    g_assert_cmpfloat(total_ms, <, MAX_TOTAL_MS);
    /* Heartbeat must have actually fired — otherwise all stall assertions are vacuous */
    g_assert_cmpuint(hb.count, >=, MIN_HEARTBEATS);
    g_assert_cmpuint(hb.missed, <=, 2 * SANITIZER_SLOWDOWN);
    g_assert_cmpfloat(hb.max_gap_us / 1000.0, <, MAX_STALL_MS * 2);

    /* Cleanup — window owns sw, lv, sel, factory; destroy cascades */
    gtk_window_destroy(win);
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);
}

/* ── Test: Model replacement doesn't cause long stall ─────────────── */
static void
test_model_swap_no_stall(void)
{
    bind_count = 0;

    GListStore *store = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    for (int i = 0; i < 100; i++) {
        g_autofree char *s = g_strdup_printf("Initial item %d", i);
        GtkStringObject *so = gtk_string_object_new(s);
        g_list_store_append(store, so);
        g_object_unref(so);
    }

    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
        gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(on_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_bind), NULL);

    /* Keep a ref on store since we need to clear/repopulate it later */
    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(store)));
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(
        GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory)));

    GtkWindow *win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(win, 400, 600);
    gtk_window_set_child(win, GTK_WIDGET(lv));
    gtk_window_present(win);

    for (int i = 0; i < 50; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    Heartbeat hb = {0};
    guint hb_id = g_timeout_add(HEARTBEAT_MS, heartbeat_tick, &hb);

    /* Perform 10 model swaps, timing each one */
    for (int swap = 0; swap < 10; swap++) {
        gint64 swap_start = g_get_monotonic_time();

        /* Clear and repopulate */
        g_list_store_remove_all(store);
        for (int i = 0; i < 100; i++) {
            g_autofree char *s = g_strdup_printf("Swap %d item %d", swap, i);
            GtkStringObject *so = gtk_string_object_new(s);
            g_list_store_append(store, so);
            g_object_unref(so);
        }

        /* Process events */
        for (int i = 0; i < 30; i++)
            g_main_context_iteration(g_main_context_default(), FALSE);

        gint64 swap_ms = (g_get_monotonic_time() - swap_start) / 1000;
        g_test_message("Swap %d took %ld ms", swap, (long)swap_ms);
    }

    /* Ensure heartbeat had time to fire */
    ensure_heartbeat_warmup(&hb, MIN_HEARTBEATS);
    g_source_remove(hb_id);

    g_test_message("After 10 swaps: heartbeat_count=%u, missed=%u, max_gap=%.1fms",
                   hb.count, hb.missed, hb.max_gap_us / 1000.0);

    /* Heartbeat must have fired */
    g_assert_cmpuint(hb.count, >=, MIN_HEARTBEATS);
    g_assert_cmpuint(hb.missed, <=, 3 * SANITIZER_SLOWDOWN);

    /* Cleanup — destroy window (cascades to lv, which owns sel and factory) */
    gtk_window_destroy(win);
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);
    /* Release our extra ref on store */
    g_object_unref(store);
}

int
main(int argc, char *argv[])
{
    gtk_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gtk/latency/bind-within-budget",
                    test_bind_latency_within_budget);
    g_test_add_func("/nostr-gtk/latency/model-swap-no-stall",
                    test_model_swap_no_stall);

    return g_test_run();
}
