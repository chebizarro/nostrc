/**
 * test_bind_latency_budget.c — Main-thread latency budget enforcement
 *
 * Verifies that GtkListView bind/unbind operations complete within
 * acceptable time budgets, ensuring smooth scrolling UX.
 *
 * Checks scroll bind work against an independent in-process GTK reference;
 * model-swap churn also uses a main-loop heartbeat.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <glib.h>
#include <time.h>
#include <stdlib.h>
#include "nostrc-test-gdk-frame.h"

/* ASan/UBSan relaxation: sanitizer builds are ~5-10x slower.
 * Scale timing budgets accordingly to avoid CI flakes.
 *
 * __has_feature is a Clang builtin; GCC macro-expands both operands of `&&`
 * before short-circuiting, so a bare `__has_feature(...)` token errors under
 * GCC even when `defined(__has_feature)` is false. Shim it to 0 so the
 * sanitizer detection compiles on GCC (which uses __SANITIZE_ADDRESS__ /
 * __SANITIZE_THREAD__ instead). Matches the fix in
 * nostr-gobject/src/nostr_simple_pool.c. */
#ifndef __has_feature
#  define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) \
    || __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#  define SANITIZER_SLOWDOWN 10
#else
#  define SANITIZER_SLOWDOWN 1
#endif

/* The native macOS GTK backend schedules AppKit layout/rendering on the same
 * main context as the heartbeat. Its normal frame-processing gaps are roughly
 * 2x the X11/Xvfb baseline; keep the total-work budget and missed-gap allowance
 * unchanged so sustained regressions still fail. */
#ifdef __APPLE__
#  define BACKEND_SLOWDOWN 2
#else
#  define BACKEND_SLOWDOWN 1
#endif

/* The short scroll loop finishes before a 5 ms heartbeat can fire. Measure
 * its synchronous work directly; the longer model-swap test uses a heartbeat. */
#define N_ITEMS         300
#define HEARTBEAT_MS    5
#define MAX_STALL_MS    (100 * SANITIZER_SLOWDOWN * BACKEND_SLOWDOWN)
#define MAX_BIND_REFERENCE_RATIO 3.0
/* Minimum heartbeat iterations we expect in any test — ensures heartbeat actually fired */
#define MIN_HEARTBEATS  3

/* ── Heartbeat tracking ───────────────────────────────────────────── */

typedef struct {
    guint count;
    guint missed;
    gint64 last_us;
    gint64 max_gap_us;
    gint64 last_cpu_us;
    gint64 max_cpu_gap_us;
} Heartbeat;

static gint64
thread_cpu_us(void)
{
    struct timespec ts;
    g_assert_cmpint(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts), ==, 0);
    return (gint64)ts.tv_sec * G_USEC_PER_SEC + ts.tv_nsec / 1000;
}

static gboolean
heartbeat_tick(gpointer data)
{
    Heartbeat *hb = data;
    gint64 now = g_get_monotonic_time();
    gint64 cpu_now = thread_cpu_us();
    if (hb->last_us > 0) {
        gint64 gap = now - hb->last_us;
        if (gap > hb->max_gap_us) hb->max_gap_us = gap;
        gint64 cpu_gap = cpu_now - hb->last_cpu_us;
        if (cpu_gap > hb->max_cpu_gap_us) hb->max_cpu_gap_us = cpu_gap;
        if (cpu_gap > MAX_STALL_MS * 1000) hb->missed++;
    }
    hb->last_us = now;
    hb->last_cpu_us = cpu_now;
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
static gint64 bind_cpu_total_ns = 0;
static gint64 bind_cpu_max_ns = 0;
static guint reference_bind_count = 0;
static gint64 reference_bind_cpu_ns = 0;

static gint64
thread_cpu_ns(void)
{
    struct timespec ts;
    g_assert_cmpint(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts), ==, 0);
    return (gint64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Independent reference, interleaved with candidate bindings on the same list.
 * Unlike setting labels on an idle item, scrolling really dirties GTK layout.
 * Do not share this implementation with on_bind: added candidate work must
 * not increase its own budget. */
static void
on_reference_bind(GtkListItemFactory *f G_GNUC_UNUSED, GtkListItem *li,
                  gpointer ud G_GNUC_UNUSED)
{
    gint64 start = thread_cpu_ns();
    GtkBox *box = GTK_BOX(gtk_list_item_get_child(li));
    GtkLabel *header = GTK_LABEL(gtk_widget_get_first_child(GTK_WIDGET(box)));
    GtkLabel *body = GTK_LABEL(gtk_widget_get_next_sibling(GTK_WIDGET(header)));
    GtkStringObject *so = GTK_STRING_OBJECT(gtk_list_item_get_item(li));
    gtk_label_set_text(header, "Author Name · 3m");
    gtk_label_set_text(body, gtk_string_object_get_string(so));
    reference_bind_count++;
    reference_bind_cpu_ns += thread_cpu_ns() - start;
}

static void
on_bind(GtkListItemFactory *f G_GNUC_UNUSED, GtkListItem *li, gpointer ud G_GNUC_UNUSED)
{
    gint64 start = thread_cpu_ns();
    GtkBox *box = GTK_BOX(gtk_list_item_get_child(li));
    GtkLabel *header = GTK_LABEL(gtk_widget_get_first_child(GTK_WIDGET(box)));
    GtkLabel *body = GTK_LABEL(gtk_widget_get_next_sibling(GTK_WIDGET(header)));
    GtkStringObject *so = GTK_STRING_OBJECT(gtk_list_item_get_item(li));

    gtk_label_set_text(header, "Author Name · 3m");
    gtk_label_set_text(body, gtk_string_object_get_string(so));
    bind_count++;
    gint64 elapsed = thread_cpu_ns() - start;
    bind_cpu_total_ns += elapsed;
    if (elapsed > bind_cpu_max_ns) bind_cpu_max_ns = elapsed;
}

static gboolean compare_bindings = FALSE;
static guint binding_sequence = 0;

static void
on_benchmark_bind(GtkListItemFactory *factory, GtkListItem *item, gpointer data)
{
    /* Neighboring rows see the same layout/cache state and CPU frequency.
     * Timing separate windows or whole sweeps gives them different workloads. */
    if (compare_bindings && binding_sequence++ % 2 == 0)
        on_reference_bind(factory, item, data);
    else
        on_bind(factory, item, data);
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

static void
scroll_through(GtkScrolledWindow *sw)
{
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(sw);
    double upper = gtk_adjustment_get_upper(vadj);
    double page = gtk_adjustment_get_page_size(vadj);
    for (int direction = 0; direction < 2; direction++) {
        for (int step = 0; step <= 50 && upper > page; step++) {
            double fraction = step / 50.0;
            double pos = (upper - page) * (direction ? 1.0 - fraction : fraction);
            gtk_adjustment_set_value(vadj, pos);
            for (int i = 0; i < 5; i++)
                g_main_context_iteration(g_main_context_default(), FALSE);
        }
    }
}

static gint
compare_ratio(gconstpointer a, gconstpointer b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* ── Test: Bind loop stays within latency budget ──────────────────── */
static void
test_bind_latency_within_budget(void)
{
    bind_count = 0;
    bind_cpu_total_ns = 0;
    bind_cpu_max_ns = 0;

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
    g_signal_connect(factory, "bind", G_CALLBACK(on_benchmark_bind), NULL);

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

    /* Initial GTK/AppKit window presentation is not bind churn. In
     * particular, its first frame can consume hundreds of milliseconds
     * before any scrolling starts. Settle it outside the measured phase. */
    gtk_window_present(win);
    for (int i = 0; i < 200; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }

    guint binds_before_scroll = bind_count;
    scroll_through(sw);
    g_assert_cmpuint(bind_count, >, binds_before_scroll);
    g_test_message("Scroll binds: %u", bind_count - binds_before_scroll);

    if (g_getenv("NOSTRC_TEST_PERF")) {
        /* A median of nine interleaved samples rejects sustained work growth
         * without letting a single allocator/cache outlier decide the result. */
        compare_bindings = TRUE;
        double ratios[9];
        for (guint sample = 0; sample < G_N_ELEMENTS(ratios); sample++) {
            bind_count = reference_bind_count = 0;
            bind_cpu_total_ns = reference_bind_cpu_ns = 0;
            bind_cpu_max_ns = 0;
            binding_sequence = sample % 2;
            scroll_through(sw);
            g_assert_cmpuint(bind_count, >, 0);
            g_assert_cmpuint(reference_bind_count, >, 0);
            double baseline = (double)reference_bind_cpu_ns / reference_bind_count;
            double candidate = (double)bind_cpu_total_ns / bind_count;
            g_assert_cmpfloat(baseline, >, 0);
            ratios[sample] = candidate / baseline;
            g_test_message("Pair %u: candidate %.3f us/bind (%u binds), "
                           "reference %.3f us/bind (%u binds), ratio %.2f, max %.3f us",
                           sample, candidate / 1000, bind_count,
                           baseline / 1000, reference_bind_count,
                           ratios[sample], bind_cpu_max_ns / 1000.0);
        }
        compare_bindings = FALSE;
        qsort(ratios, G_N_ELEMENTS(ratios), sizeof ratios[0], compare_ratio);
        double median = ratios[G_N_ELEMENTS(ratios) / 2];
        g_test_message("Median bind CPU ratio: %.2f (limit %.1f)",
                       median, MAX_BIND_REFERENCE_RATIO);
        g_assert_cmpfloat(median, <, MAX_BIND_REFERENCE_RATIO);
    }

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

    g_test_message("After 10 swaps: heartbeat_count=%u, cpu_missed=%u, "
                   "max_wall_gap=%.1fms, max_cpu_gap=%.1fms",
                   hb.count, hb.missed, hb.max_gap_us / 1000.0,
                   hb.max_cpu_gap_us / 1000.0);

    /* Heartbeat must have fired */
    g_assert_cmpuint(hb.count, >=, MIN_HEARTBEATS);
    if (g_getenv("NOSTRC_TEST_PERF"))
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
    nostrc_test_tolerate_gdk_frame_warning();

    g_test_add_func("/nostr-gtk/latency/bind-within-budget",
                    test_bind_latency_within_budget);
    g_test_add_func("/nostr-gtk/latency/model-swap-no-stall",
                    test_model_swap_no_stall);

    return g_test_run();
}
