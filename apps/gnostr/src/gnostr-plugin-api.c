/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-api.c - Plugin API implementation
 *
 * Implementation of GObject interfaces for the plugin system.
 *
 * nostrc-ih5: Guard context implementation for plugin builds.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gnostr-plugin-api.h"

/* App-internal includes - only needed when building the main app */
#ifndef GNOSTR_PLUGIN_BUILD
#include "storage_ndb.h"
#include "util/relays.h"
#include "ipc/gnostr-signer-service.h"
#include "util/utils.h"
/* nostr_pool.h provided via utils.h */
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-relay.h"
#include "nostr-json.h"
#include "nostr-filter.h"
#include "model/gn-ndb-sub-dispatcher.h"
#include "ui/gnostr-main-window.h"
#include "ui/gnostr-repo-browser.h"
#include <json-glib/json-glib.h>
#include <string.h>
#endif /* !GNOSTR_PLUGIN_BUILD */

/* Context implementation - only needed when building the main app */
#ifndef GNOSTR_PLUGIN_BUILD

/* ============================================================================
 * PLUGIN CONTEXT STRUCTURE
 * ============================================================================ */

struct _GnostrPluginContext
{
  GtkApplication *app;
  GtkWindow      *main_window;
  GNostrPool       *pool;
  char           *plugin_id;  /* For namespacing plugin data */

  /* Event subscriptions */
  GHashTable     *subscriptions;  /* subid (guint64) -> PluginSubscription* */
  guint64         next_sub_id;

  /* Action handlers */
  GHashTable     *actions;  /* action_name (char*) -> PluginAction* */
};

typedef struct
{
  char                    *name;
  GnostrPluginActionFunc   callback;
  gpointer                 user_data;
} PluginAction;

static void
plugin_action_free(PluginAction *action)
{
  if (!action) return;
  g_clear_pointer(&action->name, g_free);
  g_free(action);
}

typedef struct
{
  guint64          id;
  guint64          ndb_sub_id;
  char            *filter_json;
  GCallback        callback;
  gpointer         user_data;
  GDestroyNotify   destroy_notify;
} PluginSubscription;

static void
plugin_subscription_free(PluginSubscription *sub)
{
  if (!sub) return;
  if (sub->ndb_sub_id > 0) {
    storage_ndb_unsubscribe(sub->ndb_sub_id);
  }
  if (sub->destroy_notify && sub->user_data) {
    sub->destroy_notify(sub->user_data);
  }
  g_clear_pointer(&sub->filter_json, g_free);
  g_free(sub);
}

/* ============================================================================
 * PLUGIN CONTEXT LIFECYCLE
 * ============================================================================ */

GnostrPluginContext *
gnostr_plugin_context_new(GtkApplication *app, const char *plugin_id)
{
  GnostrPluginContext *ctx = g_new0(GnostrPluginContext, 1);
  ctx->app = app;
  ctx->plugin_id = g_strdup(plugin_id ? plugin_id : "unknown");
  ctx->subscriptions = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                              NULL, (GDestroyNotify)plugin_subscription_free);
  ctx->actions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        NULL, (GDestroyNotify)plugin_action_free);
  ctx->next_sub_id = 1;
  return ctx;
}

void
gnostr_plugin_context_free(GnostrPluginContext *ctx)
{
  if (!ctx) return;
  g_clear_pointer(&ctx->subscriptions, g_hash_table_unref);
  g_clear_pointer(&ctx->actions, g_hash_table_unref);
  g_clear_pointer(&ctx->plugin_id, g_free);
  g_free(ctx);
}

void
gnostr_plugin_context_set_main_window(GnostrPluginContext *ctx, GtkWindow *window)
{
  if (ctx) ctx->main_window = window;
}

void
gnostr_plugin_context_set_pool(GnostrPluginContext *ctx, GNostrPool *pool)
{
  if (ctx) ctx->pool = pool;
}

#endif /* !GNOSTR_PLUGIN_BUILD */

/* ============================================================================
 * INTERFACE IMPLEMENTATIONS
 * ============================================================================
 * These are only compiled in the host app, not in plugins.
 * Plugins use -undefined dynamic_lookup (macOS) or --unresolved-symbols (Linux)
 * to resolve these symbols from the host at runtime.
 */
#ifndef GNOSTR_PLUGIN_BUILD

/* ============================================================================
 * GNOSTR_PLUGIN INTERFACE
 * ============================================================================ */

G_DEFINE_INTERFACE(GnostrPlugin, gnostr_plugin, G_TYPE_OBJECT)

static void
gnostr_plugin_default_init(GnostrPluginInterface *iface)
{
  (void)iface;
  /* No default implementations - all methods are abstract */
}

void
gnostr_plugin_activate(GnostrPlugin *plugin, GnostrPluginContext *context)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN(plugin));

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->activate)
    iface->activate(plugin, context);
}

void
gnostr_plugin_deactivate(GnostrPlugin *plugin, GnostrPluginContext *context)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN(plugin));

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->deactivate)
    iface->deactivate(plugin, context);
}

const char *
gnostr_plugin_get_name(GnostrPlugin *plugin)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_name)
    return iface->get_name(plugin);
  return NULL;
}

const char *
gnostr_plugin_get_description(GnostrPlugin *plugin)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_description)
    return iface->get_description(plugin);
  return NULL;
}

const char *const *
gnostr_plugin_get_authors(GnostrPlugin *plugin)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_authors)
    return iface->get_authors(plugin);
  return NULL;
}

const char *
gnostr_plugin_get_version(GnostrPlugin *plugin)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_version)
    return iface->get_version(plugin);
  return NULL;
}

const int *
gnostr_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_supported_kinds)
    return iface->get_supported_kinds(plugin, n_kinds);

  if (n_kinds) *n_kinds = 0;
  return NULL;
}

/* ============================================================================
 * GNOSTR_EVENT_HANDLER INTERFACE
 * ============================================================================ */

G_DEFINE_INTERFACE(GnostrEventHandler, gnostr_event_handler, G_TYPE_OBJECT)

static void
gnostr_event_handler_default_init(GnostrEventHandlerInterface *iface)
{
  (void)iface;
}

gboolean
gnostr_event_handler_handle_event(GnostrEventHandler  *handler,
                                  GnostrPluginContext *context,
                                  GnostrPluginEvent   *event)
{
  g_return_val_if_fail(GNOSTR_IS_EVENT_HANDLER(handler), FALSE);

  GnostrEventHandlerInterface *iface = GNOSTR_EVENT_HANDLER_GET_IFACE(handler);
  if (iface->handle_event)
    return iface->handle_event(handler, context, event);
  return FALSE;
}

gboolean
gnostr_event_handler_can_handle_kind(GnostrEventHandler *handler, int kind)
{
  g_return_val_if_fail(GNOSTR_IS_EVENT_HANDLER(handler), FALSE);

  GnostrEventHandlerInterface *iface = GNOSTR_EVENT_HANDLER_GET_IFACE(handler);
  if (iface->can_handle_kind)
    return iface->can_handle_kind(handler, kind);
  return FALSE;
}

/* ============================================================================
 * GNOSTR_UI_EXTENSION INTERFACE
 * ============================================================================ */

G_DEFINE_INTERFACE(GnostrUIExtension, gnostr_ui_extension, G_TYPE_OBJECT)

static void
gnostr_ui_extension_default_init(GnostrUIExtensionInterface *iface)
{
  (void)iface;
}

GList *
gnostr_ui_extension_create_menu_items(GnostrUIExtension     *extension,
                                      GnostrPluginContext   *context,
                                      GnostrUIExtensionPoint point,
                                      gpointer               target_data)
{
  g_return_val_if_fail(GNOSTR_IS_UI_EXTENSION(extension), NULL);

  GnostrUIExtensionInterface *iface = GNOSTR_UI_EXTENSION_GET_IFACE(extension);
  if (iface->create_menu_items)
    return iface->create_menu_items(extension, context, point, target_data);
  return NULL;
}

GtkWidget *
gnostr_ui_extension_create_settings_page(GnostrUIExtension   *extension,
                                         GnostrPluginContext *context)
{
  g_return_val_if_fail(GNOSTR_IS_UI_EXTENSION(extension), NULL);

  GnostrUIExtensionInterface *iface = GNOSTR_UI_EXTENSION_GET_IFACE(extension);
  if (iface->create_settings_page)
    return iface->create_settings_page(extension, context);
  return NULL;
}

GtkWidget *
gnostr_ui_extension_create_note_decoration(GnostrUIExtension   *extension,
                                           GnostrPluginContext *context,
                                           GnostrPluginEvent   *event)
{
  g_return_val_if_fail(GNOSTR_IS_UI_EXTENSION(extension), NULL);

  GnostrUIExtensionInterface *iface = GNOSTR_UI_EXTENSION_GET_IFACE(extension);
  if (iface->create_note_decoration)
    return iface->create_note_decoration(extension, context, event);
  return NULL;
}

GList *
gnostr_ui_extension_get_sidebar_items(GnostrUIExtension   *extension,
                                      GnostrPluginContext *context)
{
  g_return_val_if_fail(GNOSTR_IS_UI_EXTENSION(extension), NULL);

  GnostrUIExtensionInterface *iface = GNOSTR_UI_EXTENSION_GET_IFACE(extension);
  if (iface->get_sidebar_items)
    return iface->get_sidebar_items(extension, context);
  return NULL;
}

GtkWidget *
gnostr_ui_extension_create_panel_widget(GnostrUIExtension   *extension,
                                        GnostrPluginContext *context,
                                        const char          *panel_id)
{
  g_return_val_if_fail(GNOSTR_IS_UI_EXTENSION(extension), NULL);
  g_return_val_if_fail(panel_id != NULL, NULL);

  GnostrUIExtensionInterface *iface = GNOSTR_UI_EXTENSION_GET_IFACE(extension);
  if (iface->create_panel_widget)
    return iface->create_panel_widget(extension, context, panel_id);
  return NULL;
}

/* ============================================================================
 * SIDEBAR ITEM FUNCTIONS
 * ============================================================================ */

GnostrSidebarItem *
gnostr_sidebar_item_new(const char *id,
                        const char *label,
                        const char *icon_name)
{
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(label != NULL, NULL);

  GnostrSidebarItem *item = g_new0(GnostrSidebarItem, 1);
  item->id = g_strdup(id);
  item->label = g_strdup(label);
  item->icon_name = g_strdup(icon_name);
  item->requires_auth = FALSE;
  item->position = 0;

  return item;
}

void
gnostr_sidebar_item_free(GnostrSidebarItem *item)
{
  if (!item) return;

  g_clear_pointer(&item->id, g_free);
  g_clear_pointer(&item->label, g_free);
  g_clear_pointer(&item->icon_name, g_free);
  g_free(item);
}

void
gnostr_sidebar_item_set_requires_auth(GnostrSidebarItem *item,
                                      gboolean           requires_auth)
{
  g_return_if_fail(item != NULL);
  item->requires_auth = requires_auth;
}

void
gnostr_sidebar_item_set_position(GnostrSidebarItem *item,
                                 int                position)
{
  g_return_if_fail(item != NULL);
  item->position = position;
}

/* ============================================================================
 * ERROR DOMAIN
 * ============================================================================ */

G_DEFINE_QUARK(gnostr-plugin-error-quark, gnostr_plugin_error)

#endif /* !GNOSTR_PLUGIN_BUILD - Interface implementations */

/* ============================================================================
 * PLUGIN CONTEXT API IMPLEMENTATIONS
 * These implementations use internal app services and are only built into
 * the main app, not into plugins.
 * ============================================================================ */

#ifndef GNOSTR_PLUGIN_BUILD

/* --- Application Access --- */

GtkApplication *
gnostr_plugin_context_get_application(GnostrPluginContext *context)
{
  g_return_val_if_fail(context != NULL, NULL);
  return context->app;
}

GtkWindow *
gnostr_plugin_context_get_main_window(GnostrPluginContext *context)
{
  g_return_val_if_fail(context != NULL, NULL);
  return context->main_window;
}

/* --- Repository Browser (NIP-34) --- */

void
gnostr_plugin_context_add_repository(GnostrPluginContext *context,
                                     const char          *id,
                                     const char          *name,
                                     const char          *description,
                                     const char          *clone_url,
                                     const char          *web_url,
                                     const char          *maintainer_pubkey,
                                     gint64               updated_at)
{
  g_return_if_fail(context != NULL);
  g_return_if_fail(id != NULL);

  if (!context->main_window)
    return;

  /* Get the repo browser from main window */
  if (!GNOSTR_IS_MAIN_WINDOW(context->main_window))
    return;

  GtkWidget *browser = gnostr_main_window_get_repo_browser(GNOSTR_MAIN_WINDOW(context->main_window));
  if (!browser || !GNOSTR_IS_REPO_BROWSER(browser))
    return;

  gnostr_repo_browser_add_repository(GNOSTR_REPO_BROWSER(browser),
                                     id, name, description,
                                     clone_url, web_url,
                                     maintainer_pubkey, updated_at);
}

void
gnostr_plugin_context_clear_repositories(GnostrPluginContext *context)
{
  g_return_if_fail(context != NULL);

  if (!context->main_window)
    return;

  if (!GNOSTR_IS_MAIN_WINDOW(context->main_window))
    return;

  GtkWidget *browser = gnostr_main_window_get_repo_browser(GNOSTR_MAIN_WINDOW(context->main_window));
  if (!browser || !GNOSTR_IS_REPO_BROWSER(browser))
    return;

  gnostr_repo_browser_clear(GNOSTR_REPO_BROWSER(browser));
}

/* --- Network Access --- */

GObject *
gnostr_plugin_context_get_pool(GnostrPluginContext *context)
{
  g_return_val_if_fail(context != NULL, NULL);
  if (context->pool)
    return G_OBJECT(context->pool);
  /* Fall back to shared query pool */
  return G_OBJECT(gnostr_get_shared_query_pool());
}

char **
gnostr_plugin_context_get_relay_urls(GnostrPluginContext *context, gsize *n_urls)
{
  g_return_val_if_fail(context != NULL, NULL);

  GPtrArray *relays = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relays);

  if (n_urls) *n_urls = relays->len;

  /* Convert to strv */
  g_ptr_array_add(relays, NULL);
  return (char **)g_ptr_array_free(relays, FALSE);
}

gboolean
gnostr_plugin_context_publish_event(GnostrPluginContext *context,
                                    const char          *event_json,
                                    GError             **error)
{
  g_return_val_if_fail(context != NULL, FALSE);
  g_return_val_if_fail(event_json != NULL, FALSE);

  /* Parse the signed event */
  NostrEvent *event = nostr_event_new();
  int rc = nostr_event_deserialize_compact(event, event_json, NULL);
  if (rc != 1) {
    g_set_error(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_INVALID_DATA,
                "Failed to parse event JSON");
    nostr_event_free(event);
    return FALSE;
  }

  /* Get write relay URLs */
  GPtrArray *relay_urls = gnostr_get_write_relay_urls();
  if (!relay_urls || relay_urls->len == 0) {
    g_set_error(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_NETWORK,
                "No write relays configured");
    if (relay_urls) g_ptr_array_unref(relay_urls);
    nostr_event_free(event);
    return FALSE;
  }

  guint success_count = 0;
  GError *last_error = NULL;

  for (guint i = 0; i < relay_urls->len; i++) {
    const char *url = g_ptr_array_index(relay_urls, i);
    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay) continue;

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_clear_error(&last_error);
      last_error = conn_err;
      g_object_unref(relay);
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, event, &pub_err)) {
      success_count++;
    } else {
      g_clear_error(&last_error);
      last_error = pub_err;
    }
    g_object_unref(relay);
  }

  g_ptr_array_unref(relay_urls);
  nostr_event_free(event);

  if (success_count == 0) {
    if (last_error) {
      g_propagate_error(error, last_error);
    } else {
      g_set_error(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_NETWORK,
                  "Failed to publish to any relay");
    }
    return FALSE;
  }

  g_clear_error(&last_error);
  return TRUE;
}

/* Async publish context */
typedef struct
{
  GnostrPluginContext *context;
  char *event_json;
  GTask *task;
} PublishAsyncData;

static void
publish_async_data_free(PublishAsyncData *data)
{
  if (!data) return;
  g_clear_pointer(&data->event_json, g_free);
  g_free(data);
}

static void
publish_async_thread_func(GTask        *task,
                          gpointer      source_object,
                          gpointer      task_data,
                          GCancellable *cancellable)
{
  (void)source_object;
  (void)cancellable;

  PublishAsyncData *data = task_data;
  GError *error = NULL;

  gboolean result = gnostr_plugin_context_publish_event(data->context,
                                                        data->event_json,
                                                        &error);
  if (result) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_error(task, error);
  }
}

void
gnostr_plugin_context_publish_event_async(GnostrPluginContext *context,
                                          const char          *event_json,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data)
{
  g_return_if_fail(context != NULL);
  g_return_if_fail(event_json != NULL);

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  PublishAsyncData *data = g_new0(PublishAsyncData, 1);
  data->context = context;
  data->event_json = g_strdup(event_json);

  g_task_set_task_data(task, data, (GDestroyNotify)publish_async_data_free);
  g_task_run_in_thread(task, publish_async_thread_func);
  g_object_unref(task);
}

gboolean
gnostr_plugin_context_publish_event_finish(GnostrPluginContext *context,
                                           GAsyncResult        *result,
                                           GError             **error)
{
  (void)context;
  g_return_val_if_fail(G_IS_TASK(result), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* --- Relay Event Request --- */

static void
on_request_relay_events_done(GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  GTask *task = G_TASK(user_data);
  GError *error = NULL;

  /* query_single returns JSON strings - we need to ingest them to nostrdb
   * so that plugin subscriptions can fire. */
  GPtrArray *events = gnostr_pool_query_finish(
      GNOSTR_POOL(source), res, &error);

  if (error) {
    g_debug("[plugin-api] Relay request failed: %s", error->message);
    g_task_return_error(task, error);
  } else {
    guint count = events ? events->len : 0;
    guint ingested = 0;

    /* Ingest event JSON strings to nostrdb */
    for (guint i = 0; events && i < events->len; i++) {
      const char *evt_json = g_ptr_array_index(events, i);
      if (!evt_json) continue;

      int rc = storage_ndb_ingest_event_json(evt_json, NULL);
      if (rc == 0) {
        ingested++;
      }
    }

    g_debug("[plugin-api] Relay request completed: %u events fetched, %u ingested",
            count, ingested);
    g_task_return_boolean(task, TRUE);

    if (events) {
      g_ptr_array_unref(events);
    }
  }

  g_object_unref(task);
}

void
gnostr_plugin_context_request_relay_events_async(GnostrPluginContext *context,
                                                 const int           *kinds,
                                                 gsize                n_kinds,
                                                 int                  limit,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data)
{
  g_return_if_fail(context != NULL);
  g_return_if_fail(kinds != NULL && n_kinds > 0);

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  static gint _plugin_qf_counter = 0;

  /* Get pool and relay URLs */
  GNostrPool *pool = context->pool;
  if (!pool) {
    pool = gnostr_get_shared_query_pool();
  }
  if (!pool) {
    g_task_return_new_error(task, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_NETWORK,
                            "No relay pool available");
    g_object_unref(task);
    return;
  }

  /* Get read relay URLs */
  GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_get_read_relay_urls_into(relay_arr);

  if (relay_arr->len == 0) {
    g_ptr_array_unref(relay_arr);
    g_task_return_new_error(task, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_NETWORK,
                            "No read relays configured");
    g_object_unref(task);
    return;
  }

  /* Build URL array for pool API */
  const char **urls = g_new0(const char *, relay_arr->len + 1);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }
  size_t url_count = relay_arr->len;

  /* Build filter */
  NostrFilter *filter = nostr_filter_new();
  nostr_filter_set_kinds(filter, kinds, n_kinds);
  if (limit > 0) {
    nostr_filter_set_limit(filter, limit);
  }

  g_debug("[plugin-api] Requesting events from %zu relays, kinds=%d... limit=%d",
          url_count, kinds[0], limit);

  /* Use query_async for one-shot query - waits for EOSE on all relays
   * then returns all events. We ingest them to nostrdb in the callback
   * so plugin subscriptions can fire. */
  gnostr_pool_sync_relays(pool, (const gchar **)urls, url_count);
  {
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    int _qfid = g_atomic_int_add(&_plugin_qf_counter, 1);
    char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-%d", _qfid);
    g_object_set_data_full(G_OBJECT(pool), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
    gnostr_pool_query_async(pool, _qf, cancellable,
                            on_request_relay_events_done, task);
  }

  /* Cleanup - pool copies what it needs */
  nostr_filter_free(filter);
  g_free(urls);
  g_ptr_array_unref(relay_arr);
}

gboolean
gnostr_plugin_context_request_relay_events_finish(GnostrPluginContext *context,
                                                  GAsyncResult        *result,
                                                  GError             **error)
{
  (void)context;
  g_return_val_if_fail(G_IS_TASK(result), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* --- Storage Access --- */

GPtrArray *
gnostr_plugin_context_query_events(GnostrPluginContext *context,
                                   const char          *filter_json,
                                   GError             **error)
{
  g_return_val_if_fail(context != NULL, NULL);
  g_return_val_if_fail(filter_json != NULL, NULL);

  void *txn = NULL;
  int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
  if (rc != 0 || !txn) {
    g_set_error(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_STORAGE,
                "Failed to begin storage query");
    return NULL;
  }

  char **results = NULL;
  int count = 0;
  rc = storage_ndb_query(txn, filter_json, &results, &count);
  storage_ndb_end_query(txn);

  if (rc != 0) {
    g_set_error(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_STORAGE,
                "Query failed");
    return NULL;
  }

  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  for (int i = 0; i < count; i++) {
    if (results[i]) {
      g_ptr_array_add(arr, g_strdup(results[i]));
    }
  }

  if (results) {
    storage_ndb_free_results(results, count);
  }

  return arr;
}

char *
gnostr_plugin_context_get_event_by_id(GnostrPluginContext *context,
                                      const char          *event_id_hex,
                                      GError             **error)
{
  g_return_val_if_fail(context != NULL, NULL);
  g_return_val_if_fail(event_id_hex != NULL, NULL);

  if (strlen(event_id_hex) != 64) {
    g_set_error(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_INVALID_DATA,
                "Event ID must be 64 hex characters");
    return NULL;
  }

  char *json_out = NULL;
  int json_len = 0;
  int rc = storage_ndb_get_note_by_id_nontxn(event_id_hex, &json_out, &json_len);

  if (rc != 0 || !json_out) {
    /* Event not found is not an error */
    return NULL;
  }

  /* storage_ndb_get_note_by_id_nontxn returns internal storage, need to copy */
  return g_strndup(json_out, json_len);
}

/* Dispatcher callback adapter - converts note keys to JSON and calls plugin callback */
static void
plugin_subscription_dispatch(uint64_t             subid,
                             const uint64_t      *note_keys,
                             guint                n_keys,
                             gpointer             user_data)
{
  PluginSubscription *sub = (PluginSubscription *)user_data;
  if (!sub || !sub->callback)
    return;

  g_debug("[plugin-api] Subscription %lu received %u events", (unsigned long)subid, n_keys);

  /* Cast callback to the expected signature */
  typedef void (*PluginEventCallback)(const char *event_json, gpointer user_data);
  PluginEventCallback plugin_cb = (PluginEventCallback)sub->callback;

  /* Process each note key */
  for (guint i = 0; i < n_keys; i++) {
    uint64_t key = note_keys[i];

    /* Fetch JSON for this note key */
    char *json_out = NULL;
    int json_len = 0;
    int rc = storage_ndb_get_note_json_by_key(key, &json_out, &json_len);

    if (rc == 0 && json_out && json_len > 0) {
      char *json_copy = g_strndup(json_out, json_len);
      plugin_cb(json_copy, sub->user_data);
      g_free(json_copy);
    } else {
      g_debug("[plugin-api] Failed to fetch JSON for note key %lu", (unsigned long)key);
    }
  }
}

guint64
gnostr_plugin_context_subscribe_events(GnostrPluginContext *context,
                                       const char          *filter_json,
                                       GCallback            callback,
                                       gpointer             user_data,
                                       GDestroyNotify       destroy_notify)
{
  g_return_val_if_fail(context != NULL, 0);
  g_return_val_if_fail(filter_json != NULL, 0);
  g_return_val_if_fail(callback != NULL, 0);

  /* Create subscription struct first so we can pass it to the dispatcher */
  PluginSubscription *sub = g_new0(PluginSubscription, 1);
  sub->id = context->next_sub_id++;
  sub->filter_json = g_strdup(filter_json);
  sub->callback = callback;
  sub->user_data = user_data;
  sub->destroy_notify = destroy_notify;

  /* Use the dispatcher for proper callback invocation.
   * The dispatcher calls our adapter, which then calls the plugin callback. */
  uint64_t ndb_sub = gn_ndb_subscribe(filter_json,
                                       plugin_subscription_dispatch,
                                       sub,
                                       NULL);  /* We handle cleanup in unsubscribe */
  if (ndb_sub == 0) {
    g_warning("[plugin-api] Failed to create storage subscription");
    g_free(sub->filter_json);
    g_free(sub);
    return 0;
  }

  sub->ndb_sub_id = ndb_sub;
  g_hash_table_insert(context->subscriptions, &sub->id, sub);

  g_debug("[plugin-api] Created subscription %lu (ndb=%lu) for filter: %s",
          (unsigned long)sub->id, (unsigned long)ndb_sub, filter_json);

  return sub->id;
}

void
gnostr_plugin_context_unsubscribe_events(GnostrPluginContext *context,
                                         guint64              subscription_id)
{
  g_return_if_fail(context != NULL);

  g_hash_table_remove(context->subscriptions, &subscription_id);
}

/* --- Plugin Data Storage --- */

static char *
plugin_data_path(GnostrPluginContext *context, const char *key)
{
  const char *data_dir = g_get_user_data_dir();
  return g_build_filename(data_dir, "gnostr", "plugins",
                          context->plugin_id, key, NULL);
}

gboolean
gnostr_plugin_context_store_data(GnostrPluginContext *context,
                                 const char          *key,
                                 GBytes              *data,
                                 GError             **error)
{
  g_return_val_if_fail(context != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(data != NULL, FALSE);

  g_autofree char *path = plugin_data_path(context, key);
  g_autofree char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);

  gsize len;
  gconstpointer bytes = g_bytes_get_data(data, &len);

  return g_file_set_contents(path, bytes, len, error);
}

GBytes *
gnostr_plugin_context_load_data(GnostrPluginContext *context,
                                const char          *key,
                                GError             **error)
{
  g_return_val_if_fail(context != NULL, NULL);
  g_return_val_if_fail(key != NULL, NULL);

  g_autofree char *path = plugin_data_path(context, key);
  char *contents = NULL;
  gsize len = 0;

  if (!g_file_get_contents(path, &contents, &len, error)) {
    return NULL;
  }

  return g_bytes_new_take(contents, len);
}

gboolean
gnostr_plugin_context_delete_data(GnostrPluginContext *context,
                                  const char          *key)
{
  g_return_val_if_fail(context != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);

  g_autofree char *path = plugin_data_path(context, key);
  g_autoptr(GFile) file = g_file_new_for_path(path);
  GError *error = NULL;
  gboolean ok = g_file_delete(file, NULL, &error);
  if (!ok && error) {
    /* G_IO_ERROR_NOT_FOUND is expected if file doesn't exist */
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_warning("plugin: failed to delete data file %s: %s", path, error->message);
    }
    g_clear_error(&error);
  }
  return ok;
}

/* --- Settings Access --- */

GSettings *
gnostr_plugin_context_get_settings(GnostrPluginContext *context,
                                   const char          *schema_id)
{
  g_return_val_if_fail(context != NULL, NULL);
  g_return_val_if_fail(schema_id != NULL, NULL);

  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) return NULL;

  GSettingsSchema *schema = g_settings_schema_source_lookup(source, schema_id, TRUE);
  if (!schema) return NULL;

  GSettings *settings = g_settings_new(schema_id);
  g_settings_schema_unref(schema);

  return settings;
}

/* --- User Identity --- */

const char *
gnostr_plugin_context_get_user_pubkey(GnostrPluginContext *context)
{
  g_return_val_if_fail(context != NULL, NULL);

  GnostrSignerService *signer = gnostr_signer_service_get_default();
  return gnostr_signer_service_get_pubkey(signer);
}

gboolean
gnostr_plugin_context_is_logged_in(GnostrPluginContext *context)
{
  g_return_val_if_fail(context != NULL, FALSE);

  GnostrSignerService *signer = gnostr_signer_service_get_default();
  return gnostr_signer_service_is_available(signer);
}

void
gnostr_plugin_context_open_profile_panel(GnostrPluginContext *context,
                                         const char          *pubkey_hex)
{
  g_return_if_fail(context != NULL);
  g_return_if_fail(pubkey_hex != NULL);

  if (!context->main_window)
    return;

  if (!GNOSTR_IS_MAIN_WINDOW(context->main_window))
    return;

  g_debug("[plugin-api] Opening profile panel for pubkey: %s", pubkey_hex);

  /* Navigate to the profile panel */
  gnostr_main_window_open_profile(GTK_WIDGET(context->main_window), pubkey_hex);
}

/* Async sign context */
typedef struct
{
  GTask *task;
} SignAsyncData;

static void
on_sign_complete(GnostrSignerService *service,
                 const char          *signed_event_json,
                 GError              *error,
                 gpointer             user_data)
{
  (void)service;
  GTask *task = G_TASK(user_data);

  if (error) {
    g_task_return_error(task, g_error_copy(error));
  } else if (signed_event_json) {
    g_task_return_pointer(task, g_strdup(signed_event_json), g_free);
  } else {
    g_task_return_new_error(task, GNOSTR_PLUGIN_ERROR,
                            GNOSTR_PLUGIN_ERROR_SIGNER_REFUSED,
                            "Signer returned no result");
  }
  g_object_unref(task);
}

void
gnostr_plugin_context_request_sign_event(GnostrPluginContext *context,
                                         const char          *unsigned_event_json,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  g_return_if_fail(context != NULL);
  g_return_if_fail(unsigned_event_json != NULL);

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    g_task_return_new_error(task, GNOSTR_PLUGIN_ERROR,
                            GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN,
                            "Signer not available");
    g_object_unref(task);
    return;
  }

  gnostr_signer_service_sign_event_async(signer, unsigned_event_json,
                                          cancellable, on_sign_complete, task);
}

char *
gnostr_plugin_context_request_sign_event_finish(GnostrPluginContext *context,
                                                GAsyncResult        *result,
                                                GError             **error)
{
  (void)context;
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ============================================================================
 * ACTION HANDLERS
 * ============================================================================ */

void
gnostr_plugin_context_register_action(GnostrPluginContext    *context,
                                       const char             *action_name,
                                       GnostrPluginActionFunc  callback,
                                       gpointer                user_data)
{
  g_return_if_fail(context != NULL);
  g_return_if_fail(action_name != NULL);
  g_return_if_fail(callback != NULL);

  PluginAction *action = g_new0(PluginAction, 1);
  action->name = g_strdup(action_name);
  action->callback = callback;
  action->user_data = user_data;

  g_hash_table_replace(context->actions, action->name, action);

  g_debug("[plugin-api] Plugin '%s' registered action '%s'",
          context->plugin_id, action_name);
}

void
gnostr_plugin_context_unregister_action(GnostrPluginContext *context,
                                         const char          *action_name)
{
  g_return_if_fail(context != NULL);
  g_return_if_fail(action_name != NULL);

  if (g_hash_table_remove(context->actions, action_name)) {
    g_debug("[plugin-api] Plugin '%s' unregistered action '%s'",
            context->plugin_id, action_name);
  }
}

gboolean
gnostr_plugin_context_dispatch_action(GnostrPluginContext *context,
                                       const char          *action_name,
                                       GVariant            *parameter)
{
  g_return_val_if_fail(context != NULL, FALSE);
  g_return_val_if_fail(action_name != NULL, FALSE);

  PluginAction *action = g_hash_table_lookup(context->actions, action_name);
  if (!action) {
    g_debug("[plugin-api] Plugin '%s' has no action '%s'",
            context->plugin_id, action_name);
    return FALSE;
  }

  g_debug("[plugin-api] Dispatching action '%s' to plugin '%s'",
          action_name, context->plugin_id);

  action->callback(context, action_name, parameter, action->user_data);
  return TRUE;
}

/* ============================================================================
 * PLUGIN EVENT WRAPPER
 * ============================================================================
 * GnostrPluginEvent wraps the internal NostrEvent for plugin access.
 */

struct _GnostrPluginEvent
{
  NostrEvent *event;  /* Borrowed reference - caller owns lifecycle */
};

GnostrPluginEvent *
gnostr_plugin_event_wrap(NostrEvent *event)
{
  if (!event) return NULL;

  GnostrPluginEvent *wrapper = g_new0(GnostrPluginEvent, 1);
  wrapper->event = event;
  return wrapper;
}

void
gnostr_plugin_event_free(GnostrPluginEvent *event)
{
  if (event) {
    /* Note: we don't free event->event as it's borrowed */
    g_free(event);
  }
}

const char *
gnostr_plugin_event_get_id(GnostrPluginEvent *event)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, NULL);
  return event->event->id;
}

const char *
gnostr_plugin_event_get_pubkey(GnostrPluginEvent *event)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, NULL);
  return nostr_event_get_pubkey(event->event);
}

gint64
gnostr_plugin_event_get_created_at(GnostrPluginEvent *event)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, 0);
  return nostr_event_get_created_at(event->event);
}

int
gnostr_plugin_event_get_kind(GnostrPluginEvent *event)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, 0);
  return nostr_event_get_kind(event->event);
}

const char *
gnostr_plugin_event_get_content(GnostrPluginEvent *event)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, NULL);
  return nostr_event_get_content(event->event);
}

const char *
gnostr_plugin_event_get_sig(GnostrPluginEvent *event)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, NULL);
  return nostr_event_get_sig(event->event);
}

char *
gnostr_plugin_event_get_tags_json(GnostrPluginEvent *event)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, NULL);

  NostrTags *tags = event->event->tags;
  if (!tags || nostr_tags_size(tags) == 0) {
    return g_strdup("[]");
  }

  /* Build JSON array of tags */
  GString *json = g_string_new("[");
  size_t count = nostr_tags_size(tags);

  for (size_t i = 0; i < count; i++) {
    if (i > 0) g_string_append_c(json, ',');

    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag) continue;

    g_string_append_c(json, '[');
    size_t field_count = nostr_tag_size(tag);
    for (size_t j = 0; j < field_count; j++) {
      if (j > 0) g_string_append_c(json, ',');
      const char *field = nostr_tag_get(tag, j);

      /* JSON-escape the string */
      g_string_append_c(json, '"');
      if (field) {
        for (const char *p = field; *p; p++) {
          switch (*p) {
            case '"':  g_string_append(json, "\\\""); break;
            case '\\': g_string_append(json, "\\\\"); break;
            case '\n': g_string_append(json, "\\n"); break;
            case '\r': g_string_append(json, "\\r"); break;
            case '\t': g_string_append(json, "\\t"); break;
            default:   g_string_append_c(json, *p); break;
          }
        }
      }
      g_string_append_c(json, '"');
    }
    g_string_append_c(json, ']');
  }

  g_string_append_c(json, ']');
  return g_string_free(json, FALSE);
}

const char *
gnostr_plugin_event_get_tag_value(GnostrPluginEvent *event,
                                  const char        *tag_name,
                                  guint              index)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, NULL);
  g_return_val_if_fail(tag_name != NULL, NULL);

  NostrTags *tags = event->event->tags;
  if (!tags) return NULL;

  guint match_count = 0;
  size_t count = nostr_tags_size(tags);

  for (size_t i = 0; i < count; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 1) continue;

    const char *name = nostr_tag_get(tag, 0);
    if (name && g_strcmp0(name, tag_name) == 0) {
      if (match_count == index) {
        /* Return the first value (index 1) */
        if (nostr_tag_size(tag) > 1) {
          return nostr_tag_get(tag, 1);
        }
        return NULL;
      }
      match_count++;
    }
  }

  return NULL;
}

char **
gnostr_plugin_event_get_tag_values(GnostrPluginEvent *event,
                                   const char        *tag_name)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, NULL);
  g_return_val_if_fail(tag_name != NULL, NULL);

  NostrTags *tags = event->event->tags;
  if (!tags) return NULL;

  GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
  size_t count = nostr_tags_size(tags);

  for (size_t i = 0; i < count; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2) continue;

    const char *name = nostr_tag_get(tag, 0);
    if (name && g_strcmp0(name, tag_name) == 0) {
      const char *value = nostr_tag_get(tag, 1);
      if (value) {
        g_ptr_array_add(values, g_strdup(value));
      }
    }
  }

  if (values->len == 0) {
    g_ptr_array_unref(values);
    return NULL;
  }

  g_ptr_array_add(values, NULL);
  return (char **)g_ptr_array_free(values, FALSE);
}

char *
gnostr_plugin_event_to_json(GnostrPluginEvent *event)
{
  g_return_val_if_fail(event != NULL && event->event != NULL, NULL);

  /* Use the fast-path compact serializer first */
  char *json = nostr_event_serialize_compact(event->event);
  if (json) {
    return json;  /* Already allocated, caller must free */
  }

  /* Fallback: use standard serializer */
  return nostr_event_serialize(event->event);
}

#endif /* !GNOSTR_PLUGIN_BUILD */
