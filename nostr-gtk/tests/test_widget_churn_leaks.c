/**
 * test_widget_churn_leaks.c — Bind/unbind leak detection for GTK widgets
 *
 * Exercises rapid model replacement on a GtkListView to verify that
 * no GObjects leak during the bind→unbind cycle. Designed to be run
 * under ASan+LSAN for comprehensive leak detection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <glib.h>

/* Track how many mock items are currently alive (not finalized) */
static gint g_live_item_count = 0;

/* ── MockItem GObject (minimal, tracks own lifetime) ──────────────── */

#define MOCK_TYPE_ITEM (mock_item_get_type())
G_DECLARE_FINAL_TYPE(MockItem, mock_item, MOCK, ITEM, GObject)

struct _MockItem {
    GObject parent;
    char *text;
};

G_DEFINE_TYPE(MockItem, mock_item, G_TYPE_OBJECT)

static void
mock_item_finalize(GObject *obj)
{
    MockItem *self = MOCK_ITEM(obj);
    g_free(self->text);
    g_atomic_int_dec_and_test(&g_live_item_count);
    G_OBJECT_CLASS(mock_item_parent_class)->finalize(obj);
}

static void
mock_item_class_init(MockItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mock_item_finalize;
}

static void
mock_item_init(MockItem *self G_GNUC_UNUSED)
{
    g_atomic_int_inc(&g_live_item_count);
}

static MockItem *
mock_item_new(const char *text)
{
    MockItem *item = g_object_new(MOCK_TYPE_ITEM, NULL);
    item->text = g_strdup(text);
    return item;
}

/* ── Factory callbacks ────────────────────────────────────────────── */

static void
on_setup(GtkListItemFactory *f G_GNUC_UNUSED, GtkListItem *li, gpointer ud G_GNUC_UNUSED)
{
    GtkLabel *label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_size_request(GTK_WIDGET(label), -1, 40);
    gtk_list_item_set_child(li, GTK_WIDGET(label));
}

static void
on_bind(GtkListItemFactory *f G_GNUC_UNUSED, GtkListItem *li, gpointer ud G_GNUC_UNUSED)
{
    GtkLabel *label = GTK_LABEL(gtk_list_item_get_child(li));
    MockItem *item = MOCK_ITEM(gtk_list_item_get_item(li));
    gtk_label_set_text(label, item->text ? item->text : "");
}

/* ── Test: Model replacement doesn't leak items ───────────────────── */
static void
test_model_churn_no_item_leak(void)
{
    g_atomic_int_set(&g_live_item_count, 0);

    /* Create store with initial items */
    GListStore *store = g_list_store_new(MOCK_TYPE_ITEM);
    for (int i = 0; i < 200; i++) {
        g_autofree char *text = g_strdup_printf("Item %d", i);
        g_autoptr(MockItem) item = mock_item_new(text);
        g_list_store_append(store, item);
    }

    /* Create ListView */
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

    gtk_window_present(win);
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    gint items_before_churn = g_atomic_int_get(&g_live_item_count);
    g_test_message("Items before churn: %d", items_before_churn);

    /* Perform 50 clear/repopulate cycles */
    for (int cycle = 0; cycle < 50; cycle++) {
        g_list_store_remove_all(store);

        /* Drain to let unbinds process */
        for (int i = 0; i < 20; i++)
            g_main_context_iteration(g_main_context_default(), FALSE);

        /* Repopulate */
        for (int i = 0; i < 100; i++) {
            g_autofree char *text = g_strdup_printf("Cycle %d Item %d", cycle, i);
            g_autoptr(MockItem) item = mock_item_new(text);
            g_list_store_append(store, item);
        }

        for (int i = 0; i < 20; i++)
            g_main_context_iteration(g_main_context_default(), FALSE);
    }

    /* Final clear */
    g_list_store_remove_all(store);
    for (int i = 0; i < 200; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    gint items_after_churn = g_atomic_int_get(&g_live_item_count);
    g_test_message("Items after churn: %d (should be ~0)", items_after_churn);

    /* After clearing the model and draining, live items should be very low.
     * The store holds no items, but GtkListView may still hold refs to a
     * few items in its recycle pool. Allow a small margin. */
    g_assert_cmpint(items_after_churn, <=, 20);

    /* Destroy everything */
    gtk_window_destroy(win);
    for (int i = 0; i < 200; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    gint items_final = g_atomic_int_get(&g_live_item_count);
    g_test_message("Items after window destroy: %d (should be 0)", items_final);
    g_assert_cmpint(items_final, ==, 0);
}

/* ── Test: Setting model to NULL and back doesn't leak ────────────── */
static void
test_model_null_swap_no_leak(void)
{
    g_atomic_int_set(&g_live_item_count, 0);

    GListStore *store = g_list_store_new(MOCK_TYPE_ITEM);
    for (int i = 0; i < 100; i++) {
        g_autofree char *text = g_strdup_printf("Item %d", i);
        g_autoptr(MockItem) item = mock_item_new(text);
        g_list_store_append(store, item);
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
    for (int i = 0; i < 100; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    /* Swap model to NULL and back 20 times */
    for (int cycle = 0; cycle < 20; cycle++) {
        /* Set model to NULL — triggers unbind for all visible rows */
        gtk_list_view_set_model(lv, NULL);
        for (int i = 0; i < 20; i++)
            g_main_context_iteration(g_main_context_default(), FALSE);

        /* Set model back */
        GtkNoSelection *new_sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(store)));
        gtk_list_view_set_model(lv, GTK_SELECTION_MODEL(new_sel));
        for (int i = 0; i < 20; i++)
            g_main_context_iteration(g_main_context_default(), FALSE);
    }

    /* Clean up */
    g_list_store_remove_all(store);
    gtk_window_destroy(win);

    for (int i = 0; i < 300; i++)
        g_main_context_iteration(g_main_context_default(), FALSE);

    gint items_final = g_atomic_int_get(&g_live_item_count);
    g_test_message("Items after null-swap churn: %d (should be 0)", items_final);
    g_assert_cmpint(items_final, ==, 0);

    g_object_unref(store);
}

int
main(int argc, char *argv[])
{
    gtk_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gtk/churn/model-churn-no-item-leak",
                    test_model_churn_no_item_leak);
    g_test_add_func("/nostr-gtk/churn/model-null-swap-no-leak",
                    test_model_null_swap_no_leak);

    return g_test_run();
}
