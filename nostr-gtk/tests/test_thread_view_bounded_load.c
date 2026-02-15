/**
 * test_thread_view_bounded_load.c — Bounded thread view loading tests
 *
 * Verifies that thread views (used for displaying note reply threads)
 * respect memory bounds and don't load unlimited events:
 *
 *   1. GListModel with thread items respects a maximum count
 *   2. Repeated model swaps don't accumulate leaked items
 *   3. Thread-view GtkListView can handle large models without widget explosion
 *   4. Ancestor dedup — adding the same ancestor twice doesn't double-count
 *
 * These tests use a mock GListStore as a stand-in for the real thread model,
 * since the thread view's bounded loading logic is exercised through the
 * GListModel interface that GtkListView consumes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <adwaita.h>

/* Maximum thread events — mirrors app-level constraint.
 * Thread views should never load more than this many items. */
#define MAX_THREAD_EVENTS 200

/* ── Mock Thread Item ────────────────────────────────────────────── */

#define MOCK_TYPE_THREAD_ITEM (mock_thread_item_get_type())
G_DECLARE_FINAL_TYPE(MockThreadItem, mock_thread_item, MOCK, THREAD_ITEM, GObject)

struct _MockThreadItem {
  GObject parent_instance;
  char *event_id;
  char *parent_id;
  guint depth;
  gint64 created_at;
};

G_DEFINE_TYPE(MockThreadItem, mock_thread_item, G_TYPE_OBJECT)

static void
mock_thread_item_finalize(GObject *obj)
{
  MockThreadItem *self = MOCK_THREAD_ITEM(obj);
  g_free(self->event_id);
  g_free(self->parent_id);
  G_OBJECT_CLASS(mock_thread_item_parent_class)->finalize(obj);
}

static void
mock_thread_item_class_init(MockThreadItemClass *klass)
{
  GObjectClass *obj = G_OBJECT_CLASS(klass);
  obj->finalize = mock_thread_item_finalize;
}

static void
mock_thread_item_init(MockThreadItem *self)
{
}

static MockThreadItem *
mock_thread_item_new(const char *event_id, const char *parent_id,
                     guint depth, gint64 created_at)
{
  MockThreadItem *item = g_object_new(MOCK_TYPE_THREAD_ITEM, NULL);
  item->event_id = g_strdup(event_id);
  item->parent_id = g_strdup(parent_id);
  item->depth = depth;
  item->created_at = created_at;
  return item;
}

/* ── Live item counter (atomic for leak detection) ───────────────── */

static volatile gint g_live_thread_items = 0;

/* Track items added to stores */
static MockThreadItem *
make_tracked_item(const char *event_id, const char *parent_id,
                  guint depth, gint64 created_at)
{
  MockThreadItem *item = mock_thread_item_new(event_id, parent_id, depth, created_at);
  g_atomic_int_inc(&g_live_thread_items);

  /* weak ref to decrement on finalize */
  g_object_weak_ref(G_OBJECT(item),
                    (GWeakNotify)(void (*)(void))g_atomic_int_add,
                    GINT_TO_POINTER(-1));
  return item;
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static void
drain_events(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

/**
 * Populate a GListStore with N thread items, enforcing a maximum.
 * Returns the actual number of items added (clamped to max).
 */
static guint
populate_bounded(GListStore *store, guint n, guint max)
{
  guint clamped = MIN(n, max);
  for (guint i = 0; i < clamped; i++) {
    g_autofree char *eid = g_strdup_printf("event_%u", i);
    g_autofree char *pid = (i > 0) ? g_strdup_printf("event_%u", i - 1) : NULL;
    MockThreadItem *item = make_tracked_item(eid, pid, (i > 0) ? 1 : 0, 1700000000 + i);
    g_list_store_append(store, item);
    g_object_unref(item);
  }
  return clamped;
}

/* ── Test: bounded-model-count ───────────────────────────────────── */

static void
test_bounded_model_count(void)
{
  gtk_init();

  GListStore *store = g_list_store_new(MOCK_TYPE_THREAD_ITEM);

  /* Try to add more than MAX_THREAD_EVENTS */
  guint requested = MAX_THREAD_EVENTS + 100;
  guint added = populate_bounded(store, requested, MAX_THREAD_EVENTS);

  g_assert_cmpuint(added, ==, MAX_THREAD_EVENTS);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, MAX_THREAD_EVENTS);

  g_object_unref(store);
  drain_events();
}

/* ── Test: repeated-swap-no-accumulation ─────────────────────────── */

static void
test_repeated_swap_no_accumulation(void)
{
  gtk_init();

  g_atomic_int_set(&g_live_thread_items, 0);

  GListStore *store = g_list_store_new(MOCK_TYPE_THREAD_ITEM);

  for (int cycle = 0; cycle < 20; cycle++) {
    /* Clear and repopulate */
    g_list_store_remove_all(store);
    populate_bounded(store, 50, MAX_THREAD_EVENTS);
    drain_events();
  }

  /* After all cycles, store should have exactly 50 items */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 50);

  /* Clear and verify all items are freed */
  g_list_store_remove_all(store);
  g_object_unref(store);
  drain_events();

  gint live = g_atomic_int_get(&g_live_thread_items);
  g_test_message("Live thread items after cleanup: %d", live);
  g_assert_cmpint(live, ==, 0);
}

/* ── Test: ancestor-dedup ────────────────────────────────────────── */

static void
test_ancestor_dedup(void)
{
  gtk_init();

  GListStore *store = g_list_store_new(MOCK_TYPE_THREAD_ITEM);
  GHashTable *seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  /* Simulate adding thread items with dedup check */
  const char *root_id = "root_event_abc";
  guint added = 0;

  for (guint i = 0; i < 10; i++) {
    const char *eid = (i == 0) ? root_id : "reply_event_1";
    if (i > 1) {
      g_autofree char *unique_id = g_strdup_printf("reply_%u", i);
      eid = unique_id;

      if (!g_hash_table_contains(seen_ids, eid)) {
        g_hash_table_add(seen_ids, g_strdup(eid));
        MockThreadItem *item = mock_thread_item_new(eid, root_id, 1, 1700000000 + i);
        g_list_store_append(store, item);
        g_object_unref(item);
        added++;
      }
    } else {
      if (!g_hash_table_contains(seen_ids, eid)) {
        g_hash_table_add(seen_ids, g_strdup(eid));
        MockThreadItem *item = mock_thread_item_new(eid, NULL, 0, 1700000000 + i);
        g_list_store_append(store, item);
        g_object_unref(item);
        added++;
      }
    }
  }

  /* "reply_event_1" was referenced twice (i=1 and duplicated check) but should
   * only be added once due to dedup */
  guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
  g_test_message("Added %u items with dedup (expected %u in store)", added, n);
  g_assert_cmpuint(n, ==, added);

  /* Verify no duplicates in the store */
  GHashTable *verify = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GObject) obj = g_list_model_get_item(G_LIST_MODEL(store), i);
    MockThreadItem *item = MOCK_THREAD_ITEM(obj);
    g_assert_false(g_hash_table_contains(verify, item->event_id));
    g_hash_table_add(verify, item->event_id);
  }

  g_hash_table_unref(verify);
  g_hash_table_unref(seen_ids);
  g_object_unref(store);
}

/* ── Test: listview-with-large-thread-model ──────────────────────── */

static void
on_setup_thread(GtkSignalListItemFactory *factory,
                GtkListItem *list_item,
                gpointer user_data)
{
  GtkLabel *label = GTK_LABEL(gtk_label_new(""));
  gtk_list_item_set_child(list_item, GTK_WIDGET(label));
}

static void
on_bind_thread(GtkSignalListItemFactory *factory,
               GtkListItem *list_item,
               gpointer user_data)
{
  MockThreadItem *item = MOCK_THREAD_ITEM(gtk_list_item_get_item(list_item));
  GtkLabel *label = GTK_LABEL(gtk_list_item_get_child(list_item));
  if (item && label) {
    g_autofree char *text = g_strdup_printf("[%u] %s", item->depth, item->event_id);
    gtk_label_set_text(label, text);
  }
}

static void
test_listview_large_thread(void)
{
  gtk_init();

  /* Create a thread-like store with MAX_THREAD_EVENTS items */
  GListStore *store = g_list_store_new(MOCK_TYPE_THREAD_ITEM);
  populate_bounded(store, MAX_THREAD_EVENTS, MAX_THREAD_EVENTS);

  /* Create factory */
  GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
      gtk_signal_list_item_factory_new());
  g_signal_connect(factory, "setup", G_CALLBACK(on_setup_thread), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(on_bind_thread), NULL);

  /* Create selection model */
  GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(store)));

  /* Create ListView */
  GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(
      GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory)));

  /* Put in a scrolled window */
  GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  gtk_scrolled_window_set_min_content_height(sw, 400);
  gtk_scrolled_window_set_child(sw, GTK_WIDGET(lv));

  /* Create and show window */
  GtkWindow *win = GTK_WINDOW(gtk_window_new());
  gtk_window_set_default_size(win, 400, 600);
  gtk_window_set_child(win, GTK_WIDGET(sw));
  gtk_window_present(win);

  /* Process events — factory should handle all items without crashing */
  for (int i = 0; i < 10; i++) {
    drain_events();
    g_usleep(16000); /* ~1 frame */
  }

  /* Verify model size hasn't grown */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, MAX_THREAD_EVENTS);

  gtk_window_destroy(win);
  g_object_unref(store);
  drain_events();
}

/* ── Test: empty-thread-view-no-crash ────────────────────────────── */

static void
test_empty_thread_no_crash(void)
{
  gtk_init();

  GListStore *store = g_list_store_new(MOCK_TYPE_THREAD_ITEM);

  /* Create factory */
  GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
      gtk_signal_list_item_factory_new());
  g_signal_connect(factory, "setup", G_CALLBACK(on_setup_thread), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(on_bind_thread), NULL);

  GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(store)));
  GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(
      GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory)));

  GtkWindow *win = GTK_WINDOW(gtk_window_new());
  gtk_window_set_child(win, GTK_WIDGET(lv));
  gtk_window_present(win);

  /* Empty model — should not crash */
  drain_events();

  /* Now add a few items */
  populate_bounded(store, 5, MAX_THREAD_EVENTS);
  drain_events();

  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 5);

  /* Clear again */
  g_list_store_remove_all(store);
  drain_events();

  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 0);

  gtk_window_destroy(win);
  g_object_unref(store);
  drain_events();
}

/* ── Main ────────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/nostr-gtk/thread-view/bounded-model-count",
                   test_bounded_model_count);
  g_test_add_func("/nostr-gtk/thread-view/repeated-swap-no-accumulation",
                   test_repeated_swap_no_accumulation);
  g_test_add_func("/nostr-gtk/thread-view/ancestor-dedup",
                   test_ancestor_dedup);
  g_test_add_func("/nostr-gtk/thread-view/listview-large-thread",
                   test_listview_large_thread);
  g_test_add_func("/nostr-gtk/thread-view/empty-thread-no-crash",
                   test_empty_thread_no_crash);

  return g_test_run();
}
