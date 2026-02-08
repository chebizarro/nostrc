/**
 * @file neg-client.c
 * @brief NIP-77 Negentropy sync client implementation
 *
 * Implements range-based sync using the negentropy protocol:
 * 1. Builds a kind-filtered datasource from local NostrDB
 * 2. Creates a negentropy session and computes initial fingerprint
 * 3. Opens a relay connection and runs NEG-OPEN/NEG-MSG exchange
 * 4. Reports whether local and remote event sets are in sync
 *
 * V1 limitation: One concurrent sync session (global handler state).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "neg-client.h"
#include "../storage_ndb.h"
#include <nostr/nip77/negentropy.h>
#include <nostr-relay.h>
#include <nostr-json.h>
#include "nostr_json.h"
#include <channel.h>
#include <string.h>
#include <stdlib.h>

#define NEG_SYNC_TIMEOUT_SECS 30

GQuark
gnostr_neg_error_quark(void)
{
  return g_quark_from_static_string("gnostr-neg-error");
}

/* ============================================================================
 * Kind-Filtered NDB Datasource
 *
 * Materializes (created_at, event_id) pairs from NostrDB filtered by event
 * kind, sorted for the negentropy protocol.
 * ============================================================================ */

typedef struct {
  NostrIndexItem *items;
  size_t count;
  size_t cursor;
} KindFilteredDS;

static int kf_begin(void *ctx)
{
  ((KindFilteredDS *)ctx)->cursor = 0;
  return 0;
}

static int kf_next(void *ctx, NostrIndexItem *out)
{
  KindFilteredDS *ds = ctx;
  if (ds->cursor >= ds->count) return -1;
  *out = ds->items[ds->cursor++];
  return 0;
}

static void kf_end(void *ctx) { (void)ctx; }

static int
cmp_index_item(const void *a, const void *b)
{
  const NostrIndexItem *ia = a, *ib = b;
  if (ia->created_at != ib->created_at)
    return ia->created_at < ib->created_at ? -1 : 1;
  return memcmp(ia->id.bytes, ib->id.bytes, 32);
}

static gboolean
hex_to_bytes(const char *hex, unsigned char *out, size_t n)
{
  if (!hex || strlen(hex) != n * 2) return FALSE;
  for (size_t i = 0; i < n; i++) {
    int hi = g_ascii_xdigit_value(hex[2 * i]);
    int lo = g_ascii_xdigit_value(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return FALSE;
    out[i] = (unsigned char)((hi << 4) | lo);
  }
  return TRUE;
}

/**
 * Build sorted (created_at, id) datasource from local NDB events of
 * specified kinds. This is the "local state fingerprint" that the
 * negentropy protocol will compare against the relay's event set.
 */
static gboolean
build_kind_datasource(const int *kinds, size_t kind_count,
                      NostrNegDataSource *ds_out,
                      KindFilteredDS *ctx_out,
                      guint *local_count)
{
  GString *filt = g_string_new("[{\"kinds\":[");
  for (size_t i = 0; i < kind_count; i++) {
    if (i > 0) g_string_append_c(filt, ',');
    g_string_append_printf(filt, "%d", kinds[i]);
  }
  g_string_append(filt, "]}]");

  void *txn = NULL;
  int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
  if (rc != 0 || !txn) {
    g_string_free(filt, TRUE);
    return FALSE;
  }

  char **results = NULL;
  int count = 0;
  rc = storage_ndb_query(txn, filt->str, &results, &count);
  g_string_free(filt, TRUE);

  if (rc != 0) {
    storage_ndb_end_query(txn);
    return FALSE;
  }

  /* Parse events to NostrIndexItem array */
  ctx_out->items = count > 0 ? g_new0(NostrIndexItem, count) : NULL;
  ctx_out->count = 0;
  ctx_out->cursor = 0;

  for (int i = 0; i < count; i++) {
    g_autofree char *id_hex = NULL;
    g_autofree char *ca_str = NULL;

    if ((id_hex = gnostr_json_get_string(results[i], "id", NULL)) == NULL)
      continue;
    if ((ca_str = gnostr_json_get_raw(results[i], "created_at", NULL)) == NULL)
      continue;

    NostrIndexItem *item = &ctx_out->items[ctx_out->count];
    item->created_at = (uint64_t)g_ascii_strtoull(ca_str, NULL, 10);
    if (hex_to_bytes(id_hex, item->id.bytes, 32))
      ctx_out->count++;
  }

  if (results) storage_ndb_free_results(results, count);
  storage_ndb_end_query(txn);

  /* Sort by (created_at, id) for negentropy */
  if (ctx_out->count > 1)
    qsort(ctx_out->items, ctx_out->count, sizeof(NostrIndexItem), cmp_index_item);

  *local_count = (guint)ctx_out->count;
  ds_out->ctx = ctx_out;
  ds_out->begin_iter = kf_begin;
  ds_out->next = kf_next;
  ds_out->end_iter = kf_end;
  return TRUE;
}

/* ============================================================================
 * NEG-MSG Receive Channel
 *
 * Global state for receiving NEG-MSG/NEG-ERR from the relay's custom_handler.
 * V1 limitation: one sync session at a time.
 * ============================================================================ */

static GMutex g_neg_mu;
static GCond  g_neg_cond;
static gchar *g_neg_hex;
static gchar *g_neg_err_reason;
static gboolean g_neg_got_msg;
static gboolean g_neg_got_err;
static gchar *g_neg_sub_id;

static bool
neg_handler(const char *raw)
{
  if (!raw) return false;

  /* Fast reject: look for "NEG-" after first quote */
  const char *q = strchr(raw, '"');
  if (!q || strncmp(q + 1, "NEG-", 4) != 0) return false;

  char *type = NULL;
  if ((type = gnostr_json_get_array_string(raw, NULL, 0, NULL)) == NULL)
    return false;

  gboolean is_msg = (strcmp(type, "NEG-MSG") == 0);
  gboolean is_err = (strcmp(type, "NEG-ERR") == 0);
  free(type);
  if (!is_msg && !is_err) return false;

  char *sub = NULL;
  sub = gnostr_json_get_array_string(raw, NULL, 1, NULL);

  g_mutex_lock(&g_neg_mu);
  gboolean match = (sub && g_neg_sub_id && strcmp(sub, g_neg_sub_id) == 0);
  if (match) {
    char *val = NULL;
    val = gnostr_json_get_array_string(raw, NULL, 2, NULL);
    if (is_msg) {
      g_free(g_neg_hex);
      g_neg_hex = val ? g_strdup(val) : NULL;
      g_neg_got_msg = TRUE;
    } else {
      g_free(g_neg_err_reason);
      g_neg_err_reason = val ? g_strdup(val) : g_strdup("unknown");
      g_neg_got_err = TRUE;
    }
    if (val) free(val);
    g_cond_signal(&g_neg_cond);
  }
  g_mutex_unlock(&g_neg_mu);

  free(sub);
  return match;
}

static void
reset_neg_state(const char *sub_id)
{
  g_mutex_lock(&g_neg_mu);
  g_clear_pointer(&g_neg_hex, g_free);
  g_clear_pointer(&g_neg_err_reason, g_free);
  g_free(g_neg_sub_id);
  g_neg_sub_id = g_strdup(sub_id);
  g_neg_got_msg = FALSE;
  g_neg_got_err = FALSE;
  g_mutex_unlock(&g_neg_mu);
}

static void
cleanup_neg_state(void)
{
  g_mutex_lock(&g_neg_mu);
  g_clear_pointer(&g_neg_hex, g_free);
  g_clear_pointer(&g_neg_err_reason, g_free);
  g_clear_pointer(&g_neg_sub_id, g_free);
  g_neg_got_msg = FALSE;
  g_neg_got_err = FALSE;
  g_mutex_unlock(&g_neg_mu);
}

/**
 * Wait for NEG-MSG or NEG-ERR response with timeout.
 * Returns hex payload on success, sets error on failure.
 */
static gboolean
wait_neg_response(gchar **hex_out, gint64 deadline_us, GError **error)
{
  g_mutex_lock(&g_neg_mu);
  while (!g_neg_got_msg && !g_neg_got_err) {
    if (!g_cond_wait_until(&g_neg_cond, &g_neg_mu, deadline_us)) {
      g_mutex_unlock(&g_neg_mu);
      g_set_error_literal(error, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_TIMEOUT,
                          "Relay did not respond (NIP-77 may not be supported)");
      return FALSE;
    }
  }

  if (g_neg_got_err) {
    g_autofree gchar *reason = g_steal_pointer(&g_neg_err_reason);
    g_neg_got_err = FALSE;
    g_mutex_unlock(&g_neg_mu);
    g_set_error(error, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_UNSUPPORTED,
                "Relay sent NEG-ERR: %s", reason);
    return FALSE;
  }

  *hex_out = g_steal_pointer(&g_neg_hex);
  g_neg_got_msg = FALSE;
  g_mutex_unlock(&g_neg_mu);
  return TRUE;
}

/* ============================================================================
 * GTask Implementation
 * ============================================================================ */

typedef struct {
  gchar *relay_url;
  int   *kinds;
  size_t kind_count;
} SyncTaskData;

static void
task_data_free(gpointer p)
{
  SyncTaskData *d = p;
  g_free(d->relay_url);
  g_free(d->kinds);
  g_free(d);
}

static void
sync_task(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  (void)src;
  SyncTaskData *td = data;
  GnostrNegSyncStats *stats = g_new0(GnostrNegSyncStats, 1);

  /* === Phase 1: Build local fingerprint datasource === */
  KindFilteredDS ds_ctx = {0};
  NostrNegDataSource ds = {0};
  if (!build_kind_datasource(td->kinds, td->kind_count, &ds, &ds_ctx,
                             &stats->local_count)) {
    g_task_return_new_error(task, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_LOCAL,
                            "Failed to query local event index");
    g_free(stats);
    return;
  }

  g_debug("[NEG] Local index: %u events for %zu kind(s)",
          stats->local_count, td->kind_count);

  /* === Phase 2: Create negentropy session === */
  NostrNegOptions opts = {
    .max_ranges = 8,
    .max_idlist_items = 256,
    .max_round_trips = 8
  };
  NostrNegSession *neg = nostr_neg_session_new(&ds, &opts);
  if (!neg) {
    g_free(ds_ctx.items);
    g_task_return_new_error(task, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_LOCAL,
                            "Failed to create negentropy session");
    g_free(stats);
    return;
  }

  char *initial_hex = nostr_neg_build_initial_hex(neg);
  if (!initial_hex) {
    nostr_neg_session_free(neg);
    g_free(ds_ctx.items);
    g_task_return_new_error(task, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_LOCAL,
                            "Failed to build initial fingerprint");
    g_free(stats);
    return;
  }

  /* === Phase 3: Connect to relay === */
  Error *relay_err = NULL;
  NostrRelay *relay = nostr_relay_new(NULL, td->relay_url, &relay_err);
  if (!relay) {
    g_task_return_new_error(task, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_CONNECTION,
                            "Failed to create relay: %s",
                            relay_err ? relay_err->message : "unknown");
    if (relay_err) free(relay_err);
    free(initial_hex);
    nostr_neg_session_free(neg);
    g_free(ds_ctx.items);
    g_free(stats);
    return;
  }

  nostr_relay_set_auto_reconnect(relay, false);

  /* Set up NEG-MSG handler and subscription ID */
  gchar *sub_id = g_strdup_printf("neg-%04x", g_random_int_range(0, 0xFFFF));
  reset_neg_state(sub_id);
  nostr_relay_set_custom_handler(relay, neg_handler);

  if (!nostr_relay_connect(relay, &relay_err)) {
    g_task_return_new_error(task, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_CONNECTION,
                            "Relay connect failed: %s",
                            relay_err ? relay_err->message : "unknown");
    if (relay_err) free(relay_err);
    goto cleanup;
  }

  /* Wait for WebSocket handshake (up to 5s) */
  for (int i = 0; i < 50 && !nostr_relay_is_established(relay); i++) {
    if (g_cancellable_is_cancelled(cancel)) break;
    g_usleep(100000);
  }
  if (!nostr_relay_is_established(relay)) {
    g_task_return_new_error(task, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_CONNECTION,
                            "WebSocket handshake timed out");
    goto cleanup;
  }

  /* === Phase 4: NEG-OPEN === */
  {
    GString *filt_json = g_string_new("{\"kinds\":[");
    for (size_t i = 0; i < td->kind_count; i++) {
      if (i > 0) g_string_append_c(filt_json, ',');
      g_string_append_printf(filt_json, "%d", td->kinds[i]);
    }
    g_string_append(filt_json, "]}");

    gchar *neg_open = g_strdup_printf("[\"NEG-OPEN\",\"%s\",%s,\"%s\"]",
                                       sub_id, filt_json->str, initial_hex);
    g_string_free(filt_json, TRUE);
    free(initial_hex);
    initial_hex = NULL;

    /* nostr_relay_write takes ownership of a malloc'd string */
    GoChannel *wch = nostr_relay_write(relay, strdup(neg_open));
    g_free(neg_open);
    if (wch) go_channel_free(wch);
  }

  /* === Phase 5: Protocol loop === */
  {
    gint64 deadline = g_get_monotonic_time() + NEG_SYNC_TIMEOUT_SECS * G_USEC_PER_SEC;
    GError *proto_err = NULL;

    while (!g_cancellable_is_cancelled(cancel)) {
      gchar *response_hex = NULL;
      if (!wait_neg_response(&response_hex, deadline, &proto_err))
        break;

      int rc = nostr_neg_handle_peer_hex(neg, response_hex);
      g_free(response_hex);
      if (rc != 0) {
        g_set_error_literal(&proto_err, GNOSTR_NEG_ERROR,
                            GNOSTR_NEG_ERROR_PROTOCOL,
                            "Failed to process negentropy message");
        break;
      }

      char *next_hex = nostr_neg_build_next_hex(neg);
      if (!next_hex || next_hex[0] == '\0') {
        free(next_hex);
        break; /* Protocol complete */
      }

      /* Send NEG-MSG */
      gchar *neg_msg = g_strdup_printf("[\"NEG-MSG\",\"%s\",\"%s\"]",
                                        sub_id, next_hex);
      free(next_hex);
      GoChannel *wch = nostr_relay_write(relay, strdup(neg_msg));
      g_free(neg_msg);
      if (wch) go_channel_free(wch);
    }

    /* Send NEG-CLOSE regardless of outcome */
    {
      gchar *neg_close = g_strdup_printf("[\"NEG-CLOSE\",\"%s\"]", sub_id);
      GoChannel *wch = nostr_relay_write(relay, strdup(neg_close));
      g_free(neg_close);
      if (wch) go_channel_free(wch);
    }

    if (proto_err) {
      g_task_return_error(task, proto_err);
      goto cleanup;
    }
    if (g_cancellable_is_cancelled(cancel)) {
      g_task_return_new_error(task, GNOSTR_NEG_ERROR,
                              GNOSTR_NEG_ERROR_CANCELLED, "Sync cancelled");
      goto cleanup;
    }
  }

  /* === Phase 6: Collect stats === */
  {
    NostrNegStats neg_stats;
    nostr_neg_get_stats(neg, &neg_stats);
    stats->rounds = neg_stats.rounds;
    /* If no explicit ID exchanges occurred, fingerprints matched */
    stats->in_sync = (neg_stats.ids_sent == 0 && neg_stats.ids_recv == 0
                      && neg_stats.rounds <= 2);
  }

  g_debug("[NEG] Sync complete: %u rounds, in_sync=%d, local=%u",
          stats->rounds, stats->in_sync, stats->local_count);

  /* Disconnect relay and return results */
  nostr_relay_disconnect(relay);
  nostr_relay_free(relay);
  nostr_neg_session_free(neg);
  g_free(ds_ctx.items);
  g_free(sub_id);
  cleanup_neg_state();

  g_task_return_pointer(task, stats, g_free);
  return;

cleanup:
  free(initial_hex);
  cleanup_neg_state();
  nostr_relay_disconnect(relay);
  nostr_relay_free(relay);
  nostr_neg_session_free(neg);
  g_free(ds_ctx.items);
  g_free(sub_id);
  g_free(stats);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void
gnostr_neg_sync_kinds_async(const char *relay_url,
                             const int *kinds, size_t kind_count,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  g_return_if_fail(relay_url != NULL);
  g_return_if_fail(kinds != NULL && kind_count > 0);

  SyncTaskData *td = g_new0(SyncTaskData, 1);
  td->relay_url = g_strdup(relay_url);
  td->kinds = g_memdup2(kinds, kind_count * sizeof(int));
  td->kind_count = kind_count;

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_task_data(task, td, task_data_free);
  g_task_run_in_thread(task, sync_task);
  g_object_unref(task);
}

gboolean
gnostr_neg_sync_kinds_finish(GAsyncResult *result,
                              GnostrNegSyncStats *stats_out,
                              GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), FALSE);

  GnostrNegSyncStats *stats = g_task_propagate_pointer(G_TASK(result), error);
  if (!stats) return FALSE;

  if (stats_out) *stats_out = *stats;
  g_free(stats);
  return TRUE;
}
