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

typedef struct {
  guint position;
  guint removed;
  guint added;
} ChangedSpan;

static void
collect_changed_spans(GListModel *model,
                      guint position,
                      guint removed,
                      guint added,
                      gpointer user_data)
{
  (void)model;
  GArray *spans = user_data;
  ChangedSpan span = {
    .position = position,
    .removed = removed,
    .added = added,
  };
  g_array_append_val(spans, span);
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

static void
test_snapshot_model_insert_at_head(void)
{
  GnostrTimelineSnapshotModel *model = gnostr_timeline_snapshot_model_new();
  ItemsChangedCapture capture = { 0 };
  g_signal_connect(model, "items-changed", G_CALLBACK(on_items_changed), &capture);

  GnostrTimelineSnapshotRow *a = row_new("event-a", 200, "a", 10.0);
  GnostrTimelineSnapshotRow *b = row_new("event-b", 100, "b", 10.0);
  GnostrTimelineSnapshotRow *head = row_new("event-head", 300, "head", 10.0);
  GnostrTimelineSnapshotRow *old_rows[] = { a, b };
  GnostrTimelineSnapshotRow *new_rows[] = { head, a, b };
  GnostrTimelineSnapshot *old_snapshot =
    gnostr_timeline_snapshot_new_sorted(1, 1, old_rows, G_N_ELEMENTS(old_rows), 0);
  GnostrTimelineSnapshot *new_snapshot =
    gnostr_timeline_snapshot_new_sorted(2, 1, new_rows, G_N_ELEMENTS(new_rows), 0);

  gnostr_timeline_snapshot_model_replace_snapshot(model, old_snapshot);
  capture = (ItemsChangedCapture){ 0 };
  gnostr_timeline_snapshot_model_replace_snapshot(model, new_snapshot);

  g_assert_cmpuint(capture.count, ==, 1);
  g_assert_cmpuint(capture.position, ==, 0);
  g_assert_cmpuint(capture.removed, ==, 0);
  g_assert_cmpuint(capture.added, ==, 1);

  g_object_unref(model);
  g_object_unref(old_snapshot);
  g_object_unref(new_snapshot);
  g_object_unref(a);
  g_object_unref(b);
  g_object_unref(head);
}

static void
test_snapshot_model_remove_mid(void)
{
  GnostrTimelineSnapshotModel *model = gnostr_timeline_snapshot_model_new();
  ItemsChangedCapture capture = { 0 };
  g_signal_connect(model, "items-changed", G_CALLBACK(on_items_changed), &capture);

  GnostrTimelineSnapshotRow *a = row_new("event-a", 300, "a", 10.0);
  GnostrTimelineSnapshotRow *b = row_new("event-b", 200, "b", 10.0);
  GnostrTimelineSnapshotRow *c = row_new("event-c", 100, "c", 10.0);
  GnostrTimelineSnapshotRow *old_rows[] = { a, b, c };
  GnostrTimelineSnapshotRow *new_rows[] = { a, c };
  GnostrTimelineSnapshot *old_snapshot =
    gnostr_timeline_snapshot_new_sorted(1, 1, old_rows, G_N_ELEMENTS(old_rows), 0);
  GnostrTimelineSnapshot *new_snapshot =
    gnostr_timeline_snapshot_new_sorted(2, 1, new_rows, G_N_ELEMENTS(new_rows), 0);

  gnostr_timeline_snapshot_model_replace_snapshot(model, old_snapshot);
  capture = (ItemsChangedCapture){ 0 };
  gnostr_timeline_snapshot_model_replace_snapshot(model, new_snapshot);

  g_assert_cmpuint(capture.count, ==, 1);
  g_assert_cmpuint(capture.position, ==, 1);
  g_assert_cmpuint(capture.removed, ==, 1);
  g_assert_cmpuint(capture.added, ==, 0);

  g_object_unref(model);
  g_object_unref(old_snapshot);
  g_object_unref(new_snapshot);
  g_object_unref(a);
  g_object_unref(b);
  g_object_unref(c);
}

static void
test_snapshot_model_mixed_coalesced_spans(void)
{
  GnostrTimelineSnapshotModel *model = gnostr_timeline_snapshot_model_new();
  GArray *spans = g_array_new(FALSE, FALSE, sizeof(ChangedSpan));
  g_signal_connect(model, "items-changed", G_CALLBACK(collect_changed_spans), spans);

  GnostrTimelineSnapshotRow *a = row_new("event-a", 600, "a", 10.0);
  GnostrTimelineSnapshotRow *b = row_new("event-b", 500, "b", 10.0);
  GnostrTimelineSnapshotRow *c = row_new("event-c", 400, "c", 10.0);
  GnostrTimelineSnapshotRow *d = row_new("event-d", 300, "d", 10.0);
  GnostrTimelineSnapshotRow *e = row_new("event-e", 200, "e", 10.0);
  GnostrTimelineSnapshotRow *f = row_new("event-f", 100, "f", 10.0);
  GnostrTimelineSnapshotRow *head = row_new("event-head", 700, "head", 10.0);
  GnostrTimelineSnapshotRow *b_replacement = row_new("event-b", 500, "b", 20.0);
  GnostrTimelineSnapshotRow *x = row_new("event-x", 450, "x", 10.0);
  GnostrTimelineSnapshotRow *tail = row_new("event-tail", 50, "tail", 10.0);
  GnostrTimelineSnapshotRow *old_rows[] = { a, b, c, d, e, f };
  GnostrTimelineSnapshotRow *new_rows[] = {
    head, a, b_replacement, x, d, f, tail
  };
  GnostrTimelineSnapshot *old_snapshot =
    gnostr_timeline_snapshot_new_sorted(1, 1, old_rows, G_N_ELEMENTS(old_rows), 0);
  GnostrTimelineSnapshot *new_snapshot =
    gnostr_timeline_snapshot_new_sorted(2, 1, new_rows, G_N_ELEMENTS(new_rows), 0);

  gnostr_timeline_snapshot_model_replace_snapshot(model, old_snapshot);
  g_array_set_size(spans, 0);
  gnostr_timeline_snapshot_model_replace_snapshot(model, new_snapshot);

  const ChangedSpan expected[] = {
    { 0, 0, 1 },
    { 2, 2, 2 },
    { 5, 1, 0 },
    { 6, 0, 1 },
  };
  g_assert_cmpuint(spans->len, ==, G_N_ELEMENTS(expected));
  for (guint i = 0; i < spans->len; i++) {
    ChangedSpan actual = g_array_index(spans, ChangedSpan, i);
    g_assert_cmpuint(actual.position, ==, expected[i].position);
    g_assert_cmpuint(actual.removed, ==, expected[i].removed);
    g_assert_cmpuint(actual.added, ==, expected[i].added);
  }

  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, G_N_ELEMENTS(new_rows));

  g_object_unref(model);
  g_array_unref(spans);
  g_object_unref(old_snapshot);
  g_object_unref(new_snapshot);
  g_object_unref(a);
  g_object_unref(b);
  g_object_unref(c);
  g_object_unref(d);
  g_object_unref(e);
  g_object_unref(f);
  g_object_unref(head);
  g_object_unref(b_replacement);
  g_object_unref(x);
  g_object_unref(tail);
}

static void
test_snapshot_model_large_single_replacement_is_linear(void)
{
  const guint n_rows = 10000;
  const guint replacement_index = n_rows / 2;
  GnostrTimelineSnapshotModel *model = gnostr_timeline_snapshot_model_new();
  ItemsChangedCapture capture = { 0 };
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);
  g_signal_connect(model, "items-changed", G_CALLBACK(on_items_changed), &capture);

  for (guint i = 0; i < n_rows; i++) {
    g_autofree char *event_id = g_strdup_printf("event-%05u", i);
    g_ptr_array_add(rows, row_new(event_id, (gint64)(n_rows - i), event_id, 10.0));
  }

  GnostrTimelineSnapshot *old_snapshot =
    gnostr_timeline_snapshot_new_sorted(1,
                                        1,
                                        (GnostrTimelineSnapshotRow * const *)rows->pdata,
                                        rows->len,
                                        0);
  GnostrTimelineSnapshotRow **new_rows = g_new(GnostrTimelineSnapshotRow *, n_rows);
  for (guint i = 0; i < n_rows; i++)
    new_rows[i] = g_ptr_array_index(rows, i);

  g_autofree char *replacement_id = g_strdup_printf("event-%05u", replacement_index);
  GnostrTimelineSnapshotRow *replacement =
    row_new(replacement_id,
            (gint64)(n_rows - replacement_index),
            replacement_id,
            20.0);
  new_rows[replacement_index] = replacement;
  GnostrTimelineSnapshot *new_snapshot =
    gnostr_timeline_snapshot_new_sorted(2, 1, new_rows, n_rows, 0);
  g_free(new_rows);

  gnostr_timeline_snapshot_model_replace_snapshot(model, old_snapshot);
  capture = (ItemsChangedCapture){ 0 };
  gnostr_timeline_snapshot_model_replace_snapshot(model, new_snapshot);

  g_assert_cmpuint(capture.count, ==, 1);
  g_assert_cmpuint(capture.position, ==, replacement_index);
  g_assert_cmpuint(capture.removed, ==, 1);
  g_assert_cmpuint(capture.added, ==, 1);
  g_assert_cmpuint(gnostr_timeline_snapshot_model_get_last_diff_work(model),
                   <=,
                   (guint64)n_rows * 8 + 32);

  g_object_unref(model);
  g_object_unref(old_snapshot);
  g_object_unref(new_snapshot);
  g_object_unref(replacement);
  g_ptr_array_unref(rows);
}

static void
test_snapshot_row_proxies_view_model_parsed_artifact(void)
{
  GPtrArray *descriptors =
    g_ptr_array_new_with_free_func((GDestroyNotify)gn_content_descriptor_free);
  GnContentDescriptor *descriptor = g_new0(GnContentDescriptor, 1);
  descriptor->type = GN_CONTENT_DESCRIPTOR_LINK_PREVIEW;
  descriptor->url = g_strdup("https://example.test/page");
  g_ptr_array_add(descriptors, descriptor);
  GnostrTimelineParsedContent *artifact =
    gnostr_timeline_parsed_content_new_take(
      g_strdup("shared content"), g_strdup("shared content"),
      g_strdup("shared content"), descriptors, NULL, NULL, NULL, NULL, 0);
  GnostrTimelineItemViewModelSpec spec = {
    .event_id = "shared-event",
    .note_key = "44",
    .note_key_u64 = 44,
    .pubkey = "shared-pubkey",
    .created_at = 123,
    .tie_breaker = "shared-event",
    .kind = 1,
    .parsed_content = artifact,
  };
  g_autofree char *signature =
    gnostr_timeline_item_view_model_spec_recompute_derived_fields(&spec);
  g_autoptr(GnostrTimelineItemViewModel) vm =
    gnostr_timeline_item_view_model_new(&spec);
  gnostr_timeline_parsed_content_unref(artifact);

  g_autoptr(GnostrTimelineSnapshotRow) row =
    gnostr_timeline_snapshot_row_new_from_view_model(
      vm, 320.0, 0.0, 320.0, 0.0, 120.0, 0.0, 480,
      "layout-shared", FALSE);
  g_assert_true(gnostr_timeline_snapshot_row_get_content(row) ==
                gnostr_timeline_item_view_model_get_content(vm));
  g_assert_true(gnostr_timeline_snapshot_row_get_content_descriptors(row) ==
                gnostr_timeline_item_view_model_get_content_descriptors(vm));
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_event_id(row), ==,
                  "shared-event");
  g_autoptr(GnostrTimelineItemViewModel) row_vm =
    gnostr_timeline_snapshot_row_dup_view_model(row);
  g_assert_true(row_vm == vm);
  g_assert_true(gnostr_timeline_item_view_model_get_parsed_content(row_vm) ==
                gnostr_timeline_item_view_model_get_parsed_content(vm));
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
  g_test_add_func("/gnostr/timeline-snapshot/insert-at-head", test_snapshot_model_insert_at_head);
  g_test_add_func("/gnostr/timeline-snapshot/remove-mid", test_snapshot_model_remove_mid);
  g_test_add_func("/gnostr/timeline-snapshot/mixed-coalesced-spans", test_snapshot_model_mixed_coalesced_spans);
  g_test_add_func("/gnostr/timeline-snapshot/large-single-replacement-linear",
                  test_snapshot_model_large_single_replacement_is_linear);
  g_test_add_func("/gnostr/timeline-snapshot/row-proxies-parsed-artifact",
                  test_snapshot_row_proxies_view_model_parsed_artifact);

  return g_test_run();
}
