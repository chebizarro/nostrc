/* gnostr-timeline-metadata-controller.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * nostrc-hiei: App-owned metadata batching controller. Extracted from
 * gnostr-timeline-view-app-factory.c (schedule_metadata_batch /
 * metadata_batch_idle_cb / metadata_batch_query_thread /
 * on_metadata_batch_done) so that GNostr-specific batching state no
 * longer lives on NostrGtkTimelineView's private struct.
 *
 * The behaviour is intentionally preserved:
 * - single debounce idle source per main-loop iteration
 * - worker thread runs NDB batch queries off the main thread
 * - results are applied back to the source GObjects on the main thread
 * - main-thread-only inputs (current user pubkey) are captured before
 *   dispatching to the worker
 */

#include "gnostr-timeline-metadata-controller.h"

#include <string.h>

#include "../model/gn-nostr-event-item.h"
#include "gnostr-timeline-embed-private.h"
#include <nostr-gobject-1.0/storage_ndb.h>

struct _GnostrTimelineMetadataController {
  GObject parent_instance;
  GPtrArray *pending;           /* refs to GnNostrEventItem instances */
  GHashTable *pending_set;      /* pointer set for the current batch window,
                                 * used to coalesce duplicate schedule()
                                 * calls for the same item (does not hold
                                 * refs — entries are cleared whenever the
                                 * pending array is swapped or drained) */
  guint idle_id;
};

G_DEFINE_FINAL_TYPE(GnostrTimelineMetadataController,
                    gnostr_timeline_metadata_controller,
                    G_TYPE_OBJECT)

/* Data passed from the NDB worker thread back to the main thread. */
typedef struct {
  GHashTable *reaction_counts;  /* event_id -> GUINT count */
  GHashTable *user_reacted;     /* event_id -> TRUE */
  GHashTable *zap_stats;        /* event_id -> StorageNdbZapStats* */
  GHashTable *repost_counts;    /* event_id -> GUINT count */
  GHashTable *reply_counts;     /* hq-vvmzu: event_id -> GUINT count */
  GPtrArray  *items;            /* refs to the GObjects */
} MetadataBatchResult;

static void
metadata_batch_result_free(MetadataBatchResult *r) {
  if (!r) return;
  g_clear_pointer(&r->reaction_counts, g_hash_table_unref);
  g_clear_pointer(&r->user_reacted, g_hash_table_unref);
  g_clear_pointer(&r->zap_stats, g_hash_table_unref);
  g_clear_pointer(&r->repost_counts, g_hash_table_unref);
  g_clear_pointer(&r->reply_counts, g_hash_table_unref);
  g_clear_pointer(&r->items, g_ptr_array_unref);
  g_free(r);
}

/* Worker thread: runs NDB batch queries off the main thread. */
static void
metadata_batch_query_thread(GTask *task, gpointer source_object,
                            gpointer task_data G_GNUC_UNUSED,
                            GCancellable *cancellable G_GNUC_UNUSED) {
  (void)source_object;
  GPtrArray *items = g_object_get_data(G_OBJECT(task), "items");
  if (!items || items->len == 0) {
    g_task_return_pointer(task, NULL, NULL);
    return;
  }

  guint n = items->len;
  const char **event_ids = g_new0(const char *, n);
  gchar **owned_ids = g_new0(gchar *, n);
  guint id_count = 0;

  for (guint i = 0; i < n; i++) {
    GObject *obj = g_ptr_array_index(items, i);
    if (!GN_IS_NOSTR_EVENT_ITEM(obj)) continue;
    gchar *id_hex = NULL;
    g_object_get(obj, "event-id", &id_hex, NULL);
    if (id_hex && strlen(id_hex) == 64) {
      owned_ids[id_count] = id_hex;
      event_ids[id_count] = id_hex;
      id_count++;
    } else {
      g_free(id_hex);
    }
  }

  MetadataBatchResult *result = NULL;
  if (id_count > 0) {
    const gchar *user_pubkey = g_object_get_data(G_OBJECT(task), "user-pubkey");

    result = g_new0(MetadataBatchResult, 1);
    result->reaction_counts = storage_ndb_count_reactions_batch(event_ids, id_count);
    result->user_reacted = user_pubkey
      ? storage_ndb_user_has_reacted_batch(event_ids, id_count, user_pubkey)
      : NULL;
    result->zap_stats = storage_ndb_get_zap_stats_batch(event_ids, id_count);
    result->repost_counts = storage_ndb_count_reposts_batch(event_ids, id_count);
    result->reply_counts = storage_ndb_count_replies_batch(event_ids, id_count);
    result->items = g_ptr_array_ref(items);
  }

  for (guint i = 0; i < id_count; i++)
    g_free(owned_ids[i]);
  g_free(owned_ids);
  g_free(event_ids);

  g_task_return_pointer(task, result, (GDestroyNotify)metadata_batch_result_free);
}

/* Main thread: apply NDB query results to GObject items. */
static void
on_metadata_batch_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)user_data;
  if (!GNOSTR_IS_TIMELINE_METADATA_CONTROLLER(source)) return;

  MetadataBatchResult *r = g_task_propagate_pointer(G_TASK(res), NULL);
  if (!r || !r->items) {
    metadata_batch_result_free(r);
    return;
  }

  guint n = r->items->len;
  for (guint i = 0; i < n; i++) {
    GObject *obj = g_ptr_array_index(r->items, i);
    /* Type-guard on the cast: the schedule() API accepts only
     * GnNostrEventItem, but an item may have been disposed between
     * dispatch and callback. */
    if (!GN_IS_NOSTR_EVENT_ITEM(obj)) continue;
    GnNostrEventItem *ev = GN_NOSTR_EVENT_ITEM(obj);

    gchar *id_hex = NULL;
    g_object_get(obj, "event-id", &id_hex, NULL);
    if (!id_hex || strlen(id_hex) != 64) { g_free(id_hex); continue; }

    if (r->reaction_counts) {
      gpointer val = g_hash_table_lookup(r->reaction_counts, id_hex);
      if (val) {
        guint count = GPOINTER_TO_UINT(val);
        if (count > 0)
          gn_nostr_event_item_set_like_count(ev, count);
      }
    }

    if (r->user_reacted && g_hash_table_lookup(r->user_reacted, id_hex)) {
      gn_nostr_event_item_set_is_liked(ev, TRUE);
    }

    if (r->zap_stats) {
      StorageNdbZapStats *zs = g_hash_table_lookup(r->zap_stats, id_hex);
      if (zs && zs->zap_count > 0) {
        gn_nostr_event_item_set_zap_count(ev, zs->zap_count);
        gn_nostr_event_item_set_zap_total_msat(ev, zs->total_msat);
      }
    }

    if (r->repost_counts) {
      gpointer rval = g_hash_table_lookup(r->repost_counts, id_hex);
      if (rval) {
        guint rcount = GPOINTER_TO_UINT(rval);
        if (rcount > 0)
          gn_nostr_event_item_set_repost_count(ev, rcount);
      }
    }

    /* hq-vvmzu: Reply counts from ndb_note_meta */
    if (r->reply_counts) {
      gpointer rpval = g_hash_table_lookup(r->reply_counts, id_hex);
      if (rpval) {
        guint rpcount = GPOINTER_TO_UINT(rpval);
        if (rpcount > 0)
          gn_nostr_event_item_set_reply_count(ev, rpcount);
      }
    }

    g_free(id_hex);
  }

  g_debug("[BATCH] Completed async metadata load for %u events", n);
  metadata_batch_result_free(r);
}

/* Idle callback: drains the queue and dispatches one worker task.
 * Holds a strong ref on the controller via g_idle_add_full's data.
 */
static gboolean
metadata_batch_idle_cb(gpointer user_data) {
  GnostrTimelineMetadataController *self =
    GNOSTR_TIMELINE_METADATA_CONTROLLER(user_data);

  self->idle_id = 0;

  if (!self->pending || self->pending->len == 0)
    return G_SOURCE_REMOVE;

  g_debug("[BATCH] Dispatching %u metadata items to worker thread",
          self->pending->len);

  /* Move items to the task — the worker thread owns them now. Reset
   * the dedup set so the next batch window starts fresh. */
  GPtrArray *items = self->pending;
  self->pending = g_ptr_array_new_with_free_func(g_object_unref);
  g_hash_table_remove_all(self->pending_set);

  /* Capture main-thread-only inputs before dispatching to worker. */
  gchar *user_pubkey = gnostr_timeline_embed_get_current_user_pubkey_hex();

  GTask *task = g_task_new(self, NULL, on_metadata_batch_done, NULL);
  g_object_set_data_full(G_OBJECT(task), "items", items,
                         (GDestroyNotify)g_ptr_array_unref);
  g_object_set_data_full(G_OBJECT(task), "user-pubkey", user_pubkey, g_free);
  g_task_run_in_thread(task, metadata_batch_query_thread);
  g_object_unref(task);

  return G_SOURCE_REMOVE;
}

/* ---------------- Public API ---------------- */

GnostrTimelineMetadataController *
gnostr_timeline_metadata_controller_new(void) {
  return g_object_new(GNOSTR_TYPE_TIMELINE_METADATA_CONTROLLER, NULL);
}

void
gnostr_timeline_metadata_controller_schedule(
    GnostrTimelineMetadataController *self,
    GnNostrEventItem *item) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_METADATA_CONTROLLER(self));
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(item));
  /* pending and pending_set are allocated in _init() and freed in
   * dispose(); they are always both non-NULL while the controller is
   * alive (shutdown() empties them but never frees). */
  g_assert(self->pending && self->pending_set);

  /* Dedupe by pointer identity within the current batch window. The
   * pending_set holds no refs — membership is valid only until the
   * next swap/drain inside metadata_batch_idle_cb. */
  if (g_hash_table_contains(self->pending_set, item))
    return;

  g_hash_table_add(self->pending_set, item);
  g_ptr_array_add(self->pending, g_object_ref(item));

  if (self->idle_id == 0) {
    /* nostrc-x52i: Use _full variant with g_object_ref/unref to prevent
     * UAF if the controller (or its owner) is destroyed while the idle
     * callback is pending. Shutdown() removes the source explicitly so
     * the destroy notify runs promptly on owner disposal. */
    self->idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                     metadata_batch_idle_cb,
                                     g_object_ref(self),
                                     g_object_unref);
  }
}

void
gnostr_timeline_metadata_controller_shutdown(
    GnostrTimelineMetadataController *self) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_METADATA_CONTROLLER(self));

  if (self->idle_id) {
    /* Removing the source drops its ref on self; if that was the last
     * external ref, dispose runs after this function returns. */
    g_source_remove(self->idle_id);
    self->idle_id = 0;
  }
  /* Empty the queue and dedup set without destroying the containers so
   * schedule() after shutdown() still works (primarily useful in tests
   * and in the owner-disposed + in-flight-task recovery path). Both
   * containers live from _init() through dispose(); only dispose()
   * frees them. */
  if (self->pending)
    g_ptr_array_set_size(self->pending, 0);
  if (self->pending_set)
    g_hash_table_remove_all(self->pending_set);
}

/* ---------------- qdata ensure() ---------------- */

#define GNOSTR_TIMELINE_METADATA_CONTROLLER_QDATA \
  "gnostr-timeline-metadata-controller"

static void
attached_controller_destroy_notify(gpointer p) {
  GnostrTimelineMetadataController *ctrl = p;
  if (!ctrl) return;
  /* Cancel pending work and drop queued items before dropping our ref.
   * Any in-flight worker task retains the items it was handed and runs
   * to completion independently of the owner's lifetime. */
  gnostr_timeline_metadata_controller_shutdown(ctrl);
  g_object_unref(ctrl);
}

GnostrTimelineMetadataController *
gnostr_timeline_metadata_controller_ensure(GObject *owner) {
  g_return_val_if_fail(G_IS_OBJECT(owner), NULL);

  GnostrTimelineMetadataController *ctrl =
    g_object_get_data(owner, GNOSTR_TIMELINE_METADATA_CONTROLLER_QDATA);

  if (!ctrl) {
    ctrl = gnostr_timeline_metadata_controller_new();
    g_object_set_data_full(owner,
                            GNOSTR_TIMELINE_METADATA_CONTROLLER_QDATA,
                            ctrl,
                            attached_controller_destroy_notify);
  }
  return ctrl;
}

/* ---------------- GObject plumbing ---------------- */

static void
gnostr_timeline_metadata_controller_dispose(GObject *obj) {
  GnostrTimelineMetadataController *self =
    GNOSTR_TIMELINE_METADATA_CONTROLLER(obj);
  /* Belt-and-braces: shutdown() is normally called by the qdata destroy
   * notify before the final unref, but also handle direct unref paths. */
  gnostr_timeline_metadata_controller_shutdown(self);
  g_clear_pointer(&self->pending, g_ptr_array_unref);
  g_clear_pointer(&self->pending_set, g_hash_table_unref);
  G_OBJECT_CLASS(gnostr_timeline_metadata_controller_parent_class)->dispose(obj);
}

static void
gnostr_timeline_metadata_controller_class_init(
    GnostrTimelineMetadataControllerClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gnostr_timeline_metadata_controller_dispose;
}

static void
gnostr_timeline_metadata_controller_init(
    GnostrTimelineMetadataController *self) {
  /* Allocate both containers eagerly so the schedule() hot path never
   * has to test-and-allocate and the in-sync invariant between them is
   * trivially upheld for the entire controller lifetime. */
  self->pending = g_ptr_array_new_with_free_func(g_object_unref);
  self->pending_set = g_hash_table_new(g_direct_hash, g_direct_equal);
  self->idle_id = 0;
}
