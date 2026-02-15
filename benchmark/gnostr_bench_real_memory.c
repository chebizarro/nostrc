/**
 * gnostr_bench_real_memory.c — Memory ceiling benchmark with real NDB
 *
 * Unlike gnostr_bench_memory_timeline_scroll.c which uses GtkStringObject
 * mocks, this benchmark uses a REAL nostrdb instance with realistic
 * content and REAL GnNostrEventItem objects.
 *
 * This measures actual memory consumption including:
 *   - NDB LMDB mmap overhead
 *   - Per-item cached content, pubkey, event_id, tags, hashtags
 *   - Per-item render result (Pango markup + media URL lists)
 *   - Profile cache entries
 *   - Widget tree allocation (GtkLabel, GtkBox, etc.)
 *
 * Usage:
 *   ./gnostr_bench_real_memory [--n-events N] [--max-rss-mb N]
 *
 * MUST be compiled with -DGNOSTR_TESTING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include "model/gn-nostr-event-model.h"
#include "model/gn-nostr-event-item.h"

/* ── Defaults ─────────────────────────────────────────────────────── */
#define DEFAULT_N_EVENTS      10000
#define DEFAULT_N_PROFILES    500
#define DEFAULT_MAX_RSS_MB    1024   /* 1 GB */
#define SCROLL_STEPS          200

/* ── Factory that exercises real item accessors ───────────────────── */

static guint g_bind_count = 0;

static void
bench_factory_setup(GtkListItemFactory *f G_GNUC_UNUSED,
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
bench_factory_bind(GtkListItemFactory *f G_GNUC_UNUSED,
                   GtkListItem *li, gpointer ud G_GNUC_UNUSED)
{
    GtkBox *box = GTK_BOX(gtk_list_item_get_child(li));
    GtkLabel *author_label = GTK_LABEL(gtk_widget_get_first_child(GTK_WIDGET(box)));
    GtkLabel *content_label = GTK_LABEL(
        gtk_widget_get_next_sibling(GTK_WIDGET(author_label)));

    GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(gtk_list_item_get_item(li));
    if (!item) return;

    const char *pubkey = gn_nostr_event_item_get_pubkey(item);
    const char *content = gn_nostr_event_item_get_content(item);
    gint64 ts = gn_nostr_event_item_get_created_at(item);

    if (pubkey) {
        g_autofree char *text = g_strdup_printf("%.16s... · %" G_GINT64_FORMAT,
                                                 pubkey, ts);
        gtk_label_set_text(author_label, text);
    }
    if (content) {
        gtk_label_set_text(content_label, content);
    }

    g_bind_count++;
}

int
main(int argc, char *argv[])
{
    gtk_init();

    int n_events = DEFAULT_N_EVENTS;
    int max_rss_mb = DEFAULT_MAX_RSS_MB;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--n-events") == 0 && i + 1 < argc)
            n_events = atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-rss-mb") == 0 && i + 1 < argc)
            max_rss_mb = atoi(argv[++i]);
    }

    printf("=== Real Memory Benchmark (NDB-backed) ===\n");
    printf("Events: %d, Profiles: %d, Max RSS: %d MB\n\n",
           n_events, DEFAULT_N_PROFILES, max_rss_mb);

    double rss_start = gn_test_get_rss_mb();
    printf("RSS at start: %.1f MB\n", rss_start);

    /* Create NDB and ingest realistic corpus */
    GnTestNdb *ndb = gn_test_ndb_new(NULL);
    if (!ndb) {
        fprintf(stderr, "Failed to create test NDB\n");
        return 2;
    }

    printf("Ingesting %d events + %d profiles...\n", n_events, DEFAULT_N_PROFILES);
    GPtrArray *pubkeys = gn_test_ingest_realistic_corpus(
        ndb, (guint)n_events, DEFAULT_N_PROFILES);

    double rss_after_ingest = gn_test_get_rss_mb();
    printf("RSS after NDB ingest: %.1f MB (+%.1f)\n",
           rss_after_ingest, rss_after_ingest - rss_start);

    /* Create model and populate from NDB */
    GnNostrEventModel *model = gn_nostr_event_model_new();
    GnNostrQueryParams params = {
        .kinds = (gint[]){1},
        .n_kinds = 1,
        .limit = (guint)MIN(n_events, 500),
    };
    gn_nostr_event_model_set_query(model, &params);
    gn_nostr_event_model_refresh(model);
    gn_test_drain_main_loop();

    guint model_items = g_list_model_get_n_items(G_LIST_MODEL(model));
    double rss_after_model = gn_test_get_rss_mb();
    printf("RSS after model (%u items): %.1f MB (+%.1f)\n",
           model_items, rss_after_model, rss_after_model - rss_start);

    /* Create GtkListView with real factory */
    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
        gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(bench_factory_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(bench_factory_bind), NULL);

    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(model)));
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(
        GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory)));

    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(sw, GTK_WIDGET(lv));
    gtk_widget_set_size_request(GTK_WIDGET(sw), 400, 800);

    GtkWindow *win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(win, 400, 800);
    gtk_window_set_child(win, GTK_WIDGET(sw));

    gtk_window_present(win);
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    double rss_after_render = gn_test_get_rss_mb();
    printf("RSS after initial render: %.1f MB (+%.1f)\n",
           rss_after_render, rss_after_render - rss_start);

    /* Scroll through the entire list */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(sw);
    double upper = gtk_adjustment_get_upper(vadj);
    double page = gtk_adjustment_get_page_size(vadj);
    double peak_rss = rss_after_render;

    printf("\nScrolling %d steps...\n", SCROLL_STEPS);
    for (int i = 0; i < SCROLL_STEPS && upper > page; i++) {
        double pos = (upper - page) * i / (double)SCROLL_STEPS;
        gtk_adjustment_set_value(vadj, pos);

        for (int j = 0; j < 10; j++)
            g_main_context_iteration(g_main_context_default(), FALSE);

        if (i % 20 == 0) {
            double rss = gn_test_get_rss_mb();
            if (rss > peak_rss) peak_rss = rss;

            printf("  Step %3d/%d (%.0f%%): RSS=%.1f MB, binds=%u\n",
                   i, SCROLL_STEPS, pos / (upper - page) * 100, rss, g_bind_count);

            if (rss > max_rss_mb) {
                printf("\n❌ FAIL: RSS %.1f MB exceeds limit %d MB\n",
                       rss, max_rss_mb);
                goto cleanup;
            }
        }
    }

    /* Final measurement after scrolling back to top */
    gtk_adjustment_set_value(vadj, 0);
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    double rss_final = gn_test_get_rss_mb();
    if (rss_final > peak_rss) peak_rss = rss_final;

    printf("\n=== Results ===\n");
    printf("RSS start:        %.1f MB\n", rss_start);
    printf("RSS after ingest: %.1f MB  (NDB + LMDB mmap)\n", rss_after_ingest);
    printf("RSS after model:  %.1f MB  (+ GnNostrEventItem cache)\n", rss_after_model);
    printf("RSS after render: %.1f MB  (+ widget tree)\n", rss_after_render);
    printf("RSS peak scroll:  %.1f MB  (max during scroll)\n", peak_rss);
    printf("RSS final:        %.1f MB  (after scroll-to-top)\n", rss_final);
    printf("Total binds:      %u\n", g_bind_count);
    printf("Max allowed:      %d MB\n\n", max_rss_mb);

cleanup:
    gtk_window_destroy(win);
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);
    g_object_unref(model);
    g_ptr_array_unref(pubkeys);
    gn_test_ndb_free(ndb);

    if (peak_rss > max_rss_mb) {
        printf("❌ FAIL: Peak RSS %.1f MB exceeds limit %d MB\n",
               peak_rss, max_rss_mb);
        return 1;
    }

    printf("✅ PASS: Peak RSS %.1f MB within limit %d MB\n",
           peak_rss, max_rss_mb);
    return 0;
}
