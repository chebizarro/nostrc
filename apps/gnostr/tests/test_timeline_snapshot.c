#include <glib.h>
#include <gio/gio.h>

#include "../src/model/gnostr-timeline-snapshot.h"
#include "../src/model/gnostr-timeline-snapshot-model.h"

static GnostrTimelineSnapshotRow *
row_new(const char *event_id,
        gint64 created_at,
        const char *tie_breaker,
        double height)
{
  return gnostr_timeline_snapshot_row_new(event_id,
                                          event_id,
                                          "pubkey",
                                          created_at,
                                          tie_breaker,
                                          "content",
                                          height,
                                          0.0,
                                          height,
                                          0.0,
                                          0.0,
                                          480,
                                          "layout-v1",
                                          FALSE);
}

static void
test_snapshot_ordering_lookup_and_prefix(void)
{
  GnostrTimelineSnapshotRow *older = row_new("event-c", 100, "c", 30.0);
  GnostrTimelineSnapshotRow *newer_b = row_new("event-b", 200, "b", 20.0);
  GnostrTimelineSnapshotRow *newer_a = row_new("event-a", 200, "a", 10.0);
  GnostrTimelineSnapshotRow *rows[] = { older, newer_b, newer_a };

  GnostrTimelineSnapshot *snapshot = gnostr_timeline_snapshot_new(7, 3, rows, G_N_ELEMENTS(rows), 2);

  g_assert_cmpuint(gnostr_timeline_snapshot_get_generation(snapshot), ==, 7);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_query_generation(snapshot), ==, 3);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_pending_head_count(snapshot), ==, 2);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(snapshot), ==, 3);

  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_event_id(gnostr_timeline_snapshot_get_row(snapshot, 0)), ==, "event-a");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_event_id(gnostr_timeline_snapshot_get_row(snapshot, 1)), ==, "event-b");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_event_id(gnostr_timeline_snapshot_get_row(snapshot, 2)), ==, "event-c");

  guint index = G_MAXUINT;
  g_assert_true(gnostr_timeline_snapshot_lookup_event(snapshot, "event-b", &index));
  g_assert_cmpuint(index, ==, 1);
  g_assert_false(gnostr_timeline_snapshot_lookup_event(snapshot, "missing", &index));

  g_assert_cmpfloat_with_epsilon(gnostr_timeline_snapshot_get_row_top(snapshot, 0), 0.0, 0.001);
  g_assert_cmpfloat_with_epsilon(gnostr_timeline_snapshot_get_row_bottom(snapshot, 0), 10.0, 0.001);
  g_assert_cmpfloat_with_epsilon(gnostr_timeline_snapshot_get_row_top(snapshot, 1), 10.0, 0.001);
  g_assert_cmpfloat_with_epsilon(gnostr_timeline_snapshot_get_row_bottom(snapshot, 1), 30.0, 0.001);
  g_assert_cmpfloat_with_epsilon(gnostr_timeline_snapshot_get_row_top(snapshot, 2), 30.0, 0.001);
  g_assert_cmpfloat_with_epsilon(gnostr_timeline_snapshot_get_total_height(snapshot), 60.0, 0.001);

  g_object_unref(snapshot);
  g_object_unref(older);
  g_object_unref(newer_b);
  g_object_unref(newer_a);
}

static void
test_row_replacement_is_object_replacement(void)
{
  GnostrTimelineSnapshotRow *first = gnostr_timeline_snapshot_row_new("event-a", "note-a", "pubkey-a",
                                                                      100, "event-a", "first", 10.0,
                                                                      0.0, 10.0, 0.0, 0.0, 480, "layout-v1", FALSE);
  GnostrTimelineSnapshotRow *replacement = gnostr_timeline_snapshot_row_new("event-a", "note-a", "pubkey-a",
                                                                            100, "event-a", "replacement", 20.0,
                                                                            20.0, 20.0, 0.0, 0.0, 480, "layout-v2", TRUE);
  GnostrTimelineSnapshotRow *first_rows[] = { first };
  GnostrTimelineSnapshotRow *replacement_rows[] = { replacement };
  GnostrTimelineSnapshot *snapshot_1 = gnostr_timeline_snapshot_new(1, 1, first_rows, 1, 0);
  GnostrTimelineSnapshot *snapshot_2 = gnostr_timeline_snapshot_new(2, 1, replacement_rows, 1, 0);

  GnostrTimelineSnapshotRow *snap_1_row = gnostr_timeline_snapshot_get_row(snapshot_1, 0);
  GnostrTimelineSnapshotRow *snap_2_row = gnostr_timeline_snapshot_get_row(snapshot_2, 0);

  g_assert_true(snap_1_row == first);
  g_assert_true(snap_2_row == replacement);
  g_assert_true(snap_1_row != snap_2_row);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_content(snap_1_row), ==, "first");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_content(snap_2_row), ==, "replacement");

  g_object_unref(snapshot_1);
  g_object_unref(snapshot_2);
  g_object_unref(first);
  g_object_unref(replacement);
}

typedef struct {
  guint position;
  guint removed;
  guint added;
  guint count;
} ItemsChangedCapture;

static void
on_items_changed(GListModel *model,
                 guint position,
                 guint removed,
                 guint added,
                 gpointer user_data)
{
  (void)model;
  ItemsChangedCapture *capture = user_data;
  capture->position = position;
  capture->removed = removed;
  capture->added = added;
  capture->count++;
}

static void
test_snapshot_model_replacement(void)
{
  GnostrTimelineSnapshotModel *model = gnostr_timeline_snapshot_model_new();
  ItemsChangedCapture capture = { 0, 0, 0, 0 };
  g_signal_connect(model, "items-changed", G_CALLBACK(on_items_changed), &capture);

  GnostrTimelineSnapshotRow *a = row_new("event-a", 100, "a", 10.0);
  GnostrTimelineSnapshotRow *b = row_new("event-b", 90, "b", 10.0);
  GnostrTimelineSnapshotRow *first_rows[] = { a, b };
  GnostrTimelineSnapshot *first = gnostr_timeline_snapshot_new(1, 1, first_rows, 2, 0);

  gnostr_timeline_snapshot_model_replace_snapshot(model, first);
  g_assert_cmpuint(capture.count, ==, 1);
  g_assert_cmpuint(capture.position, ==, 0);
  g_assert_cmpuint(capture.removed, ==, 0);
  g_assert_cmpuint(capture.added, ==, 2);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 2);
  g_assert_true(gnostr_timeline_snapshot_model_get_snapshot(model) == first);

  GObject *item = g_list_model_get_item(G_LIST_MODEL(model), 0);
  g_assert_true(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(item));
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_event_id(GNOSTR_TIMELINE_SNAPSHOT_ROW(item)), ==, "event-a");
  g_object_unref(item);

  GnostrTimelineSnapshotRow *c = row_new("event-c", 110, "c", 10.0);
  GnostrTimelineSnapshotRow *second_rows[] = { c };
  GnostrTimelineSnapshot *second = gnostr_timeline_snapshot_new(2, 1, second_rows, 1, 0);

  gnostr_timeline_snapshot_model_replace_snapshot(model, second);
  g_assert_cmpuint(capture.count, ==, 2);
  g_assert_cmpuint(capture.position, ==, 0);
  g_assert_cmpuint(capture.removed, ==, 2);
  g_assert_cmpuint(capture.added, ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 1);
  g_assert_true(gnostr_timeline_snapshot_model_get_snapshot(model) == second);

  g_object_unref(model);
  g_object_unref(first);
  g_object_unref(second);
  g_object_unref(a);
  g_object_unref(b);
  g_object_unref(c);
}

/* Regression: multi-span diff publication must keep the model consistent
 * with consumers at EVERY items-changed emission (GListModel contract).
 * The shadow array mimics GTK's list item manager bookkeeping; under the
 * old implementation (swap to final array, then emit spans) the n_items
 * check below failed mid-sequence and GTK crashed. */
static void
shadow_items_changed(GListModel *model,
                     guint position,
                     guint removed,
                     guint added,
                     gpointer user_data)
{
  GPtrArray *shadow = user_data;

  g_assert_cmpuint(position, <=, shadow->len);
  g_assert_cmpuint(removed, <=, shadow->len - position);

  if (removed > 0)
    g_ptr_array_remove_range(shadow, position, removed);
  for (guint k = 0; k < added; k++) {
    gpointer item = g_list_model_get_item(model, position + k);
    g_assert_nonnull(item);
    g_ptr_array_insert(shadow, (gint)(position + k), item); /* takes ref */
  }

  /* Model must agree with the consumer view after applying this span. */
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, shadow->len);
}

static void
test_snapshot_model_multi_span_diff_consistency(void)
{
  GnostrTimelineSnapshotModel *model = gnostr_timeline_snapshot_model_new();
  GPtrArray *shadow = g_ptr_array_new_with_free_func(g_object_unref);
  g_signal_connect(model, "items-changed", G_CALLBACK(shadow_items_changed), shadow);

  GnostrTimelineSnapshotRow *a = row_new("event-a", 400, "a", 10.0);
  GnostrTimelineSnapshotRow *b = row_new("event-b", 300, "b", 10.0);
  GnostrTimelineSnapshotRow *c = row_new("event-c", 200, "c", 10.0);
  GnostrTimelineSnapshotRow *d = row_new("event-d", 100, "d", 10.0);
  GnostrTimelineSnapshotRow *first_rows[] = { a, b, c, d };
  GnostrTimelineSnapshot *first = gnostr_timeline_snapshot_new(1, 1, first_rows, 4, 0);
  gnostr_timeline_snapshot_model_replace_snapshot(model, first);
  g_assert_cmpuint(shadow->len, ==, 4);

  /* Second snapshot: keep a and c (same pointers), replace b with x,
   * replace d with e — forces two disjoint diff spans. */
  GnostrTimelineSnapshotRow *x = row_new("event-x", 250, "x", 10.0);
  GnostrTimelineSnapshotRow *e = row_new("event-e", 50, "e", 10.0);
  GnostrTimelineSnapshotRow *second_rows[] = { a, x, c, e };
  GnostrTimelineSnapshot *second = gnostr_timeline_snapshot_new(2, 1, second_rows, 4, 0);
  gnostr_timeline_snapshot_model_replace_snapshot(model, second);

  /* Consumer view must equal the final model contents. */
  g_assert_cmpuint(shadow->len, ==, 4);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 4);
  for (guint i = 0; i < shadow->len; i++) {
    GObject *item = g_list_model_get_item(G_LIST_MODEL(model), i);
    g_assert_true(item == g_ptr_array_index(shadow, i));
    g_object_unref(item);
  }
  GObject *row1 = g_list_model_get_item(G_LIST_MODEL(model), 1);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_event_id(GNOSTR_TIMELINE_SNAPSHOT_ROW(row1)), ==, "event-x");
  g_object_unref(row1);

  /* Two spans where the FIRST changes the item count (remove x, then
   * replace e with f+g). Under the old implementation the model already
   * reported the final length during the first emission while the
   * consumer was still mid-sequence — exactly the divergence that
   * crashed GTK's list item manager. */
  GnostrTimelineSnapshotRow *f = row_new("event-f", 150, "f", 10.0);
  GnostrTimelineSnapshotRow *g = row_new("event-g", 120, "g", 10.0);
  GnostrTimelineSnapshotRow *third_rows[] = { a, c, f, g };
  GnostrTimelineSnapshot *third = gnostr_timeline_snapshot_new(3, 1, third_rows, 4, 0);
  gnostr_timeline_snapshot_model_replace_snapshot(model, third);
  g_assert_cmpuint(shadow->len, ==, 4);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 4);
  for (guint i = 0; i < shadow->len; i++) {
    GObject *item = g_list_model_get_item(G_LIST_MODEL(model), i);
    g_assert_true(item == g_ptr_array_index(shadow, i));
    g_object_unref(item);
  }

  g_object_unref(model);
  g_ptr_array_unref(shadow);
  g_object_unref(first);
  g_object_unref(second);
  g_object_unref(third);
  g_object_unref(a);
  g_object_unref(b);
  g_object_unref(c);
  g_object_unref(d);
  g_object_unref(x);
  g_object_unref(e);
  g_object_unref(f);
  g_object_unref(g);
}

int
main(int argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/timeline-snapshot/ordering-lookup-prefix", test_snapshot_ordering_lookup_and_prefix);
  g_test_add_func("/gnostr/timeline-snapshot/row-replacement", test_row_replacement_is_object_replacement);
  g_test_add_func("/gnostr/timeline-snapshot/model-replacement", test_snapshot_model_replacement);
  g_test_add_func("/gnostr/timeline-snapshot/multi-span-diff-consistency", test_snapshot_model_multi_span_diff_consistency);

  return g_test_run();
}
