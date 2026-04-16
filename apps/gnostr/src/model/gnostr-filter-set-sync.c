/* gnostr-filter-set-sync.c — NIP-78 sync layer for custom filter sets.
 *
 * SPDX-License-Identifier: MIT
 *
 * See gnostr-filter-set-sync.h for the high-level design notes.
 *
 * Internals
 * ---------
 * A process-wide static SyncState holds the enabled pubkey, the
 * manager handler id for items-changed, and a debounce timeout id.
 * The items-changed handler ignores mutations triggered by apply()
 * itself via the `applying_remote` guard, so pulls never round-trip
 * into pushes.
 *
 * All serialization reuses gnostr_filter_set_to_json()/new_from_json()
 * so the wire format stays bit-compatible with the on-disk storage.
 *
 * nostrc-yg8j.9: Filter set persistence via NIP-78.
 */

#define G_LOG_DOMAIN "gnostr-filter-set-sync"

#include "gnostr-filter-set-sync.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "gnostr-filter-set.h"
#include "gnostr-filter-set-manager.h"

#ifndef GNOSTR_FILTER_SET_SYNC_TEST_ONLY
#  include "../util/gnostr-app-data-manager.h"
#endif

/* ------------------------------------------------------------------------
 * Debounce window
 * ------------------------------------------------------------------------
 *
 * Long enough that a burst of add/update/remove calls during an
 * import coalesce into a single publish; short enough that a user
 * editing a filter still sees their change propagate before they
 * pick up a second device.
 */
#define FLUSH_DELAY_MS 3000

/* ------------------------------------------------------------------------
 * Shared state
 * ------------------------------------------------------------------------ */

typedef struct _SyncState {
  gchar        *pubkey_hex;          /* owned; NULL when disabled       */
  GListModel   *watched_model;       /* strong ref; connect/disconnect  */
  gulong        items_changed_id;    /* manager::items-changed handler  */
  guint         flush_source_id;     /* GSource id for debounce timer   */
  gboolean      applying_remote;     /* reentrancy guard during apply() */
  GCancellable *cancellable;         /* cancels in-flight pull/push     */
} SyncState;

static SyncState g_state = {
  .pubkey_hex       = NULL,
  .watched_model    = NULL,
  .items_changed_id = 0,
  .flush_source_id  = 0,
  .applying_remote  = FALSE,
  .cancellable      = NULL,
};

/* Process-wide flag: have we triggered the default manager's load()
 * at least once? Prod code currently never calls load() explicitly,
 * so sync_enable() does it defensively on first invocation so our
 * pull/apply round-trips see a fully-populated manager (predefined
 * sets + on-disk custom sets) and any later call to load() from
 * other code paths is idempotent. */
static gboolean g_manager_loaded_once = FALSE;

/* Forward declarations */
static void on_manager_items_changed(GListModel *model,
                                     guint position,
                                     guint removed,
                                     guint added,
                                     gpointer user_data);
static gboolean flush_debounced_push(gpointer user_data);
static void schedule_debounced_push(void);

/* ------------------------------------------------------------------------
 * Serialization helpers
 * ------------------------------------------------------------------------ */

gchar *
gnostr_filter_set_sync_serialize(void)
{
  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  GListModel *model = gnostr_filter_set_manager_get_model(mgr);
  if (!model) return NULL;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "version");
  json_builder_add_int_value(builder, GNOSTR_FILTER_SET_SYNC_FORMAT_VERSION);
  json_builder_set_member_name(builder, "filter_sets");
  json_builder_begin_array(builder);

  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    GnostrFilterSet *fs = g_list_model_get_item(model, i);
    if (!fs) continue;

    if (gnostr_filter_set_get_source(fs) != GNOSTR_FILTER_SET_SOURCE_CUSTOM) {
      g_object_unref(fs);
      continue;
    }

    g_autofree gchar *item_json = gnostr_filter_set_to_json(fs);
    g_object_unref(fs);
    if (!item_json) continue;

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, item_json, -1, NULL)) continue;

    /* add_value takes ownership of the node, so duplicate the parser's
     * root — it keeps its own copy. */
    JsonNode *node = json_node_copy(json_parser_get_root(parser));
    json_builder_add_value(builder, node);
  }

  json_builder_end_array(builder);   /* filter_sets */
  json_builder_end_object(builder);  /* root */

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  gchar *out = json_generator_to_data(gen, NULL);
  json_node_free(root);
  return out;
}

/* Walk @model and collect the ids of every currently-stored custom
 * filter set into the returned table. Predefined ids are not
 * included. The caller owns the table. */
static GHashTable *
collect_local_custom_ids(GListModel *model)
{
  GHashTable *ids = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, NULL);
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    GnostrFilterSet *fs = g_list_model_get_item(model, i);
    if (!fs) continue;
    if (gnostr_filter_set_get_source(fs) == GNOSTR_FILTER_SET_SOURCE_CUSTOM) {
      const gchar *id = gnostr_filter_set_get_id(fs);
      if (id && *id)
        g_hash_table_add(ids, g_strdup(id));
    }
    g_object_unref(fs);
  }
  return ids;
}

gboolean
gnostr_filter_set_sync_apply(const gchar *json, GError **error)
{
  g_return_val_if_fail(json != NULL, FALSE);

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, -1, error))
    return FALSE;

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "filter-set sync payload must be a JSON object");
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);
  if (!json_object_has_member(obj, "filter_sets")) {
    /* Empty payload is valid — remote has an empty list for this
     * account. Nothing to apply. */
    return TRUE;
  }

  JsonNode *arr_node = json_object_get_member(obj, "filter_sets");
  if (!JSON_NODE_HOLDS_ARRAY(arr_node)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "filter-set sync payload \"filter_sets\" must be an array");
    return FALSE;
  }

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  GListModel *model = gnostr_filter_set_manager_get_model(mgr);

  /* Guard against our own items-changed handler scheduling a push
   * after every add/update below. We already know the data we're
   * installing matches the remote snapshot, so a push would be
   * redundant. */
  g_state.applying_remote = TRUE;

  g_autoptr(GHashTable) local_ids = collect_local_custom_ids(model);

  JsonArray *arr = json_node_get_array(arr_node);
  guint len = json_array_get_length(arr);
  guint applied = 0, updated = 0, added = 0;

  for (guint i = 0; i < len; i++) {
    JsonNode *item = json_array_get_element(arr, i);
    if (!JSON_NODE_HOLDS_OBJECT(item)) continue;

    g_autoptr(JsonGenerator) gen = json_generator_new();
    json_generator_set_root(gen, item);
    g_autofree gchar *item_json = json_generator_to_data(gen, NULL);
    if (!item_json) continue;

    g_autoptr(GError) parse_err = NULL;
    g_autoptr(GnostrFilterSet) fs =
      gnostr_filter_set_new_from_json(item_json, &parse_err);
    if (!fs) {
      g_warning("filter-set-sync: skipping invalid entry: %s",
                parse_err ? parse_err->message : "unknown");
      continue;
    }

    /* Paranoia: the remote payload could claim source=PREDEFINED for
     * some reason. Force CUSTOM — we never want the sync layer to
     * mint predefined sets. */
    gnostr_filter_set_set_source(fs, GNOSTR_FILTER_SET_SOURCE_CUSTOM);

    const gchar *id = gnostr_filter_set_get_id(fs);
    if (!id || !*id) continue;

    if (g_hash_table_contains(local_ids, id)) {
      if (gnostr_filter_set_manager_update(mgr, fs)) {
        updated++;
        applied++;
      }
    } else {
      if (gnostr_filter_set_manager_add(mgr, fs)) {
        added++;
        applied++;
      }
    }
  }

  g_state.applying_remote = FALSE;

  g_debug("filter-set-sync: applied %u set(s) from remote (%u added, %u updated)",
          applied, added, updated);

  /* Persist the merged state so a relay-less restart still sees the
   * remote data. */
  g_autoptr(GError) save_err = NULL;
  if (!gnostr_filter_set_manager_save(mgr, &save_err)) {
    g_warning("filter-set-sync: local save after apply failed: %s",
              save_err ? save_err->message : "unknown");
    /* Not fatal — the in-memory state still reflects the merge. */
  }

  return TRUE;
}

/* ------------------------------------------------------------------------
 * Enable / disable
 * ------------------------------------------------------------------------ */

#ifndef GNOSTR_FILTER_SET_SYNC_TEST_ONLY

static void on_initial_pull_done(gboolean success,
                                 const gchar *error_msg,
                                 gpointer user_data);

#endif /* GNOSTR_FILTER_SET_SYNC_TEST_ONLY */

void
gnostr_filter_set_sync_enable(const gchar *pubkey_hex)
{
  if (!pubkey_hex || !*pubkey_hex) {
    g_debug("filter-set-sync: enable() called with empty pubkey — ignoring");
    return;
  }

  /* Re-enabling with the same pubkey: no-op. */
  if (g_state.pubkey_hex && g_strcmp0(g_state.pubkey_hex, pubkey_hex) == 0)
    return;

  /* Switching pubkey: tear down the old hooks first. */
  if (g_state.pubkey_hex)
    gnostr_filter_set_sync_disable();

  g_state.pubkey_hex = g_strdup(pubkey_hex);
  g_state.cancellable = g_cancellable_new();

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();

  /* Defensive load(): the production call graph never invokes load()
   * explicitly today, so on first enable() we kick it off ourselves.
   * load() is idempotent with respect to on-disk content — it always
   * begins with remove_all + install_defaults + re-append from JSON —
   * so calling it again from another future call path is harmless. */
  if (!g_manager_loaded_once) {
    g_autoptr(GError) load_err = NULL;
    if (!gnostr_filter_set_manager_load(mgr, &load_err)) {
      g_warning("filter-set-sync: manager load failed: %s",
                load_err ? load_err->message : "unknown");
      /* Not fatal — proceed with whatever state the manager has. */
    }
    g_manager_loaded_once = TRUE;
  }

  GListModel *model = gnostr_filter_set_manager_get_model(mgr);
  if (model && g_state.items_changed_id == 0) {
    g_state.watched_model = g_object_ref(model);
    g_state.items_changed_id =
      g_signal_connect(model, "items-changed",
                       G_CALLBACK(on_manager_items_changed), NULL);
  }

#ifndef GNOSTR_FILTER_SET_SYNC_TEST_ONLY
  /* Thread the pubkey through to the app-data manager and kick off
   * the initial pull. We don't gate on is_sync_enabled() here so that
   * filter sets sync regardless of the broader app-data toggle — the
   * feature is currently opt-out via login/logout rather than a user
   * preference. */
  GnostrAppDataManager *app = gnostr_app_data_manager_get_default();
  gnostr_app_data_manager_set_user_pubkey(app, pubkey_hex);

  g_message("filter-set-sync: enabled for %.8s…, starting initial pull",
            pubkey_hex);
  gnostr_filter_set_sync_pull_async(g_state.cancellable,
                                    on_initial_pull_done, NULL);
#else
  g_message("filter-set-sync: enabled for %.8s… (test mode, no pull)",
            pubkey_hex);
#endif
}

void
gnostr_filter_set_sync_disable(void)
{
  if (g_state.flush_source_id != 0) {
    g_source_remove(g_state.flush_source_id);
    g_state.flush_source_id = 0;
  }

  if (g_state.items_changed_id != 0) {
    /* Disconnect on the model ref we captured at connect time — not
     * on whatever get_model() returns right now — so teardown works
     * even if the default manager has been reset in the meantime
     * (which only happens in tests). */
    if (g_state.watched_model)
      g_signal_handler_disconnect(g_state.watched_model,
                                  g_state.items_changed_id);
    g_state.items_changed_id = 0;
  }
  g_clear_object(&g_state.watched_model);

  if (g_state.cancellable) {
    g_cancellable_cancel(g_state.cancellable);
    g_clear_object(&g_state.cancellable);
  }

  g_clear_pointer(&g_state.pubkey_hex, g_free);
  g_state.applying_remote = FALSE;
}

gboolean
gnostr_filter_set_sync_is_enabled(void)
{
  return g_state.pubkey_hex != NULL && *g_state.pubkey_hex != '\0';
}

/* ------------------------------------------------------------------------
 * Items-changed debounce
 * ------------------------------------------------------------------------ */

static void
on_manager_items_changed(GListModel *model,
                         guint position,
                         guint removed,
                         guint added,
                         gpointer user_data)
{
  (void)model; (void)position; (void)removed; (void)added; (void)user_data;

  if (g_state.applying_remote) return;
  if (!g_state.pubkey_hex)     return;

  schedule_debounced_push();
}

static void
schedule_debounced_push(void)
{
  if (g_state.flush_source_id != 0)
    g_source_remove(g_state.flush_source_id);

  g_state.flush_source_id =
    g_timeout_add(FLUSH_DELAY_MS, flush_debounced_push, NULL);
}

static gboolean
flush_debounced_push(gpointer user_data)
{
  (void)user_data;
  g_state.flush_source_id = 0;

  if (!g_state.pubkey_hex) return G_SOURCE_REMOVE;

  gnostr_filter_set_sync_push_async(g_state.cancellable, NULL, NULL);
  return G_SOURCE_REMOVE;
}

#ifdef GNOSTR_FILTER_SET_SYNC_TEST_ONLY
/* Test-only accessor: returns TRUE iff a debounced push is currently
 * pending. Lets unit tests assert the applying_remote guard actually
 * suppressed the echo push that would otherwise fire on every mutation
 * inside apply(). */
gboolean
gnostr_filter_set_sync_debug_has_pending_push(void)
{
  return g_state.flush_source_id != 0;
}
#endif

/* ------------------------------------------------------------------------
 * Async push / pull
 * ------------------------------------------------------------------------ */

#ifndef GNOSTR_FILTER_SET_SYNC_TEST_ONLY

typedef struct {
  GCancellable               *cancellable;
  GnostrFilterSetSyncCallback callback;
  gpointer                    user_data;
} SyncOpCtx;

static SyncOpCtx *
sync_op_ctx_new(GCancellable *cancellable,
                GnostrFilterSetSyncCallback cb, gpointer user_data)
{
  SyncOpCtx *ctx = g_new0(SyncOpCtx, 1);
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->callback = cb;
  ctx->user_data = user_data;
  return ctx;
}

static void
sync_op_ctx_free(SyncOpCtx *ctx)
{
  if (!ctx) return;
  g_clear_object(&ctx->cancellable);
  g_free(ctx);
}

static gboolean
sync_op_ctx_is_cancelled(SyncOpCtx *ctx)
{
  return ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable);
}

static void
sync_op_ctx_report(SyncOpCtx *ctx, gboolean ok, const gchar *err)
{
  if (ctx->callback)
    ctx->callback(ok, err, ctx->user_data);
  sync_op_ctx_free(ctx);
}

/* ---- Pull ---- */

static void
on_custom_data_fetched(GnostrAppDataManager *manager,
                       const char *content,
                       gint64 created_at,
                       gboolean success,
                       const char *error_message,
                       gpointer user_data)
{
  (void)manager; (void)created_at;
  SyncOpCtx *ctx = user_data;
  if (!ctx) return;

  if (sync_op_ctx_is_cancelled(ctx)) {
    sync_op_ctx_report(ctx, FALSE, "cancelled");
    return;
  }

  if (!success) {
    /* Not finding an event is expected on a fresh account — treat it
     * as a successful no-op rather than a hard error.
     *
     * FRAGILITY: #GnostrAppDataManager does not currently expose
     * typed GError domains, so we sniff the string instead. If the
     * upstream message text ever changes or gets localized this
     * branch stops firing and we surface a spurious failure to the
     * caller (still non-fatal — see on_initial_pull_done). The
     * long-term fix is a GError code like APP_DATA_ERROR_NOT_FOUND
     * exported by gnostr-app-data-manager.h and matched via
     * g_error_matches() here. Tracked informally with yg8j.9. */
    if (error_message && (strstr(error_message, "No matching data") ||
                          strstr(error_message, "No app data"))) {
      g_debug("filter-set-sync: no remote filter sets for this user yet");
      sync_op_ctx_report(ctx, TRUE, NULL);
      return;
    }
    /* Downgrade to debug: unreachable relays during login are common
     * and we don't want to spam the console on every cold start. */
    g_debug("filter-set-sync: pull failed: %s",
            error_message ? error_message : "unknown");
    sync_op_ctx_report(ctx, FALSE, error_message);
    return;
  }

  if (!content || !*content) {
    /* Empty content: nothing to merge. */
    sync_op_ctx_report(ctx, TRUE, NULL);
    return;
  }

  g_autoptr(GError) apply_err = NULL;
  if (!gnostr_filter_set_sync_apply(content, &apply_err)) {
    g_warning("filter-set-sync: apply failed: %s",
              apply_err ? apply_err->message : "unknown");
    sync_op_ctx_report(ctx, FALSE,
                       apply_err ? apply_err->message : "apply failed");
    return;
  }

  g_message("filter-set-sync: pull applied (created_at=%" G_GINT64_FORMAT ")",
            created_at);
  sync_op_ctx_report(ctx, TRUE, NULL);
}

void
gnostr_filter_set_sync_pull_async(GCancellable *cancellable,
                                  GnostrFilterSetSyncCallback callback,
                                  gpointer user_data)
{
  if (!g_state.pubkey_hex) {
    if (callback) callback(FALSE, "sync disabled", user_data);
    return;
  }

  SyncOpCtx *ctx = sync_op_ctx_new(cancellable, callback, user_data);

  GnostrAppDataManager *app = gnostr_app_data_manager_get_default();
  gnostr_app_data_manager_get_custom_data_async(
      app, GNOSTR_FILTER_SET_SYNC_DATA_KEY, on_custom_data_fetched, ctx);
}

/* ---- Push ---- */

static void
on_custom_data_published(GnostrAppDataManager *manager,
                         gboolean success,
                         const char *error_message,
                         gpointer user_data)
{
  (void)manager;
  SyncOpCtx *ctx = user_data;
  if (!ctx) return;

  if (sync_op_ctx_is_cancelled(ctx)) {
    sync_op_ctx_report(ctx, FALSE, "cancelled");
    return;
  }

  if (!success) {
    g_warning("filter-set-sync: push failed: %s",
              error_message ? error_message : "unknown");
  } else {
    g_debug("filter-set-sync: push published");
  }
  sync_op_ctx_report(ctx, success, error_message);
}

void
gnostr_filter_set_sync_push_async(GCancellable *cancellable,
                                  GnostrFilterSetSyncCallback callback,
                                  gpointer user_data)
{
  if (!g_state.pubkey_hex) {
    if (callback) callback(FALSE, "sync disabled", user_data);
    return;
  }

  /* Always take a local snapshot first: if the relays are unreachable
   * the user's data still survives a restart. gnostr_filter_set_manager
   * already writes on every add/update/remove in practice, but be
   * defensive here since we're the authoritative "checkpoint" path. */
  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  g_autoptr(GError) save_err = NULL;
  if (!gnostr_filter_set_manager_save(mgr, &save_err)) {
    g_warning("filter-set-sync: local save before push failed: %s",
              save_err ? save_err->message : "unknown");
    /* Continue — pushing to relays can still succeed. */
  }

  g_autofree gchar *payload = gnostr_filter_set_sync_serialize();
  if (!payload) {
    if (callback) callback(FALSE, "serialization failed", user_data);
    return;
  }

  SyncOpCtx *ctx = sync_op_ctx_new(cancellable, callback, user_data);

  GnostrAppDataManager *app = gnostr_app_data_manager_get_default();
  gnostr_app_data_manager_set_custom_data_async(
      app, GNOSTR_FILTER_SET_SYNC_DATA_KEY, payload,
      on_custom_data_published, ctx);
}

/* ---- Initial pull on enable() ---- */

static void
on_initial_pull_done(gboolean success,
                     const gchar *error_msg,
                     gpointer user_data)
{
  (void)user_data;
  if (!success) {
    g_debug("filter-set-sync: initial pull failed (%s) — will retry on next mutation",
            error_msg ? error_msg : "unknown");
    return;
  }
  g_debug("filter-set-sync: initial pull succeeded");
}

#else  /* GNOSTR_FILTER_SET_SYNC_TEST_ONLY */

void
gnostr_filter_set_sync_pull_async(GCancellable *cancellable,
                                  GnostrFilterSetSyncCallback callback,
                                  gpointer user_data)
{
  (void)cancellable;
  if (callback) callback(FALSE, "test-only build: no relay access", user_data);
}

void
gnostr_filter_set_sync_push_async(GCancellable *cancellable,
                                  GnostrFilterSetSyncCallback callback,
                                  gpointer user_data)
{
  (void)cancellable;
  if (callback) callback(FALSE, "test-only build: no relay access", user_data);
}

#endif /* GNOSTR_FILTER_SET_SYNC_TEST_ONLY */
