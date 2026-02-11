#define G_LOG_DOMAIN "gn-ndb-dispatcher"

#include "gn-ndb-sub-dispatcher.h"
#include "../storage_ndb.h"

#include <glib.h>
#include <stdint.h>
#include <stdio.h>

#define GN_NDB_DISPATCH_BATCH_CAP 256
/* nostrc-mzab: Max note keys to process per main-loop iteration.
 * Prevents unbounded main-thread blocking when many events arrive at once.
 * After processing this many, we reschedule to let GTK render a frame. */
#define GN_NDB_DISPATCH_MAX_PER_TICK 64

/* nostrc-sbqe: Max entries in the per-subid dedup set before resetting.
 * When connected to 5+ relays, the same event arrives from multiple relays
 * and NDB queues duplicate note keys per subscription. Filtering at the
 * dispatcher level (before invoking handler callbacks) avoids 30-50% of
 * wasted dispatch work. The model-level dedup in on_sub_timeline_batch
 * remains as a safety net. */
#define GN_NDB_DEDUP_SET_CAP 4096

typedef struct {
  GnNdbSubBatchFn cb;
  gpointer user_data;
  GDestroyNotify destroy;
  GHashTable *recent_keys; /* nostrc-sbqe: per-subid dedup set (main thread only) */
} GnNdbHandler;

typedef struct {
  GMutex lock;
  GHashTable *handlers; /* key: uint64_t* subid, value: GnNdbHandler* */
  GHashTable *pending;  /* key: uint64_t* subid, value: GINT_TO_POINTER(1) */
  GMainContext *ui_context; /* captured on UI thread at init */
} GnNdbDispatcher;

typedef struct {
  uint64_t subid;
} DispatchCtx;

static GnNdbDispatcher s_disp;
static gsize s_inited = 0;

static guint u64_hash(gconstpointer key) {
  const uint64_t k = *(const uint64_t *)key;
  return (guint)(k ^ (k >> 32));
}

static gboolean u64_equal(gconstpointer a, gconstpointer b) {
  return *(const uint64_t *)a == *(const uint64_t *)b;
}

static void handler_free(gpointer p) {
  GnNdbHandler *h = (GnNdbHandler *)p;
  if (!h) return;
  if (h->recent_keys) {
    g_hash_table_destroy(h->recent_keys);
    h->recent_keys = NULL;
  }
  if (h->destroy) {
    h->destroy(h->user_data);
    h->destroy = NULL;
  }
  g_free(h);
}

static gboolean dispatch_subid_on_main(gpointer data);

static void on_ndb_notify_from_writer(void *ctx, uint64_t subid) {
  GnNdbDispatcher *disp = (GnNdbDispatcher *)ctx;
  if (!disp || subid == 0) return;

  gboolean should_schedule = FALSE;

  g_mutex_lock(&disp->lock);
  if (disp->pending) {
    if (!g_hash_table_contains(disp->pending, &subid)) {
      uint64_t *k = g_new(uint64_t, 1);
      *k = subid;
      g_hash_table_insert(disp->pending, k, GINT_TO_POINTER(1));
      should_schedule = TRUE;
    }
  }
  g_mutex_unlock(&disp->lock);

  if (should_schedule) {
    DispatchCtx *dctx = g_new0(DispatchCtx, 1);
    dctx->subid = subid;

    /* Use explicit GSource pattern recommended by GNOME for cross-thread dispatch.
     * g_idle_source_new() + g_source_attach() is more reliable than
     * g_main_context_invoke_full() when called from a foreign pthread. */
    GSource *source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(source, dispatch_subid_on_main, dctx, g_free);
    g_source_attach(source, disp->ui_context);
    g_source_unref(source);
  }
}

static void gn_ndb_dispatcher_init_once(void) {
  if (s_disp.handlers) return;

  g_mutex_init(&s_disp.lock);
  s_disp.handlers = g_hash_table_new_full(u64_hash, u64_equal, g_free, handler_free);
  s_disp.pending = g_hash_table_new_full(u64_hash, u64_equal, g_free, NULL);

  /* Capture the global default main context (the one GTK uses).
   * We use g_main_context_default() + ref rather than g_main_context_ref_thread_default()
   * because this must be the same context GTK is running, regardless of which thread
   * calls dispatcher_init. */
  s_disp.ui_context = g_main_context_ref(g_main_context_default());

  storage_ndb_set_notify_callback(on_ndb_notify_from_writer, &s_disp);

  g_debug("initialized; ui_context=%p, installed storage_ndb notify callback",
          (void *)s_disp.ui_context);
}

static GnNdbDispatcher *gn_ndb_dispatcher_get(void) {
  if (g_once_init_enter(&s_inited)) {
    gn_ndb_dispatcher_init_once();
    g_once_init_leave(&s_inited, 1);
  }
  return &s_disp;
}

void gn_ndb_dispatcher_init(void) {
  (void)gn_ndb_dispatcher_get();
}

static void clear_pending(GnNdbDispatcher *disp, uint64_t subid) {
  if (!disp || subid == 0) return;
  g_mutex_lock(&disp->lock);
  if (disp->pending) {
    g_hash_table_remove(disp->pending, &subid);
  }
  g_mutex_unlock(&disp->lock);
}

static GnNdbHandler *lookup_handler_locked(GnNdbDispatcher *disp, uint64_t subid) {
  if (!disp || !disp->handlers || subid == 0) return NULL;
  return (GnNdbHandler *)g_hash_table_lookup(disp->handlers, &subid);
}

static gboolean dispatch_subid_on_main(gpointer data) {
  DispatchCtx *dctx = (DispatchCtx *)data;
  uint64_t subid = dctx ? dctx->subid : 0;

  /* Invalidate cached transaction to ensure we see newly committed notes */
  storage_ndb_invalidate_txn_cache();

  GnNdbDispatcher *disp = gn_ndb_dispatcher_get();
  if (!disp || subid == 0) return G_SOURCE_REMOVE;

  /* nostrc-mzab: Process a bounded number of events per main-loop tick.
   * Instead of draining everything in an unbounded while loop (which blocks
   * GTK rendering), process at most GN_NDB_DISPATCH_MAX_PER_TICK keys then
   * yield back to the main loop. Return G_SOURCE_CONTINUE if more remain. */
  uint64_t keys[GN_NDB_DISPATCH_BATCH_CAP];
  int total_polled = 0;

  while (total_polled < GN_NDB_DISPATCH_MAX_PER_TICK) {
    /* nostrc-x3on: Check handler BEFORE polling. If the subscription was
     * unsubscribed (e.g., thread view navigated away), stop immediately.
     * Without this, the loop keeps polling stale nostrdb queue entries
     * every main-loop tick, starving GTK of CPU until the queue drains. */
    GnNdbHandler *h = NULL;
    g_mutex_lock(&disp->lock);
    h = lookup_handler_locked(disp, subid);
    g_mutex_unlock(&disp->lock);

    if (!h || !h->cb) {
      g_debug("dispatch: handler gone for subid=%" G_GUINT64_FORMAT ", stopping", (guint64)subid);
      break;
    }

    int cap = MIN(GN_NDB_DISPATCH_BATCH_CAP,
                  GN_NDB_DISPATCH_MAX_PER_TICK - total_polled);
    int n = storage_ndb_poll_notes(subid, keys, cap);
    if (n <= 0) break;
    total_polled += n;

    /* nostrc-sbqe: Filter duplicate note keys per-subid before invoking
     * the handler callback. When connected to 5+ relays, the same event
     * arrives from multiple relays and NDB queues duplicate keys. Filtering
     * here avoids 30-50% of wasted handler invocations. */
    if (!h->recent_keys) {
      h->recent_keys = g_hash_table_new_full(u64_hash, u64_equal, g_free, NULL);
    }

    /* Reset the dedup set when it gets too large. The model-level dedup
     * (e.g., hash set checks in on_sub_timeline_batch) remains as a
     * safety net, so a simple clear is safe. */
    if (g_hash_table_size(h->recent_keys) > GN_NDB_DEDUP_SET_CAP) {
      g_hash_table_remove_all(h->recent_keys);
      g_debug("dispatch: dedup set reset for subid=%" G_GUINT64_FORMAT
              " (exceeded %d cap)", (guint64)subid, GN_NDB_DEDUP_SET_CAP);
    }

    /* Compact the keys array in-place, skipping duplicates */
    int unique_count = 0;
    for (int i = 0; i < n; i++) {
      if (!g_hash_table_contains(h->recent_keys, &keys[i])) {
        uint64_t *dk = g_new(uint64_t, 1);
        *dk = keys[i];
        g_hash_table_insert(h->recent_keys, dk, GINT_TO_POINTER(1));
        keys[unique_count++] = keys[i];
      }
    }

    if (unique_count > 0) {
      h->cb(subid, keys, (guint)unique_count, h->user_data);
    }
  }

  if (total_polled >= GN_NDB_DISPATCH_MAX_PER_TICK) {
    /* More events likely remain — keep the idle source alive so we
     * continue draining in the next main-loop iteration.  Also keep
     * the pending flag clear so new writer notifications don't stack
     * duplicate sources while we're already draining. */
    return G_SOURCE_CONTINUE;
  }

  /* Fully drained (or handler gone) — allow future notifications to schedule again. */
  clear_pending(disp, subid);
  return G_SOURCE_REMOVE;
}

uint64_t gn_ndb_subscribe(
  const char *filter_json,
  GnNdbSubBatchFn cb,
  gpointer user_data,
  GDestroyNotify destroy
) {
  g_return_val_if_fail(filter_json != NULL, 0);
  g_return_val_if_fail(cb != NULL, 0);

  GnNdbDispatcher *disp = gn_ndb_dispatcher_get();
  g_return_val_if_fail(disp != NULL, 0);

  uint64_t subid = storage_ndb_subscribe(filter_json);
  if (subid == 0) {
    g_warning("subscribe failed (filter=%s)", filter_json);
    return 0;
  }

  GnNdbHandler *h = g_new0(GnNdbHandler, 1);
  h->cb = cb;
  h->user_data = user_data;
  h->destroy = destroy;

  uint64_t *k = g_new(uint64_t, 1);
  *k = subid;

  g_mutex_lock(&disp->lock);
  if (disp->handlers) {
    g_hash_table_replace(disp->handlers, k, h);
  } else {
    g_mutex_unlock(&disp->lock);
    g_free(k);
    handler_free(h);
    storage_ndb_unsubscribe(subid);
    return 0;
  }
  g_mutex_unlock(&disp->lock);

  g_debug("subscribed: subid=%" G_GUINT64_FORMAT " filter=%s", (guint64)subid, filter_json);
  return subid;
}

void gn_ndb_unsubscribe(uint64_t subid) {
  if (subid == 0) return;

  GnNdbDispatcher *disp = gn_ndb_dispatcher_get();
  if (!disp) return;

  g_mutex_lock(&disp->lock);
  if (disp->pending) {
    g_hash_table_remove(disp->pending, &subid);
  }
  if (disp->handlers) {
    g_hash_table_remove(disp->handlers, &subid);
    /* handler_free (value destroy) runs here and will call GDestroyNotify. */
  }
  g_mutex_unlock(&disp->lock);

  storage_ndb_unsubscribe(subid);

  g_debug("unsubscribed: subid=%" G_GUINT64_FORMAT, (guint64)subid);
}