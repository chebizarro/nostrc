#define G_LOG_DOMAIN "gnostr-timeline-feed-controller"

#include "gnostr-timeline-feed-controller.h"
#include "../model/gnostr-timeline-geometry.h"
#include "../model/gnostr-timeline-hydrator.h"

#include <string.h>

#define COMPOSE_DEBOUNCE_MS     50u
#define AT_TOP_EPSILON_PX       1.0
#define DEFAULT_VISIBLE_PAGE_SIZE 30u

typedef struct {
  GnostrTimelineItemViewModel *vm;
} WorkingEntry;

struct _GnostrTimelineFeedController {
  GObject parent_instance;

  GnostrTimelineSource *source;
  gulong source_batch_handler;
  guint64 query_generation;
  guint64 snapshot_generation;

  GnostrTimelineHydrator *hydrator;
  GnostrTimelineSnapshotModel *model;

  GPtrArray *working;       /* element-type: WorkingEntry* */
  GHashTable *by_event_id;  /* char* -> WorkingEntry* (borrowed value) */
  GHashTable *pending_head; /* char* set of event ids hidden from visible snapshots */
  GHashTable *deferred_patch_events; /* char* set of visible rows awaiting safe patch publication */
  GHashTable *deferred_replacement_vms; /* char* -> GnostrTimelineItemViewModel* awaiting admission */
  GnostrTimelineGeometryResolver *geometry;

  guint visible_limit; /* number of sorted, non-pending rows admitted to snapshots */
  guint page_size;

  gboolean user_at_top;
  double scroll_y;
  double viewport_height;
  guint width_bucket;

  guint compose_source_id;
  gboolean scheduled_preserve_anchor;
  gboolean scheduled_scroll_to_top;
  gboolean loading_older;
  gboolean loading_newer;
};

G_DEFINE_TYPE(GnostrTimelineFeedController,
              gnostr_timeline_feed_controller,
              G_TYPE_OBJECT)

enum {
  SIGNAL_PENDING_COUNT_CHANGED,
  SIGNAL_RESTORE_SCROLL,
  SIGNAL_SNAPSHOT_PUBLISHED,
  SIGNAL_NEED_PROFILE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void schedule_compose(GnostrTimelineFeedController *self,
                             gboolean preserve_anchor,
                             gboolean scroll_to_top);
static void compose_and_publish(GnostrTimelineFeedController *self,
                                gboolean preserve_anchor,
                                gboolean scroll_to_top);

static WorkingEntry *
working_entry_new_from_vm(GnostrTimelineItemViewModel *vm)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(vm), NULL);

  WorkingEntry *we = g_new0(WorkingEntry, 1);
  we->vm = g_object_ref(vm);
  return we;
}

static void
working_entry_free(WorkingEntry *we)
{
  if (!we)
    return;
  g_clear_object(&we->vm);
  g_free(we);
}

static void
working_entry_replace_vm(WorkingEntry *we,
                         GnostrTimelineItemViewModel *vm)
{
  if (!we || !GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(vm))
    return;
  g_set_object(&we->vm, vm);
}

static gboolean
batch_generation_is_current(GnostrTimelineFeedController *self,
                            GnostrTimelineBatch *batch)
{
  guint64 batch_generation = gnostr_timeline_batch_get_generation(batch);
  guint64 current_generation = self->query_generation;
#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  if (self->source)
    current_generation = gnostr_timeline_source_get_generation(self->source);
#endif

  if (batch_generation != current_generation) {
    g_debug("[COMPOSITOR] Dropping stale %s batch gen=%" G_GUINT64_FORMAT
            " current=%" G_GUINT64_FORMAT,
            gnostr_timeline_batch_kind_to_string(gnostr_timeline_batch_get_kind(batch)),
            batch_generation,
            current_generation);
    return FALSE;
  }

  return TRUE;
}

static WorkingEntry *
lookup_working(GnostrTimelineFeedController *self,
               const char *event_id)
{
  if (!event_id)
    return NULL;
  return g_hash_table_lookup(self->by_event_id, event_id);
}

static WorkingEntry *
merge_hydrated_vm(GnostrTimelineFeedController *self,
                  GnostrTimelineItemViewModel *vm,
                  gboolean *out_inserted)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(vm), NULL);

  const char *event_id = gnostr_timeline_item_view_model_get_event_id(vm);
  WorkingEntry *existing = lookup_working(self, event_id);
  if (existing) {
    working_entry_replace_vm(existing, vm);
    if (out_inserted)
      *out_inserted = FALSE;
    return existing;
  }

  WorkingEntry *created = working_entry_new_from_vm(vm);
  g_ptr_array_add(self->working, created);
  g_hash_table_insert(self->by_event_id, g_strdup(event_id), created);

  if (out_inserted)
    *out_inserted = TRUE;
  return created;
}

static GPtrArray *
hydrate_batch_items(GnostrTimelineFeedController *self,
                    GnostrTimelineBatch *batch)
{
  if (self->hydrator)
    gnostr_timeline_hydrator_set_generation(self->hydrator, self->query_generation);
  return gnostr_timeline_hydrator_hydrate_batch(self->hydrator, batch);
}

static void
clear_working_set(GnostrTimelineFeedController *self)
{
  g_ptr_array_set_size(self->working, 0);
  g_hash_table_remove_all(self->by_event_id);
  g_hash_table_remove_all(self->pending_head);
  g_hash_table_remove_all(self->deferred_patch_events);
  g_hash_table_remove_all(self->deferred_replacement_vms);
  self->visible_limit = 0;
}

static gboolean
remove_working_event(GnostrTimelineFeedController *self,
                     const char *event_id,
                     gboolean *out_pending_changed,
                     gboolean *out_was_pending)
{
  if (!event_id || !*event_id)
    return FALSE;

  WorkingEntry *entry = lookup_working(self, event_id);
  if (!entry)
    return FALSE;

  gboolean was_pending = g_hash_table_remove(self->pending_head, event_id);
  if (was_pending && out_pending_changed)
    *out_pending_changed = TRUE;
  if (out_was_pending)
    *out_was_pending = was_pending;

  g_hash_table_remove(self->deferred_patch_events, event_id);
  g_hash_table_remove(self->deferred_replacement_vms, event_id);
  g_hash_table_remove(self->by_event_id, event_id);

  for (guint i = 0; i < self->working->len; i++) {
    if (g_ptr_array_index(self->working, i) == entry) {
      g_ptr_array_remove_index(self->working, i);
      return TRUE;
    }
  }

  return TRUE;
}

static guint
pending_count(GnostrTimelineFeedController *self)
{
  return self->pending_head ? g_hash_table_size(self->pending_head) : 0;
}

static guint
count_publishable_entries(GnostrTimelineFeedController *self)
{
  guint count = 0;
  for (guint i = 0; i < self->working->len; i++) {
    WorkingEntry *entry = g_ptr_array_index(self->working, i);
    GnostrTimelineItemViewModel *vm = entry ? entry->vm : NULL;
    const char *event_id = vm ? gnostr_timeline_item_view_model_get_event_id(vm) : NULL;
    if (event_id && *event_id && !g_hash_table_contains(self->pending_head, event_id))
      count++;
  }
  return count;
}

static void
set_initial_visible_limit(GnostrTimelineFeedController *self)
{
  guint eligible = count_publishable_entries(self);
  self->visible_limit = MIN(self->page_size, eligible);
}

static void
expand_visible_limit(GnostrTimelineFeedController *self,
                     guint delta)
{
  if (delta == 0)
    return;

  guint eligible = count_publishable_entries(self);
  guint64 expanded = (guint64)self->visible_limit + delta;
  self->visible_limit = (guint)MIN((guint64)eligible, expanded);
}

static void
clamp_visible_limit(GnostrTimelineFeedController *self)
{
  guint eligible = count_publishable_entries(self);
  self->visible_limit = MIN(self->visible_limit, eligible);
}

static void
emit_pending_count(GnostrTimelineFeedController *self)
{
  g_signal_emit(self, signals[SIGNAL_PENDING_COUNT_CHANGED], 0, pending_count(self));
}

static void
emit_profile_requests(GnostrTimelineFeedController *self,
                      GnostrTimelineBatch *batch)
{
  guint n = gnostr_timeline_batch_get_n_profile_requests(batch);
  for (guint i = 0; i < n; i++) {
    const char *pubkey = gnostr_timeline_batch_get_profile_request(batch, i);
    if (pubkey && *pubkey)
      g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey);
  }
}

static GnostrTimelineSnapshot *
dup_current_snapshot(GnostrTimelineFeedController *self)
{
  return gnostr_timeline_snapshot_model_dup_snapshot(self->model);
}

static gboolean
capture_anchor(GnostrTimelineFeedController *self,
               GnostrTimelineSnapshot *snapshot,
               GnostrTimelineAnchor *out_anchor)
{
  if (!snapshot || !out_anchor)
    return FALSE;

  guint n_rows = gnostr_timeline_snapshot_get_n_rows(snapshot);
  if (n_rows == 0)
    return FALSE;

  double scroll_y = MAX(self->scroll_y, 0.0);
  guint lo = 0;
  guint hi = n_rows;
  while (lo < hi) {
    guint mid = lo + (hi - lo) / 2;
    double bottom = gnostr_timeline_snapshot_get_row_bottom(snapshot, mid);
    if (bottom <= scroll_y)
      lo = mid + 1;
    else
      hi = mid;
  }

  guint index = MIN(lo, n_rows - 1);
  GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(snapshot, index);
  const char *event_id = row ? gnostr_timeline_snapshot_row_get_event_id(row) : NULL;
  if (!event_id || !*event_id)
    return FALSE;

  out_anchor->event_id = g_strdup(event_id);
  out_anchor->index_hint = index;
  out_anchor->offset_px_in_row = MAX(0.0, scroll_y - gnostr_timeline_snapshot_get_row_top(snapshot, index));
  out_anchor->snapshot_generation = gnostr_timeline_snapshot_get_generation(snapshot);
  return TRUE;
}

static gboolean
restore_anchor_scroll(GnostrTimelineSnapshot *old_snapshot,
                      GnostrTimelineSnapshot *new_snapshot,
                      const GnostrTimelineAnchor *anchor,
                      double *out_scroll_y)
{
  if (!old_snapshot || !new_snapshot || !anchor || !out_scroll_y)
    return FALSE;

  guint new_index = 0;
  if (anchor->event_id && gnostr_timeline_snapshot_lookup_event(new_snapshot, anchor->event_id, &new_index)) {
    *out_scroll_y = gnostr_timeline_snapshot_get_row_top(new_snapshot, new_index) + anchor->offset_px_in_row;
    return TRUE;
  }

  guint old_n = gnostr_timeline_snapshot_get_n_rows(old_snapshot);
  if (old_n == 0)
    return FALSE;

  guint hint = MIN(anchor->index_hint, old_n - 1);

  for (guint i = hint + 1; i < old_n; i++) {
    GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(old_snapshot, i);
    const char *event_id = row ? gnostr_timeline_snapshot_row_get_event_id(row) : NULL;
    if (event_id && gnostr_timeline_snapshot_lookup_event(new_snapshot, event_id, &new_index)) {
      *out_scroll_y = gnostr_timeline_snapshot_get_row_top(new_snapshot, new_index) + anchor->offset_px_in_row;
      return TRUE;
    }
  }

  for (gint i = (gint)hint - 1; i >= 0; i--) {
    GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(old_snapshot, (guint)i);
    const char *event_id = row ? gnostr_timeline_snapshot_row_get_event_id(row) : NULL;
    if (event_id && gnostr_timeline_snapshot_lookup_event(new_snapshot, event_id, &new_index)) {
      *out_scroll_y = gnostr_timeline_snapshot_get_row_top(new_snapshot, new_index) + anchor->offset_px_in_row;
      return TRUE;
    }
  }

  if (gnostr_timeline_snapshot_get_n_rows(new_snapshot) > 0) {
    *out_scroll_y = 0.0;
    return TRUE;
  }

  return FALSE;
}

static void
resolve_footprint_for_vm(GnostrTimelineFeedController *self,
                         GnostrTimelineItemViewModel *vm,
                         GnostrTimelineRowFootprint *out_footprint)
{
  g_return_if_fail(out_footprint != NULL);

  GnostrTimelineGeometryInput input = {
    .event_id = vm ? gnostr_timeline_item_view_model_get_event_id(vm) : NULL,
    .content = vm ? gnostr_timeline_item_view_model_get_content(vm) : NULL,
    .root_id = vm ? gnostr_timeline_item_view_model_get_root_id(vm) : NULL,
    .reply_id = vm ? gnostr_timeline_item_view_model_get_reply_id(vm) : NULL,
    .quoted_event_id = vm ? gnostr_timeline_item_view_model_get_quoted_event_id(vm) : NULL,
    .reposted_event_id = vm ? gnostr_timeline_item_view_model_get_reposted_event_id(vm) : NULL,
    .geometry_signature = vm ? gnostr_timeline_item_view_model_get_geometry_signature(vm) : NULL,
    .kind = vm ? gnostr_timeline_item_view_model_get_kind(vm) : 1,
    .has_profile = vm ? gnostr_timeline_item_view_model_get_has_profile(vm) : FALSE,
    .moderation_state = vm ? gnostr_timeline_item_view_model_get_moderation_state(vm) : 0,
    .has_content_warning = vm ? (gnostr_timeline_item_view_model_get_content_warning(vm) != NULL) : FALSE,
    .media_reservation_count = vm ? gnostr_timeline_item_view_model_get_media_reservation_count(vm) : 0,
    .media_reserved_height = vm ? gnostr_timeline_item_view_model_get_media_reserved_height(vm) : 0.0,
    .link_preview_reservation_count = vm ? gnostr_timeline_item_view_model_get_link_preview_reservation_count(vm) : 0,
    .link_preview_reserved_height = vm ? gnostr_timeline_item_view_model_get_link_preview_reserved_height(vm) : 0.0,
    .has_reply_context_reservation = vm ? gnostr_timeline_item_view_model_get_has_reply_context_reservation(vm) : FALSE,
    .has_repost_context_reservation = vm ? gnostr_timeline_item_view_model_get_has_repost_context_reservation(vm) : FALSE,
    .has_quote_context_reservation = vm ? gnostr_timeline_item_view_model_get_has_quote_context_reservation(vm) : FALSE,
    .has_footer_action_reservation = TRUE,
    .initial_reserved_height = vm ? gnostr_timeline_item_view_model_get_initial_reserved_height(vm) : 0.0,
    .like_count = vm ? gnostr_timeline_item_view_model_get_like_count(vm) : 0,
    .repost_count = vm ? gnostr_timeline_item_view_model_get_repost_count(vm) : 0,
    .reply_count = vm ? gnostr_timeline_item_view_model_get_reply_count(vm) : 0,
    .zap_count = vm ? gnostr_timeline_item_view_model_get_zap_count(vm) : 0,
    .explicit_expanded = FALSE,
  };

  gnostr_timeline_geometry_resolver_resolve(self->geometry,
                                            &input,
                                            self->width_bucket,
                                            out_footprint);
}

static void
entry_resolve_footprint(GnostrTimelineFeedController *self,
                        WorkingEntry *entry,
                        GnostrTimelineRowFootprint *out_footprint)
{
  resolve_footprint_for_vm(self, entry ? entry->vm : NULL, out_footprint);
}

static gint
sort_working_entries_cb(gconstpointer a,
                        gconstpointer b)
{
  WorkingEntry *entry_a = *(WorkingEntry * const *)a;
  WorkingEntry *entry_b = *(WorkingEntry * const *)b;

  if (!entry_a || !entry_a->vm)
    return (!entry_b || !entry_b->vm) ? 0 : 1;
  if (!entry_b || !entry_b->vm)
    return -1;

  return gnostr_timeline_item_view_model_compare(entry_a->vm, entry_b->vm);
}

static GnostrTimelineSnapshotRow *
snapshot_row_from_entry(GnostrTimelineFeedController *self,
                        WorkingEntry *entry)
{
  if (!entry || !entry->vm)
    return NULL;

  GnostrTimelineItemViewModel *vm = entry->vm;
  GnostrTimelineRowFootprint footprint = { 0 };
  entry_resolve_footprint(self, entry, &footprint);

  GnostrTimelineSnapshotRow *row =
    gnostr_timeline_snapshot_row_new_from_view_model(vm,
                                                     footprint.estimated_height,
                                                     footprint.measured_height,
                                                     footprint.effective_height,
                                                     footprint.width_bucket,
                                                     footprint.layout_signature,
                                                     footprint.geometry_measured);
  gnostr_timeline_row_footprint_clear(&footprint);
  return row;
}

static GnostrTimelineSnapshot *
compose_snapshot(GnostrTimelineFeedController *self)
{
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);
  GPtrArray *eligible = g_ptr_array_new();

  for (guint i = 0; i < self->working->len; i++) {
    WorkingEntry *entry = g_ptr_array_index(self->working, i);
    GnostrTimelineItemViewModel *vm = entry ? entry->vm : NULL;
    const char *event_id = vm ? gnostr_timeline_item_view_model_get_event_id(vm) : NULL;
    if (!event_id || !*event_id)
      continue;
    if (g_hash_table_contains(self->pending_head, event_id))
      continue;
    g_ptr_array_add(eligible, entry);
  }

  g_ptr_array_sort(eligible, sort_working_entries_cb);
  clamp_visible_limit(self);

  guint n_visible = MIN(self->visible_limit, eligible->len);
  for (guint i = 0; i < n_visible; i++) {
    WorkingEntry *entry = g_ptr_array_index(eligible, i);
    GnostrTimelineSnapshotRow *row = snapshot_row_from_entry(self, entry);
    if (row)
      g_ptr_array_add(rows, row);
  }
  g_ptr_array_unref(eligible);

  self->snapshot_generation++;
  if (self->snapshot_generation == 0)
    self->snapshot_generation = 1;

  GnostrTimelineSnapshot *snapshot = gnostr_timeline_snapshot_new(self->snapshot_generation,
                                                                  self->query_generation,
                                                                  (GnostrTimelineSnapshotRow * const *)rows->pdata,
                                                                  rows->len,
                                                                  pending_count(self));
  g_ptr_array_unref(rows);
  return snapshot;
}

static gboolean
compose_timeout_cb(gpointer user_data)
{
  GnostrTimelineFeedController *self = GNOSTR_TIMELINE_FEED_CONTROLLER(user_data);
  self->compose_source_id = 0;

  gboolean preserve_anchor = self->scheduled_preserve_anchor;
  gboolean scroll_to_top = self->scheduled_scroll_to_top;
  self->scheduled_preserve_anchor = FALSE;
  self->scheduled_scroll_to_top = FALSE;

  compose_and_publish(self, preserve_anchor, scroll_to_top);
  return G_SOURCE_REMOVE;
}

static void
schedule_compose(GnostrTimelineFeedController *self,
                 gboolean preserve_anchor,
                 gboolean scroll_to_top)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  self->scheduled_preserve_anchor |= preserve_anchor;
  self->scheduled_scroll_to_top |= scroll_to_top;

  if (self->compose_source_id != 0)
    return;

  self->compose_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE,
                                               COMPOSE_DEBOUNCE_MS,
                                               compose_timeout_cb,
                                               g_object_ref(self),
                                               g_object_unref);
}

static void
compose_and_publish(GnostrTimelineFeedController *self,
                    gboolean preserve_anchor,
                    gboolean scroll_to_top)
{
  g_autoptr(GnostrTimelineSnapshot) old_snapshot = dup_current_snapshot(self);
  GnostrTimelineAnchor anchor = { 0 };
  gboolean have_anchor = FALSE;

  if (preserve_anchor && !scroll_to_top)
    have_anchor = capture_anchor(self, old_snapshot, &anchor);

  g_autoptr(GnostrTimelineSnapshot) snapshot = compose_snapshot(self);
  gnostr_timeline_snapshot_model_replace_snapshot(self->model, snapshot);

  if (scroll_to_top) {
    self->scroll_y = 0.0;
    g_signal_emit(self, signals[SIGNAL_RESTORE_SCROLL], 0, 0.0);
  } else if (have_anchor) {
    double restore_y = self->scroll_y;
    if (restore_anchor_scroll(old_snapshot, snapshot, &anchor, &restore_y)) {
      restore_y = MAX(0.0, restore_y);
      self->scroll_y = restore_y;
      g_signal_emit(self, signals[SIGNAL_RESTORE_SCROLL], 0, restore_y);
    }
  }

  g_signal_emit(self, signals[SIGNAL_SNAPSHOT_PUBLISHED], 0, snapshot);
  gnostr_timeline_anchor_clear(&anchor);
}

typedef enum {
  PATCH_ADMISSION_NO_CHANGE,
  PATCH_ADMISSION_HIDDEN_ONLY,
  PATCH_ADMISSION_PUBLISH,
  PATCH_ADMISSION_BLOCKED,
} PatchAdmission;

static PatchAdmission
patch_admission_combine(PatchAdmission current,
                        PatchAdmission next)
{
  if (current == PATCH_ADMISSION_BLOCKED || next == PATCH_ADMISSION_BLOCKED)
    return PATCH_ADMISSION_BLOCKED;
  if (current == PATCH_ADMISSION_PUBLISH || next == PATCH_ADMISSION_PUBLISH)
    return PATCH_ADMISSION_PUBLISH;
  if (current == PATCH_ADMISSION_HIDDEN_ONLY || next == PATCH_ADMISSION_HIDDEN_ONLY)
    return PATCH_ADMISSION_HIDDEN_ONLY;
  return PATCH_ADMISSION_NO_CHANGE;
}

static gboolean
snapshot_row_intersects_viewport(GnostrTimelineFeedController *self,
                                 GnostrTimelineSnapshot *snapshot,
                                 guint index)
{
  if (!snapshot)
    return FALSE;
  if (self->viewport_height <= 0.0)
    return TRUE;

  double viewport_top = MAX(self->scroll_y, 0.0);
  double viewport_bottom = viewport_top + self->viewport_height;
  double row_top = gnostr_timeline_snapshot_get_row_top(snapshot, index);
  double row_bottom = gnostr_timeline_snapshot_get_row_bottom(snapshot, index);

  return row_bottom > viewport_top && row_top < viewport_bottom;
}

static PatchAdmission
admission_for_replacement_vm(GnostrTimelineFeedController *self,
                             const char *event_id,
                             GnostrTimelineItemViewModel *replacement)
{
  if (!event_id || !*event_id || !GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(replacement))
    return PATCH_ADMISSION_NO_CHANGE;

  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_current_snapshot(self);
  guint index = 0;
  if (!snapshot || !gnostr_timeline_snapshot_lookup_event(snapshot, event_id, &index))
    return PATCH_ADMISSION_HIDDEN_ONLY;

  GnostrTimelineSnapshotRow *current_row = gnostr_timeline_snapshot_get_row(snapshot, index);
  if (!current_row)
    return PATCH_ADMISSION_HIDDEN_ONLY;

  GnostrTimelineRowFootprint replacement_footprint = { 0 };
  resolve_footprint_for_vm(self, replacement, &replacement_footprint);

  gboolean same_layout =
    g_strcmp0(gnostr_timeline_snapshot_row_get_layout_signature(current_row),
              replacement_footprint.layout_signature) == 0;
  gboolean same_height =
    ABS(gnostr_timeline_snapshot_row_get_effective_height(current_row) -
        replacement_footprint.effective_height) < 0.001;
  gboolean geometry_safe = same_layout && same_height;
  gboolean in_viewport = snapshot_row_intersects_viewport(self, snapshot, index);
  gnostr_timeline_row_footprint_clear(&replacement_footprint);

  if (geometry_safe || !in_viewport)
    return PATCH_ADMISSION_PUBLISH;

  if (!g_hash_table_contains(self->deferred_patch_events, event_id))
    g_hash_table_add(self->deferred_patch_events, g_strdup(event_id));
  return PATCH_ADMISSION_BLOCKED;
}

static WorkingEntry *
merge_hydrated_vm_with_admission(GnostrTimelineFeedController *self,
                                 GnostrTimelineItemViewModel *vm,
                                 gboolean *out_inserted,
                                 PatchAdmission *out_admission)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(vm), NULL);

  if (out_admission)
    *out_admission = PATCH_ADMISSION_NO_CHANGE;

  const char *event_id = gnostr_timeline_item_view_model_get_event_id(vm);
  WorkingEntry *existing = lookup_working(self, event_id);
  if (!existing)
    return merge_hydrated_vm(self, vm, out_inserted);

  if (out_inserted)
    *out_inserted = FALSE;

  PatchAdmission admission = admission_for_replacement_vm(self, event_id, vm);
  if (out_admission)
    *out_admission = admission;

  if (admission == PATCH_ADMISSION_BLOCKED) {
    g_hash_table_replace(self->deferred_replacement_vms,
                         g_strdup(event_id),
                         g_object_ref(vm));
    return existing;
  }

  g_hash_table_remove(self->deferred_patch_events, event_id);
  g_hash_table_remove(self->deferred_replacement_vms, event_id);
  working_entry_replace_vm(existing, vm);
  return existing;
}

static gboolean
reevaluate_deferred_patch_events(GnostrTimelineFeedController *self)
{
  if (!self->deferred_patch_events || g_hash_table_size(self->deferred_patch_events) == 0)
    return FALSE;

  gboolean should_publish = FALSE;
  GHashTableIter iter;
  gpointer key = NULL;
  g_hash_table_iter_init(&iter, self->deferred_patch_events);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    const char *event_id = key;
    WorkingEntry *entry = lookup_working(self, event_id);
    if (!entry || !entry->vm) {
      g_hash_table_remove(self->deferred_replacement_vms, event_id);
      g_hash_table_iter_remove(&iter);
      continue;
    }

    GnostrTimelineItemViewModel *pending_replacement =
      g_hash_table_lookup(self->deferred_replacement_vms, event_id);
    GnostrTimelineItemViewModel *candidate = pending_replacement ? pending_replacement : entry->vm;
    PatchAdmission admission = admission_for_replacement_vm(self, event_id, candidate);
    if (admission == PATCH_ADMISSION_PUBLISH) {
      if (pending_replacement)
        working_entry_replace_vm(entry, pending_replacement);
      g_hash_table_remove(self->deferred_replacement_vms, event_id);
      should_publish = TRUE;
      g_hash_table_iter_remove(&iter);
    } else if (admission == PATCH_ADMISSION_HIDDEN_ONLY ||
               admission == PATCH_ADMISSION_NO_CHANGE) {
      if (pending_replacement)
        working_entry_replace_vm(entry, pending_replacement);
      g_hash_table_remove(self->deferred_replacement_vms, event_id);
      g_hash_table_iter_remove(&iter);
    }
  }

  return should_publish;
}

static PatchAdmission
apply_metadata_patch(GnostrTimelineFeedController *self,
                     const GnostrTimelineMetadataPatch *patch)
{
  if (!patch || !patch->event_id)
    return PATCH_ADMISSION_NO_CHANGE;

  WorkingEntry *entry = lookup_working(self, patch->event_id);
  if (!entry || !entry->vm)
    return PATCH_ADMISSION_NO_CHANGE;

  GnostrTimelineItemViewModel *deferred =
    g_hash_table_lookup(self->deferred_replacement_vms, patch->event_id);
  GnostrTimelineItemViewModel *base = deferred ? deferred : entry->vm;

  gboolean changed = FALSE;
  changed |= patch->has_like_count &&
    gnostr_timeline_item_view_model_get_like_count(base) != patch->like_count;
  changed |= patch->has_is_liked &&
    gnostr_timeline_item_view_model_get_is_liked(base) != patch->is_liked;
  changed |= patch->has_repost_count &&
    gnostr_timeline_item_view_model_get_repost_count(base) != patch->repost_count;
  changed |= patch->has_reply_count &&
    gnostr_timeline_item_view_model_get_reply_count(base) != patch->reply_count;
  changed |= patch->has_zap_count &&
    gnostr_timeline_item_view_model_get_zap_count(base) != patch->zap_count;
  changed |= patch->has_zap_total_msat &&
    gnostr_timeline_item_view_model_get_zap_total_msat(base) != patch->zap_total_msat;

  if (changed) {
    g_autoptr(GnostrTimelineItemViewModel) replacement =
      gnostr_timeline_item_view_model_copy_with_interactions(base,
                                                             patch->has_like_count,
                                                             patch->like_count,
                                                             patch->has_is_liked,
                                                             patch->is_liked,
                                                             patch->has_repost_count,
                                                             patch->repost_count,
                                                             patch->has_reply_count,
                                                             patch->reply_count,
                                                             patch->has_zap_count,
                                                             patch->zap_count,
                                                             patch->has_zap_total_msat,
                                                             patch->zap_total_msat);
    PatchAdmission admission = admission_for_replacement_vm(self, patch->event_id, replacement);
    if (deferred && admission == PATCH_ADMISSION_BLOCKED) {
      g_hash_table_replace(self->deferred_replacement_vms,
                           g_strdup(patch->event_id),
                           g_object_ref(replacement));
    } else {
      g_hash_table_remove(self->deferred_replacement_vms, patch->event_id);
      if (admission != PATCH_ADMISSION_BLOCKED)
        g_hash_table_remove(self->deferred_patch_events, patch->event_id);
      working_entry_replace_vm(entry, replacement);
    }
    return admission;
  }

  return PATCH_ADMISSION_NO_CHANGE;
}

static PatchAdmission
apply_profile_patch(GnostrTimelineFeedController *self,
                    const GnostrTimelineProfilePatch *patch)
{
  if (!patch || !patch->pubkey_hex)
    return PATCH_ADMISSION_NO_CHANGE;

  PatchAdmission admission = PATCH_ADMISSION_NO_CHANGE;
  for (guint i = 0; i < self->working->len; i++) {
    WorkingEntry *entry = g_ptr_array_index(self->working, i);
    if (!entry || !entry->vm)
      continue;

    const char *entry_event_id = gnostr_timeline_item_view_model_get_event_id(entry->vm);
    GnostrTimelineItemViewModel *deferred =
      g_hash_table_lookup(self->deferred_replacement_vms, entry_event_id);
    GnostrTimelineItemViewModel *base = deferred ? deferred : entry->vm;
    if (g_strcmp0(gnostr_timeline_item_view_model_get_pubkey(base), patch->pubkey_hex) != 0)
      continue;

    gboolean row_changed = FALSE;
    row_changed |= g_strcmp0(gnostr_timeline_item_view_model_get_display_name(base), patch->display_name) != 0;
    row_changed |= g_strcmp0(gnostr_timeline_item_view_model_get_handle(base), patch->handle) != 0;
    row_changed |= g_strcmp0(gnostr_timeline_item_view_model_get_avatar_url(base), patch->avatar_url) != 0;
    row_changed |= g_strcmp0(gnostr_timeline_item_view_model_get_nip05(base), patch->nip05) != 0;
    row_changed |= !gnostr_timeline_item_view_model_get_has_profile(base);

    if (row_changed) {
      g_autoptr(GnostrTimelineItemViewModel) replacement =
        gnostr_timeline_item_view_model_copy_with_profile(base,
                                                          patch->display_name,
                                                          patch->handle,
                                                          patch->avatar_url,
                                                          patch->nip05,
                                                          TRUE);
      PatchAdmission row_admission = admission_for_replacement_vm(self,
                                                                  entry_event_id,
                                                                  replacement);
      admission = patch_admission_combine(admission, row_admission);
      if (deferred && row_admission == PATCH_ADMISSION_BLOCKED) {
        g_hash_table_replace(self->deferred_replacement_vms,
                             g_strdup(entry_event_id),
                             g_object_ref(replacement));
      } else {
        g_hash_table_remove(self->deferred_replacement_vms, entry_event_id);
        if (row_admission != PATCH_ADMISSION_BLOCKED)
          g_hash_table_remove(self->deferred_patch_events, entry_event_id);
        working_entry_replace_vm(entry, replacement);
      }
    }
  }

  return admission;
}

static void
on_source_batch(GnostrTimelineSource *source G_GNUC_UNUSED,
                GnostrTimelineBatch *batch,
                gpointer user_data)
{
  gnostr_timeline_feed_controller_ingest_batch(GNOSTR_TIMELINE_FEED_CONTROLLER(user_data), batch);
}

static void
gnostr_timeline_feed_controller_dispose(GObject *object)
{
  GnostrTimelineFeedController *self = GNOSTR_TIMELINE_FEED_CONTROLLER(object);

  if (self->compose_source_id != 0) {
    g_source_remove(self->compose_source_id);
    self->compose_source_id = 0;
  }

  if (self->source && self->source_batch_handler != 0) {
    g_signal_handler_disconnect(self->source, self->source_batch_handler);
    self->source_batch_handler = 0;
  }

  g_clear_object(&self->source);
  g_clear_object(&self->hydrator);
  g_clear_object(&self->model);
  g_clear_pointer(&self->working, g_ptr_array_unref);
  g_clear_pointer(&self->by_event_id, g_hash_table_unref);
  g_clear_pointer(&self->pending_head, g_hash_table_unref);
  g_clear_pointer(&self->deferred_patch_events, g_hash_table_unref);
  g_clear_pointer(&self->deferred_replacement_vms, g_hash_table_unref);
  g_clear_pointer(&self->geometry, gnostr_timeline_geometry_resolver_free);

  G_OBJECT_CLASS(gnostr_timeline_feed_controller_parent_class)->dispose(object);
}

static void
gnostr_timeline_feed_controller_class_init(GnostrTimelineFeedControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_timeline_feed_controller_dispose;

  signals[SIGNAL_PENDING_COUNT_CHANGED] =
    g_signal_new("pending-count-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 g_cclosure_marshal_VOID__UINT,
                 G_TYPE_NONE,
                 1,
                 G_TYPE_UINT);

  signals[SIGNAL_RESTORE_SCROLL] =
    g_signal_new("restore-scroll",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE,
                 1,
                 G_TYPE_DOUBLE);

  signals[SIGNAL_SNAPSHOT_PUBLISHED] =
    g_signal_new("snapshot-published",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE,
                 1,
                 GNOSTR_TYPE_TIMELINE_SNAPSHOT);

  signals[SIGNAL_NEED_PROFILE] =
    g_signal_new("need-profile",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 g_cclosure_marshal_VOID__STRING,
                 G_TYPE_NONE,
                 1,
                 G_TYPE_STRING);
}

static void
gnostr_timeline_feed_controller_init(GnostrTimelineFeedController *self)
{
  self->query_generation = 1;
  self->snapshot_generation = 0;
  self->hydrator = gnostr_timeline_hydrator_new(self->query_generation);
  self->model = gnostr_timeline_snapshot_model_new();
  self->working = g_ptr_array_new_with_free_func((GDestroyNotify)working_entry_free);
  self->by_event_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->pending_head = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->deferred_patch_events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->deferred_replacement_vms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->geometry = gnostr_timeline_geometry_resolver_new();
  self->page_size = DEFAULT_VISIBLE_PAGE_SIZE;
  self->visible_limit = 0;
  self->user_at_top = TRUE;
  self->scroll_y = 0.0;
  self->viewport_height = 0.0;
  self->width_bucket = GNOSTR_TIMELINE_GEOMETRY_DEFAULT_WIDTH_BUCKET;
}

GnostrTimelineFeedController *
gnostr_timeline_feed_controller_new(GnostrTimelineSource *source)
{
  GnostrTimelineFeedController *self =
    g_object_new(GNOSTR_TYPE_TIMELINE_FEED_CONTROLLER, NULL);
  if (source)
    gnostr_timeline_feed_controller_set_source(self, source);
  return self;
}

GnostrTimelineSnapshotModel *
gnostr_timeline_feed_controller_get_model(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), NULL);
  return self->model;
}

GnostrTimelineSource *
gnostr_timeline_feed_controller_get_source(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), NULL);
  return self->source;
}

guint64
gnostr_timeline_feed_controller_get_query_generation(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), 0);
  return self->query_generation;
}

void
gnostr_timeline_feed_controller_set_source(GnostrTimelineFeedController *self,
                                           GnostrTimelineSource *source)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  g_return_if_fail(source == NULL || GNOSTR_IS_TIMELINE_SOURCE(source));
#endif

  if (self->source == source)
    return;

  if (self->source && self->source_batch_handler != 0) {
    g_signal_handler_disconnect(self->source, self->source_batch_handler);
    self->source_batch_handler = 0;
  }

  g_set_object(&self->source, source);
#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  if (self->source) {
    self->query_generation = gnostr_timeline_source_get_generation(self->source);
    if (self->hydrator)
      gnostr_timeline_hydrator_set_generation(self->hydrator, self->query_generation);
    self->source_batch_handler = g_signal_connect(self->source,
                                                   "batch",
                                                   G_CALLBACK(on_source_batch),
                                                   self);
  }
#endif
}

void
gnostr_timeline_feed_controller_set_query(GnostrTimelineFeedController *self,
                                          GNostrTimelineQuery *query)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  if (!self->source) {
    GnostrTimelineSource *source = gnostr_timeline_source_new();
    gnostr_timeline_feed_controller_set_source(self, source);
    g_object_unref(source);
  }

  gnostr_timeline_source_set_query(self->source, query);
  self->query_generation = gnostr_timeline_source_get_generation(self->source);
#else
  (void)query;
  self->query_generation++;
  if (self->query_generation == 0)
    self->query_generation = 1;
#endif
  if (self->hydrator)
    gnostr_timeline_hydrator_set_generation(self->hydrator, self->query_generation);
  self->loading_older = FALSE;
  self->loading_newer = FALSE;
  clear_working_set(self);
  emit_pending_count(self);
  compose_and_publish(self, FALSE, TRUE);
}

void
gnostr_timeline_feed_controller_refresh(GnostrTimelineFeedController *self)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  if (self->source)
    gnostr_timeline_source_refresh_async(self->source);
#endif
}

void
gnostr_timeline_feed_controller_load_older(GnostrTimelineFeedController *self,
                                           guint count)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
#ifdef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  (void)count;
  return;
#else
  if (!self->source || count == 0 || self->loading_older)
    return;

  self->loading_older = TRUE;

  gint64 before = 0;
  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_current_snapshot(self);
  if (snapshot && gnostr_timeline_snapshot_get_n_rows(snapshot) > 0) {
    GnostrTimelineSnapshotRow *last =
      gnostr_timeline_snapshot_get_row(snapshot,
                                       gnostr_timeline_snapshot_get_n_rows(snapshot) - 1);
    before = last ? gnostr_timeline_snapshot_row_get_created_at(last) : 0;
  }

  gnostr_timeline_source_load_older_async(self->source, count, before);
#endif
}

void
gnostr_timeline_feed_controller_load_newer(GnostrTimelineFeedController *self,
                                           guint count)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
#ifdef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  (void)count;
  return;
#else
  if (!self->source || count == 0 || self->loading_newer)
    return;

  self->loading_newer = TRUE;

  gint64 after = 0;
  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_current_snapshot(self);
  if (snapshot && gnostr_timeline_snapshot_get_n_rows(snapshot) > 0) {
    GnostrTimelineSnapshotRow *first = gnostr_timeline_snapshot_get_row(snapshot, 0);
    after = first ? gnostr_timeline_snapshot_row_get_created_at(first) : 0;
  }

  gnostr_timeline_source_load_newer_async(self->source, count, after);
#endif
}

void
gnostr_timeline_feed_controller_ingest_batch(GnostrTimelineFeedController *self,
                                             GnostrTimelineBatch *batch)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
  g_return_if_fail(GNOSTR_IS_TIMELINE_BATCH(batch));

  if (!batch_generation_is_current(self, batch))
    return;

  GnostrTimelineBatchKind kind = gnostr_timeline_batch_get_kind(batch);
  guint n_entries = gnostr_timeline_batch_get_n_entries(batch);

  switch (kind) {
    case GNOSTR_TIMELINE_BATCH_REFRESH: {
      g_autoptr(GPtrArray) items = hydrate_batch_items(self, batch);
      if (!items)
        return;
      clear_working_set(self);
      for (guint i = 0; i < items->len; i++)
        merge_hydrated_vm(self, g_ptr_array_index(items, i), NULL);
      set_initial_visible_limit(self);
      emit_pending_count(self);
      schedule_compose(self, FALSE, TRUE);
      break;
    }

    case GNOSTR_TIMELINE_BATCH_LIVE_HEAD: {
      g_autoptr(GPtrArray) items = hydrate_batch_items(self, batch);
      if (!items)
        return;
      gboolean pending_changed = FALSE;
      guint visible_increment = 0;
      PatchAdmission admission = PATCH_ADMISSION_NO_CHANGE;
      for (guint i = 0; i < items->len; i++) {
        GnostrTimelineItemViewModel *vm = g_ptr_array_index(items, i);
        gboolean inserted = FALSE;
        PatchAdmission item_admission = PATCH_ADMISSION_NO_CHANGE;
        WorkingEntry *we = merge_hydrated_vm_with_admission(self, vm, &inserted, &item_admission);
        admission = patch_admission_combine(admission, item_admission);
        const char *event_id = (we && we->vm) ? gnostr_timeline_item_view_model_get_event_id(we->vm) : NULL;
        if (!self->user_at_top && event_id && inserted && !g_hash_table_contains(self->pending_head, event_id)) {
          g_hash_table_add(self->pending_head, g_strdup(event_id));
          pending_changed = TRUE;
        } else if (self->user_at_top && inserted) {
          visible_increment++;
        }
      }

      if (self->user_at_top && self->scroll_y <= AT_TOP_EPSILON_PX) {
        expand_visible_limit(self, visible_increment);
        if (visible_increment > 0)
          schedule_compose(self, FALSE, TRUE);
        else if (admission == PATCH_ADMISSION_PUBLISH)
          schedule_compose(self, TRUE, FALSE);
        else if (admission == PATCH_ADMISSION_BLOCKED)
          g_debug("[COMPOSITOR] Deferring live replacement publication for visible geometry-unsafe rows");
      } else {
        if (pending_changed)
          emit_pending_count(self);
        if (admission == PATCH_ADMISSION_PUBLISH)
          schedule_compose(self, TRUE, FALSE);
        else if (admission == PATCH_ADMISSION_BLOCKED)
          g_debug("[COMPOSITOR] Deferring live replacement publication for visible geometry-unsafe rows");
      }
      break;
    }

    case GNOSTR_TIMELINE_BATCH_PAGE_OLDER: {
      g_autoptr(GPtrArray) items = hydrate_batch_items(self, batch);
      if (!items)
        return;
      self->loading_older = FALSE;
      PatchAdmission admission = PATCH_ADMISSION_NO_CHANGE;
      for (guint i = 0; i < items->len; i++) {
        PatchAdmission item_admission = PATCH_ADMISSION_NO_CHANGE;
        merge_hydrated_vm_with_admission(self, g_ptr_array_index(items, i), NULL, &item_admission);
        admission = patch_admission_combine(admission, item_admission);
      }
      expand_visible_limit(self, items->len);
      schedule_compose(self, TRUE, FALSE);
      if (admission == PATCH_ADMISSION_BLOCKED)
        g_debug("[COMPOSITOR] Deferring older-page replacement publication for visible geometry-unsafe rows");
      break;
    }

    case GNOSTR_TIMELINE_BATCH_PAGE_NEWER: {
      g_autoptr(GPtrArray) items = hydrate_batch_items(self, batch);
      if (!items)
        return;
      self->loading_newer = FALSE;
      PatchAdmission admission = PATCH_ADMISSION_NO_CHANGE;
      for (guint i = 0; i < items->len; i++) {
        PatchAdmission item_admission = PATCH_ADMISSION_NO_CHANGE;
        merge_hydrated_vm_with_admission(self, g_ptr_array_index(items, i), NULL, &item_admission);
        admission = patch_admission_combine(admission, item_admission);
      }
      expand_visible_limit(self, items->len);
      schedule_compose(self, TRUE, FALSE);
      if (admission == PATCH_ADMISSION_BLOCKED)
        g_debug("[COMPOSITOR] Deferring newer-page replacement publication for visible geometry-unsafe rows");
      break;
    }

    case GNOSTR_TIMELINE_BATCH_DELETE: {
      gboolean pending_changed = FALSE;
      gboolean visible_changed = FALSE;
      g_autoptr(GnostrTimelineSnapshot) current_snapshot = dup_current_snapshot(self);
      guint n_targets = gnostr_timeline_batch_get_n_delete_targets(batch);
      for (guint i = 0; i < n_targets; i++) {
        const GnostrTimelineDeleteTarget *target =
          gnostr_timeline_batch_get_delete_target(batch, i);
        if (target) {
          gboolean was_pending = FALSE;
          guint visible_index = 0;
          gboolean was_visible = current_snapshot && target->target_event_id &&
            gnostr_timeline_snapshot_lookup_event(current_snapshot, target->target_event_id, &visible_index);
          gboolean removed = remove_working_event(self,
                                                  target->target_event_id,
                                                  &pending_changed,
                                                  &was_pending);
          visible_changed |= removed && was_visible && !was_pending;
        }
      }

      if (pending_changed)
        emit_pending_count(self);
      clamp_visible_limit(self);
      if (visible_changed)
        schedule_compose(self, TRUE, FALSE);
      else if (n_targets == 0 && n_entries > 0)
        g_debug("[COMPOSITOR] Ignoring delete batch with no resolved NIP-09 targets (%u entries)",
                n_entries);
      break;
    }

    case GNOSTR_TIMELINE_BATCH_PROFILE_PATCH: {
      PatchAdmission admission = PATCH_ADMISSION_NO_CHANGE;
      guint n_patches = gnostr_timeline_batch_get_n_profile_patches(batch);
      for (guint i = 0; i < n_patches; i++) {
        const GnostrTimelineProfilePatch *patch =
          gnostr_timeline_batch_get_profile_patch(batch, i);
        admission = patch_admission_combine(admission, apply_profile_patch(self, patch));
      }
      if (admission == PATCH_ADMISSION_PUBLISH)
        schedule_compose(self, TRUE, FALSE);
      else if (admission == PATCH_ADMISSION_BLOCKED)
        g_debug("[COMPOSITOR] Deferring profile patch publication for visible geometry-unsafe rows");
      else if (n_patches == 0 && n_entries > 0)
        g_debug("[COMPOSITOR] Ignoring profile patch batch with no projected profile payload (%u entries)",
                n_entries);
      break;
    }

    case GNOSTR_TIMELINE_BATCH_METADATA_PATCH: {
      PatchAdmission admission = PATCH_ADMISSION_NO_CHANGE;
      guint n_patches = gnostr_timeline_batch_get_n_metadata_patches(batch);
      for (guint i = 0; i < n_patches; i++) {
        const GnostrTimelineMetadataPatch *patch =
          gnostr_timeline_batch_get_metadata_patch(batch, i);
        admission = patch_admission_combine(admission, apply_metadata_patch(self, patch));
      }
      if (admission == PATCH_ADMISSION_PUBLISH)
        schedule_compose(self, TRUE, FALSE);
      else if (admission == PATCH_ADMISSION_BLOCKED)
        g_debug("[COMPOSITOR] Deferring metadata patch publication for visible geometry-unsafe rows");
      else if (n_patches == 0 && n_entries > 0)
        g_debug("[COMPOSITOR] Ignoring metadata patch batch with no target-row payload (%u entries)",
                n_entries);
      break;
    }
  }

  emit_profile_requests(self, batch);
}

void
gnostr_timeline_feed_controller_set_viewport(GnostrTimelineFeedController *self,
                                             double scroll_y,
                                             double viewport_height,
                                             guint width_px)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  guint old_bucket = self->width_bucket;
  self->scroll_y = MAX(scroll_y, 0.0);
  self->viewport_height = MAX(viewport_height, 0.0);
  self->width_bucket = gnostr_timeline_geometry_width_to_bucket(width_px);
  self->user_at_top = self->scroll_y <= AT_TOP_EPSILON_PX;

  if (old_bucket != self->width_bucket)
    schedule_compose(self, TRUE, FALSE);
  else if (reevaluate_deferred_patch_events(self))
    schedule_compose(self, TRUE, FALSE);
}

void
gnostr_timeline_feed_controller_set_user_at_top(GnostrTimelineFeedController *self,
                                                gboolean at_top)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
  self->user_at_top = at_top;
}

gboolean
gnostr_timeline_feed_controller_get_user_at_top(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), FALSE);
  return self->user_at_top;
}

guint
gnostr_timeline_feed_controller_get_pending_count(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), 0);
  return pending_count(self);
}

void
gnostr_timeline_feed_controller_admit_pending_head(GnostrTimelineFeedController *self,
                                                   gboolean scroll_to_top)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  guint admitted = pending_count(self);
  if (admitted == 0)
    return;

  g_hash_table_remove_all(self->pending_head);
  expand_visible_limit(self, admitted);
  emit_pending_count(self);
  schedule_compose(self, !scroll_to_top, scroll_to_top);
}

void
gnostr_timeline_feed_controller_compose_now(GnostrTimelineFeedController *self)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  gboolean preserve_anchor = self->scheduled_preserve_anchor;
  gboolean scroll_to_top = self->scheduled_scroll_to_top;
  self->scheduled_preserve_anchor = FALSE;
  self->scheduled_scroll_to_top = FALSE;

  if (self->compose_source_id != 0) {
    g_source_remove(self->compose_source_id);
    self->compose_source_id = 0;
  }

  compose_and_publish(self, preserve_anchor, scroll_to_top);
}

char *
gnostr_timeline_feed_controller_dup_geometry_token_for_row(GnostrTimelineSnapshotRow *row)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(row), NULL);

  return gnostr_timeline_geometry_dup_cache_key(gnostr_timeline_snapshot_row_get_event_id(row),
                                                gnostr_timeline_snapshot_row_get_width_bucket(row),
                                                gnostr_timeline_snapshot_row_get_layout_signature(row));
}

void
gnostr_timeline_feed_controller_record_geometry(GnostrTimelineFeedController *self,
                                                const char *geometry_token,
                                                guint64 snapshot_generation,
                                                gint width_px,
                                                gint height_px)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  if (!geometry_token || !*geometry_token || height_px <= 0)
    return;

  g_autofree char *event_id = NULL;
  g_autofree char *layout_signature = NULL;
  guint token_width_bucket = 0;
  if (!gnostr_timeline_geometry_parse_cache_key(geometry_token,
                                                &event_id,
                                                &token_width_bucket,
                                                &layout_signature))
    return;

  guint actual_width_bucket = gnostr_timeline_geometry_width_to_bucket((guint)MAX(width_px, 0));
  if (actual_width_bucket != token_width_bucket) {
    g_debug("[COMPOSITOR] Ignoring geometry token width mismatch token=%u actual=%u",
            token_width_bucket,
            actual_width_bucket);
    return;
  }

  g_autoptr(GnostrTimelineSnapshot) current_snapshot = dup_current_snapshot(self);
  guint64 current_generation = current_snapshot ?
    gnostr_timeline_snapshot_get_generation(current_snapshot) : 0;
  if (snapshot_generation > 0 && current_generation > 0 && snapshot_generation < current_generation)
    return;

  gnostr_timeline_geometry_resolver_record_measurement(self->geometry,
                                                       event_id,
                                                       token_width_bucket,
                                                       layout_signature,
                                                       (double)height_px);

  /* Measurements refine future editions only.  Replacing the currently visible
   * snapshot in response to row measurement creates a feedback loop: GTK
   * measures, the compositor republishes, scroll anchoring restores, GTK
   * measures again.  That is visible jitter.  Cache this geometry for the next
   * intentional compose instead of mutating the active reading surface. */
}

void
gnostr_timeline_anchor_clear(GnostrTimelineAnchor *anchor)
{
  if (!anchor)
    return;
  g_clear_pointer(&anchor->event_id, g_free);
  anchor->index_hint = 0;
  anchor->offset_px_in_row = 0.0;
  anchor->snapshot_generation = 0;
}
