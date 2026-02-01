/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-api.c - Plugin API implementation
 *
 * Implementation of GObject interfaces for the plugin system.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gnostr-plugin-api.h"
#include "storage_ndb.h"
#include "util/relays.h"
#include "ipc/gnostr-signer-service.h"
#include "util/utils.h"
#include "nostr_simple_pool.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-json.h"
#include <json-glib/json-glib.h>
#include <string.h>

/* ============================================================================
 * PLUGIN CONTEXT STRUCTURE
 * ============================================================================ */

struct _GnostrPluginContext
{
  GtkApplication *app;
  GtkWindow      *main_window;
  GnostrSimplePool *pool;
  char           *plugin_id;  /* For namespacing plugin data */

  /* Event subscriptions */
  GHashTable     *subscriptions;  /* subid (guint64) -> PluginSubscription* */
  guint64         next_sub_id;
};

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
  g_free(sub->filter_json);
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
  ctx->next_sub_id = 1;
  return ctx;
}

void
gnostr_plugin_context_free(GnostrPluginContext *ctx)
{
  if (!ctx) return;
  g_hash_table_unref(ctx->subscriptions);
  g_free(ctx->plugin_id);
  g_free(ctx);
}

void
gnostr_plugin_context_set_main_window(GnostrPluginContext *ctx, GtkWindow *window)
{
  if (ctx) ctx->main_window = window;
}

void
gnostr_plugin_context_set_pool(GnostrPluginContext *ctx, GnostrSimplePool *pool)
{
  if (ctx) ctx->pool = pool;
}

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

/* ============================================================================
 * ERROR DOMAIN
 * ============================================================================ */

G_DEFINE_QUARK(gnostr-plugin-error-quark, gnostr_plugin_error)

/* ============================================================================
 * PLUGIN CONTEXT API IMPLEMENTATIONS
 * ============================================================================ */

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
  int rc = nostr_event_deserialize_compact(event, event_json);
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
  g_free(data->event_json);
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

  /* Create nostrdb subscription */
  uint64_t ndb_sub = storage_ndb_subscribe(filter_json);
  if (ndb_sub == 0) {
    g_warning("Failed to create storage subscription");
    return 0;
  }

  PluginSubscription *sub = g_new0(PluginSubscription, 1);
  sub->id = context->next_sub_id++;
  sub->ndb_sub_id = ndb_sub;
  sub->filter_json = g_strdup(filter_json);
  sub->callback = callback;
  sub->user_data = user_data;
  sub->destroy_notify = destroy_notify;

  g_hash_table_insert(context->subscriptions, &sub->id, sub);

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

  char *path = plugin_data_path(context, key);
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);

  gsize len;
  gconstpointer bytes = g_bytes_get_data(data, &len);

  gboolean ok = g_file_set_contents(path, bytes, len, error);
  g_free(path);

  return ok;
}

GBytes *
gnostr_plugin_context_load_data(GnostrPluginContext *context,
                                const char          *key,
                                GError             **error)
{
  g_return_val_if_fail(context != NULL, NULL);
  g_return_val_if_fail(key != NULL, NULL);

  char *path = plugin_data_path(context, key);
  char *contents = NULL;
  gsize len = 0;

  if (!g_file_get_contents(path, &contents, &len, error)) {
    g_free(path);
    return NULL;
  }

  g_free(path);
  return g_bytes_new_take(contents, len);
}

gboolean
gnostr_plugin_context_delete_data(GnostrPluginContext *context,
                                  const char          *key)
{
  g_return_val_if_fail(context != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);

  char *path = plugin_data_path(context, key);
  GFile *file = g_file_new_for_path(path);
  gboolean ok = g_file_delete(file, NULL, NULL);
  g_object_unref(file);
  g_free(path);

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
