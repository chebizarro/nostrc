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

/* Budget: bind loop for N items should not cause any stall > MAX_STALL_MS */
#define N_ITEMS         300
#define HEARTBEAT_MS    5
#define MAX_STALL_MS    100   /* 100ms max stall — beyond this, UI is noticeably janky */
#define MAX_TOTAL_MS    5000  /* 5s total budget for all bind operations */

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

    /* Create factory + list view */
    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
        gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(on_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_bind), NULL);

    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(store));
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(
        GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory)));

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

    /* Iterate until initial binds complete */
    for (int i = 0; i < 200; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }

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
    g_test_message("  Heartbeat count: %u", hb.count);
    g_test_message("  Missed heartbeats (>%dms): %u", MAX_STALL_MS, hb.missed);
    g_test_message("  Max gap: %.1f ms", hb.max_gap_us / 1000.0);

    /* Assertions */
    g_assert_cmpuint(bind_count, >, 0);
    g_assert_cmpfloat(total_ms, <, MAX_TOTAL_MS);
    g_assert_cmpuint(hb.missed, <=, 2); /* Allow small margin for CI */
    g_assert_cmpfloat(hb.max_gap_us / 1000.0, <, MAX_STALL_MS * 2); /* 2x margin */

    /* Cleanup */
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

    g_source_remove(hb_id);

    g_test_message("After 10 swaps: heartbeat_count=%u, missed=%u, max_gap=%.1fms",
                   hb.count, hb.missed, hb.max_gap_us / 1000.0);

    g_assert_cmpuint(hb.missed, <=, 3);

    gtk_window_destroy(win);
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);
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
