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
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr/nip77/negentropy.h>
#include <nostr-relay.h>
#include <nostr-json.h>
#include <nostr-subscription.h>
#include <nostr-filter.h>
#include <nostr-event.h>
#include <select.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include <channel.h>
#include <string.h>
#include <stdlib.h>

#define NEG_HANDSHAKE_TIMEOUT_MS 5000   /* max wait for WebSocket establishment */
#define NEG_PROTOCOL_DEADLINE_SECS 30  /* max wait for relay NIP-77 response (GCond, not polling) */

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

static gboolean
append_authors_json(GString *json, const char * const *authors, size_t author_count)
{
  if (!json || !authors || author_count == 0) return FALSE;

  size_t appended = 0;
  for (size_t i = 0; i < author_count; i++) {
    if (!authors[i] || !*authors[i]) continue;
    if (appended == 0)
      g_string_append(json, ",\"authors\":[");
    else
      g_string_append_c(json, ',');
    g_string_append_printf(json, "\"%s\"", authors[i]);
    appended++;
  }

  if (appended > 0)
    g_string_append_c(json, ']');
  return appended > 0;
}

/**
 * Build sorted (created_at, id) datasource from local NDB events of
 * specified kinds. This is the "local state fingerprint" that the
 * negentropy protocol will compare against the relay's event set.
 */
static gboolean
build_kind_datasource(const int *kinds, size_t kind_count,
                      const char * const *authors, size_t author_count,
                      NostrNegDataSource *ds_out,
                      KindFilteredDS *ctx_out,
                      guint *local_count)
{
  GString *filt = g_string_new("[{\"kinds\":[");
  for (size_t i = 0; i < kind_count; i++) {
    if (i > 0) g_string_append_c(filt, ',');
    g_string_append_printf(filt, "%d", kinds[i]);
  }
  g_string_append(filt, "]");
  append_authors_json(filt, authors, author_count);
  g_string_append(filt, "}]");

  void *txn = NULL;
  int rc = storage_ndb_begin_query_retry(&txn, 3, 10, NULL);
  if (rc != 0 || !txn) {
    g_string_free(filt, TRUE);
    return FALSE;
  }

  char **results = NULL;
  int count = 0;
  rc = storage_ndb_query(txn, filt->str, &results, &count, NULL);
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
 * Per-session NEG-MSG Receive Context
 *
 * libnostr's custom_handler currently has no user_data, so we keep only a
 * small process-wide registry from subscription id to per-session context.
 * The mutable protocol state itself lives on NegSessionContext, allowing
 * concurrent sync sessions without sharing response slots.
 * ============================================================================ */

typedef struct {
  GMutex mu;
  GCond  cond;
  gchar *sub_id;
  gchar *hex;
  gchar *err_reason;
  gboolean got_msg;
  gboolean got_err;
  gboolean saw_auth;
} NegSessionContext;

static GMutex g_neg_sessions_mu;
static GHashTable *g_neg_sessions_by_sub_id;

static void
neg_session_context_init(NegSessionContext *ctx, const char *sub_id)
{
  g_mutex_init(&ctx->mu);
  g_cond_init(&ctx->cond);
  ctx->sub_id = g_strdup(sub_id);
}

static void
neg_session_context_clear(NegSessionContext *ctx)
{
  if (!ctx) return;
  g_clear_pointer(&ctx->sub_id, g_free);
  g_clear_pointer(&ctx->hex, g_free);
  g_clear_pointer(&ctx->err_reason, g_free);
  g_cond_clear(&ctx->cond);
  g_mutex_clear(&ctx->mu);
}

static void
neg_sessions_register(NegSessionContext *ctx)
{
  g_mutex_lock(&g_neg_sessions_mu);
  if (!g_neg_sessions_by_sub_id)
    g_neg_sessions_by_sub_id = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(g_neg_sessions_by_sub_id, ctx->sub_id, ctx);
  g_mutex_unlock(&g_neg_sessions_mu);
}

static void
neg_sessions_unregister(NegSessionContext *ctx)
{
  g_mutex_lock(&g_neg_sessions_mu);
  if (g_neg_sessions_by_sub_id && ctx && ctx->sub_id)
    g_hash_table_remove(g_neg_sessions_by_sub_id, ctx->sub_id);
  g_mutex_unlock(&g_neg_sessions_mu);
}

static void
neg_session_set_error(NegSessionContext *ctx, const char *reason)
{
  if (!ctx) return;
  g_mutex_lock(&ctx->mu);
  g_free(ctx->err_reason);
  ctx->err_reason = g_strdup(reason ? reason : "unknown protocol error");
  ctx->got_err = TRUE;
  g_cond_signal(&ctx->cond);
  g_mutex_unlock(&ctx->mu);
}

static void
neg_session_set_auth_required(NegSessionContext *ctx, const char *challenge)
{
  /* TODO(F06): once Bucket 2 lands nip42_parse_challenge() and
   * nip42_build_auth_response(), call them here, sign the kind-22242 AUTH
   * event via the app signer, and send the returned ["AUTH", <event>]
   * frame on this session's relay before continuing the NEG exchange.
   * Until those helpers are available in this bucket, fail loudly instead
   * of silently dropping AUTH and returning an empty sync result. */
  g_autofree gchar *msg = g_strdup_printf(
      "Relay requested NIP-42 AUTH%s%s, but nip42 auth helpers are not available in this build",
      challenge && *challenge ? ": " : "",
      challenge && *challenge ? challenge : "");
  if (ctx) {
    g_mutex_lock(&ctx->mu);
    ctx->saw_auth = TRUE;
    g_mutex_unlock(&ctx->mu);
  }
  neg_session_set_error(ctx, msg);
}

static bool
neg_handler(const char *raw)
{
  if (!raw) return false;

  char *type = gnostr_json_get_array_string(raw, NULL, 0, NULL);
  if (!type) return false;

  gboolean is_msg = (strcmp(type, "NEG-MSG") == 0);
  gboolean is_err = (strcmp(type, "NEG-ERR") == 0);
  gboolean is_auth = (strcmp(type, "AUTH") == 0);
  free(type);

  if (is_auth) {
    char *challenge = gnostr_json_get_array_string(raw, NULL, 1, NULL);
    /* AUTH frames are connection-scoped. The auth callback with userdata is
     * the normal path; this fallback handles relays that route AUTH through
     * the extension handler by notifying all active sessions. */
    g_mutex_lock(&g_neg_sessions_mu);
    if (g_neg_sessions_by_sub_id) {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init(&iter, g_neg_sessions_by_sub_id);
      while (g_hash_table_iter_next(&iter, &key, &value))
        neg_session_set_auth_required((NegSessionContext *)value, challenge);
    }
    g_mutex_unlock(&g_neg_sessions_mu);
    if (challenge) free(challenge);
    return true;
  }

  if (!is_msg && !is_err) return false;

  char *sub = gnostr_json_get_array_string(raw, NULL, 1, NULL);
  char *val = gnostr_json_get_array_string(raw, NULL, 2, NULL);
  gboolean match = FALSE;

  g_mutex_lock(&g_neg_sessions_mu);
  NegSessionContext *ctx = NULL;
  if (g_neg_sessions_by_sub_id && sub)
    ctx = g_hash_table_lookup(g_neg_sessions_by_sub_id, sub);
  if (ctx) {
    match = TRUE;
    g_mutex_lock(&ctx->mu);
    if (is_msg) {
      if (val && *val) {
        g_free(ctx->hex);
        ctx->hex = g_strdup(val);
        ctx->got_msg = TRUE;
      } else {
        g_free(ctx->err_reason);
        ctx->err_reason = g_strdup("NEG-MSG missing payload");
        ctx->got_err = TRUE;
      }
    } else {
      g_free(ctx->err_reason);
      ctx->err_reason = val ? g_strdup(val) : g_strdup("unknown");
      ctx->got_err = TRUE;
    }
    g_cond_signal(&ctx->cond);
    g_mutex_unlock(&ctx->mu);
  }
  g_mutex_unlock(&g_neg_sessions_mu);

  if (val) free(val);
  if (sub) free(sub);
  return match;
}

static void
neg_auth_callback(NostrRelay *relay, const char *challenge, void *user_data)
{
  (void)relay;
  neg_session_set_auth_required((NegSessionContext *)user_data, challenge);
}

/**
 * Wait for NEG-MSG or NEG-ERR response with timeout.
 * Returns hex payload on success, sets error on failure.
 */
static gboolean
wait_neg_response(NegSessionContext *ctx, gchar **hex_out, gint64 deadline_us, GError **error)
{
  g_mutex_lock(&ctx->mu);
  while (!ctx->got_msg && !ctx->got_err) {
    if (!g_cond_wait_until(&ctx->cond, &ctx->mu, deadline_us)) {
      g_mutex_unlock(&ctx->mu);
      g_set_error_literal(error, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_TIMEOUT,
                          "Relay did not respond (NIP-77 may not be supported)");
      return FALSE;
    }
  }

  if (ctx->got_err) {
    g_autofree gchar *reason = g_steal_pointer(&ctx->err_reason);
    ctx->got_err = FALSE;
    g_mutex_unlock(&ctx->mu);
    g_set_error(error, GNOSTR_NEG_ERROR,
                ctx->saw_auth ? GNOSTR_NEG_ERROR_CONNECTION : GNOSTR_NEG_ERROR_UNSUPPORTED,
                "%s", reason ? reason : "unknown relay error");
    return FALSE;
  }

  *hex_out = g_steal_pointer(&ctx->hex);
  ctx->got_msg = FALSE;
  g_mutex_unlock(&ctx->mu);
  return TRUE;
}

/* ============================================================================
 * Event Fetching — download NEED events from relay after reconciliation
 * ============================================================================ */

#define FETCH_TIMEOUT_MS  30000  /* 30s for event fetch */
#define FETCH_BATCH_SIZE  256    /* Max IDs per REQ */

static gchar *
bin2hex_g(const unsigned char *bin, size_t len)
{
  static const char hex[] = "0123456789abcdef";
  gchar *out = g_malloc(len * 2 + 1);
  for (size_t i = 0; i < len; i++) {
    out[2 * i]     = hex[bin[i] >> 4];
    out[2 * i + 1] = hex[bin[i] & 0x0F];
  }
  out[len * 2] = '\0';
  return out;
}

/**
 * Fetch NEED events from relay after negentropy reconciliation.
 * Sends REQ with ID filter, receives events, and ingests into NDB.
 * Returns number of events successfully fetched and ingested.
 */
static guint
fetch_need_events(NostrRelay *relay, NostrNegSession *neg,
                  GCancellable *cancel)
{
  const unsigned char *need_ids = NULL;
  size_t need_count = 0;
  if (nostr_neg_get_need_ids(neg, &need_ids, &need_count) != 0 ||
      need_count == 0)
    return 0;

  g_debug("[NEG] Fetching %zu missing events", need_count);

  guint total_fetched = 0;

  /* Batch IDs into groups to avoid giant filters */
  for (size_t batch_start = 0; batch_start < need_count;
       batch_start += FETCH_BATCH_SIZE) {
    if (g_cancellable_is_cancelled(cancel)) break;

    size_t batch_end = batch_start + FETCH_BATCH_SIZE;
    if (batch_end > need_count) batch_end = need_count;
    size_t batch_size = batch_end - batch_start;

    /* Build filter with IDs */
    NostrFilter *filter = nostr_filter_new();
    for (size_t i = batch_start; i < batch_end; i++) {
      gchar *hex = bin2hex_g(need_ids + i * 32, 32);
      nostr_filter_add_id(filter, hex);
      g_free(hex);
    }

    NostrFilters *filters = nostr_filters_new();
    nostr_filters_add(filters, filter);

    /* Create and fire subscription */
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    if (!sub) {
      g_warning("[NEG] Failed to prepare fetch subscription");
      break;
    }

    Error *fire_err = NULL;
    if (!nostr_subscription_fire(sub, &fire_err)) {
      g_warning("[NEG] Failed to fire fetch subscription: %s",
                fire_err ? fire_err->message : "unknown");
      if (fire_err) free(fire_err);
      nostr_subscription_free(sub);
      break;
    }

    /* Receive events until EOSE or timeout */
    guint batch_fetched = 0;
    gboolean done = FALSE;
    while (!done && !g_cancellable_is_cancelled(cancel)) {
      NostrEvent *event = NULL;
      void *eose_val = NULL;
      GoSelectCase cases[2] = {
        { .op = GO_SELECT_RECEIVE, .chan = sub->events,
          .recv_buf = (void **)&event },
        { .op = GO_SELECT_RECEIVE, .chan = sub->end_of_stored_events,
          .recv_buf = &eose_val },
      };

      GoSelectResult result = go_select_timeout(cases, 2, FETCH_TIMEOUT_MS);

      if (result.selected_case == 0 && event) {
        /* EVENT received — serialize and ingest */
        char *json = nostr_event_serialize(event);
        if (json) {
          storage_ndb_ingest_event_json(json, NULL);
          batch_fetched++;
          free(json);
        }
        nostr_event_free(event);
      } else if (result.selected_case == 1) {
        /* EOSE — all stored events received */
        done = TRUE;
      } else {
        /* timeout or error */
        g_debug("[NEG] Fetch timeout after %u events", batch_fetched);
        done = TRUE;
      }
    }

    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    total_fetched += batch_fetched;

    g_debug("[NEG] Batch fetched %u/%zu events",
            batch_fetched, batch_size);
  }

  return total_fetched;
}

/* ============================================================================
 * GTask Implementation
 * ============================================================================ */

typedef struct {
  gchar *relay_url;
  int   *kinds;
  size_t kind_count;
  gchar **authors;
  size_t author_count;
} SyncTaskData;

static void
task_data_free(gpointer p)
{
  SyncTaskData *d = p;
  g_free(d->relay_url);
  g_free(d->kinds);
  g_strfreev(d->authors);
  g_free(d);
}

/* State callback: unblocks the handshake wait when relay connects or fails. */
static void
neg_relay_state_cb(NostrRelay *relay,
                   NostrRelayConnectionState old_state,
                   NostrRelayConnectionState new_state,
                   void *user_data)
{
  (void)relay; (void)old_state;
  GoChannel *ch = (GoChannel *)user_data;
  if (new_state == NOSTR_RELAY_STATE_CONNECTED ||
      new_state == NOSTR_RELAY_STATE_DISCONNECTED) {
    int dummy = 1;
    go_channel_try_send(ch, &dummy);
  }
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
  if (!build_kind_datasource(td->kinds, td->kind_count,
                             (const char * const *)td->authors, td->author_count,
                             &ds, &ds_ctx,
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

  /* Set up per-session NEG-MSG/AUTH handlers and subscription ID */
  gchar *sub_id = g_strdup_printf("neg-%04x", g_random_int_range(0, 0xFFFF));
  NegSessionContext neg_ctx = {0};
  neg_session_context_init(&neg_ctx, sub_id);
  neg_sessions_register(&neg_ctx);
  nostr_relay_set_custom_handler(relay, neg_handler);
  nostr_relay_set_auth_callback(relay, neg_auth_callback, &neg_ctx);

  if (!nostr_relay_connect(relay, &relay_err)) {
    g_task_return_new_error(task, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_CONNECTION,
                            "Relay connect failed: %s",
                            relay_err ? relay_err->message : "unknown");
    if (relay_err) free(relay_err);
    goto cleanup;
  }

  /* Wait for WebSocket handshake using state callback + channel (no polling).
   * The relay fires the state callback from its worker thread when the
   * connection transitions to CONNECTED or DISCONNECTED. */
  if (!nostr_relay_is_established(relay)) {
    GoChannel *ready_ch = go_channel_create(1);
    nostr_relay_set_state_callback(relay, neg_relay_state_cb, ready_ch);

    /* Check again after setting callback to avoid race */
    if (!nostr_relay_is_established(relay)) {
      GoSelectCase cases[1] = {
        { .op = GO_SELECT_RECEIVE, .chan = ready_ch, .recv_buf = NULL },
      };
      go_select_timeout(cases, 1, NEG_HANDSHAKE_TIMEOUT_MS);
    }

    nostr_relay_set_state_callback(relay, NULL, NULL);
    go_channel_free(ready_ch);
  }
  if (!nostr_relay_is_established(relay)) {
    g_task_return_new_error(task, GNOSTR_NEG_ERROR, GNOSTR_NEG_ERROR_CONNECTION,
                            "WebSocket handshake failed");
    goto cleanup;
  }

  /* === Phase 4: NEG-OPEN === */
  {
    GString *filt_json = g_string_new("{\"kinds\":[");
    for (size_t i = 0; i < td->kind_count; i++) {
      if (i > 0) g_string_append_c(filt_json, ',');
      g_string_append_printf(filt_json, "%d", td->kinds[i]);
    }
    g_string_append(filt_json, "]");
    append_authors_json(filt_json, (const char * const *)td->authors, td->author_count);
    g_string_append_c(filt_json, '}');

    gchar *neg_open = g_strdup_printf("[\"NEG-OPEN\",\"%s\",%s,\"%s\"]",
                                       sub_id, filt_json->str, initial_hex);
    g_string_free(filt_json, TRUE);
    free(initial_hex);
    initial_hex = NULL;

    /* nostr_relay_write takes ownership of a malloc'd string */
    GoChannel *wch = nostr_relay_write(relay, strdup(neg_open));
    g_free(neg_open);
    /* hq-e3ach: close + unref to drop our reference; write_operations
     * holds the other ref and will free when done. */
    if (wch) { go_channel_close(wch); go_channel_unref(wch); }
  }

  /* === Phase 5: Protocol loop === */
  {
    gint64 deadline = g_get_monotonic_time() + NEG_PROTOCOL_DEADLINE_SECS * G_USEC_PER_SEC;
    GError *proto_err = NULL;

    while (!g_cancellable_is_cancelled(cancel)) {
      gchar *response_hex = NULL;
      if (!wait_neg_response(&neg_ctx, &response_hex, deadline, &proto_err))
        break;
      if (!response_hex) {
        g_set_error_literal(&proto_err, GNOSTR_NEG_ERROR,
                            GNOSTR_NEG_ERROR_PROTOCOL,
                            "NEG-MSG had no payload");
        break;
      }

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
      if (wch) { go_channel_close(wch); go_channel_unref(wch); }  /* hq-e3ach */
    }

    /* === Phase 5.5: Fetch missing events (NEED IDs) === */
    if (!proto_err && !g_cancellable_is_cancelled(cancel)) {
      stats->events_fetched = fetch_need_events(relay, neg, cancel);
      if (stats->events_fetched > 0) {
        g_debug("[NEG] Fetched %u events from relay", stats->events_fetched);
      }
    }

    /* Send NEG-CLOSE regardless of outcome */
    {
      gchar *neg_close = g_strdup_printf("[\"NEG-CLOSE\",\"%s\"]", sub_id);
      GoChannel *wch = nostr_relay_write(relay, strdup(neg_close));
      g_free(neg_close);
      if (wch) { go_channel_close(wch); go_channel_unref(wch); }  /* hq-e3ach */
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
  nostr_relay_set_auth_callback(relay, NULL, NULL);
  nostr_relay_set_custom_handler(relay, NULL);
  nostr_relay_disconnect(relay);
  nostr_relay_free(relay);
  nostr_neg_session_free(neg);
  g_free(ds_ctx.items);
  g_free(sub_id);
  neg_sessions_unregister(&neg_ctx);
  neg_session_context_clear(&neg_ctx);

  g_task_return_pointer(task, stats, g_free);
  return;

cleanup:
  free(initial_hex);
  nostr_relay_set_auth_callback(relay, NULL, NULL);
  nostr_relay_set_custom_handler(relay, NULL);
  neg_sessions_unregister(&neg_ctx);
  neg_session_context_clear(&neg_ctx);
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
  gnostr_neg_sync_kinds_for_authors_async(relay_url, kinds, kind_count,
                                          NULL, 0, cancellable,
                                          callback, user_data);
}

void
gnostr_neg_sync_kinds_for_authors_async(const char *relay_url,
                                         const int *kinds, size_t kind_count,
                                         const char * const *authors,
                                         size_t author_count,
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
  if (authors && author_count > 0) {
    td->authors = g_new0(gchar *, author_count + 1);
    for (size_t i = 0; i < author_count; i++)
      td->authors[i] = g_strdup(authors[i]);
    td->author_count = author_count;
  }

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
