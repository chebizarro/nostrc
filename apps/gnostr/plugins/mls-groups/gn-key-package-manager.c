/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-key-package-manager.c - MLS Key Package Lifecycle Manager
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-key-package-manager.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>

struct _GnKeyPackageManager
{
  GObject parent_instance;

  GnMarmotService     *service;     /* strong ref */
  GnostrPluginContext *context;     /* borrowed — only accessed on main thread */

  /* Cancellable for in-flight async operations */
  GCancellable *cancellable;

  /* Last published key package reference (for rotation tracking) */
  gchar *last_kp_event_id;

  /* Timestamp of last successful key package publish (monotonic, microseconds) */
  gint64 last_kp_publish_time;

  /* Auto-rotation source ID (0 if inactive) */
  guint rotation_source_id;
};

G_DEFINE_TYPE(GnKeyPackageManager, gn_key_package_manager, G_TYPE_OBJECT)

/* Key package rotation interval: 24 hours */
#define KP_ROTATION_INTERVAL_SECS (24 * 60 * 60)

static void
gn_key_package_manager_dispose(GObject *object)
{
  GnKeyPackageManager *self = GN_KEY_PACKAGE_MANAGER(object);

  if (self->rotation_source_id > 0)
    {
      g_source_remove(self->rotation_source_id);
      self->rotation_source_id = 0;
    }

  /* Cancel any in-flight async operations */
  g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);

  g_clear_object(&self->service);
  self->context = NULL;

  G_OBJECT_CLASS(gn_key_package_manager_parent_class)->dispose(object);
}

static void
gn_key_package_manager_finalize(GObject *object)
{
  GnKeyPackageManager *self = GN_KEY_PACKAGE_MANAGER(object);

  g_clear_pointer(&self->last_kp_event_id, g_free);
  self->last_kp_publish_time = 0;

  G_OBJECT_CLASS(gn_key_package_manager_parent_class)->finalize(object);
}

static void
gn_key_package_manager_class_init(GnKeyPackageManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose  = gn_key_package_manager_dispose;
  object_class->finalize = gn_key_package_manager_finalize;
}

static void
gn_key_package_manager_init(GnKeyPackageManager *self)
{
  self->service            = NULL;
  self->context            = NULL;
  self->cancellable          = g_cancellable_new();
  self->last_kp_event_id     = NULL;
  self->last_kp_publish_time = 0;
  self->rotation_source_id   = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Internal: Key Package Creation Flow
 *
 * 1. marmot_gobject_client_create_key_package_unsigned_async() → unsigned kind:443 event JSON
 * 2. gnostr_plugin_context_request_sign_event() → signed event JSON
 * 3. gnostr_plugin_context_publish_event_async() → publish to relays
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  GnKeyPackageManager *manager;   /* strong ref */
  GTask               *task;
} CreateKpData;

static void
create_kp_data_free(CreateKpData *data)
{
  g_clear_object(&data->manager);
  g_free(data);
}

static void
on_kp_published(GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  CreateKpData *data = user_data;
  g_autoptr(GError) error = NULL;

  /* Validate the manager is still alive and not cancelled */
  if (data->manager->context == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Plugin deactivated");
      g_object_unref(data->task);
      create_kp_data_free(data);
      return;
    }

  gboolean ok = gnostr_plugin_context_publish_event_finish(
    data->manager->context, result, &error);

  if (ok)
    {
      g_info("KeyPackageManager: key package published successfully");
      data->manager->last_kp_publish_time = g_get_monotonic_time();
      g_task_return_boolean(data->task, TRUE);
    }
  else
    {
      g_warning("KeyPackageManager: failed to publish key package: %s",
                error ? error->message : "unknown");
      g_task_return_error(data->task, g_steal_pointer(&error));
    }

  g_object_unref(data->task);
  create_kp_data_free(data);
}

static void
on_kp_signed(GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  CreateKpData *data = user_data;
  g_autoptr(GError) error = NULL;

  if (data->manager->context == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Plugin deactivated");
      g_object_unref(data->task);
      create_kp_data_free(data);
      return;
    }

  g_autofree gchar *signed_json =
    gnostr_plugin_context_request_sign_event_finish(
      data->manager->context, result, &error);

  if (signed_json == NULL)
    {
      g_warning("KeyPackageManager: signer refused key package: %s",
                error ? error->message : "unknown");
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      create_kp_data_free(data);
      return;
    }

  g_info("KeyPackageManager: key package signed, publishing…");

  /* Step 3: Publish the signed event */
  gnostr_plugin_context_publish_event_async(
    data->manager->context,
    signed_json,
    g_task_get_cancellable(data->task),
    on_kp_published,
    data);
}

static void
on_kp_created(GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
  CreateKpData *data = user_data;
  MarmotGobjectClient *client = MARMOT_GOBJECT_CLIENT(source);
  g_autoptr(GError) error = NULL;

  g_autofree gchar *unsigned_json =
    marmot_gobject_client_create_key_package_unsigned_finish(client, result, &error);

  if (unsigned_json == NULL)
    {
      g_warning("KeyPackageManager: failed to create key package: %s",
                error ? error->message : "unknown");
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      create_kp_data_free(data);
      return;
    }

  if (data->manager->context == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Plugin deactivated");
      g_object_unref(data->task);
      create_kp_data_free(data);
      return;
    }

  g_info("KeyPackageManager: key package created, requesting signature…");

  /* Step 2: Sign the event via D-Bus signer */
  gnostr_plugin_context_request_sign_event(
    data->manager->context,
    unsigned_json,
    g_task_get_cancellable(data->task),
    on_kp_signed,
    data);
}

static void
create_and_publish_key_package(GnKeyPackageManager *self,
                               GTask               *task)
{
  GnMarmotService *service = self->service;
  if (service == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "Marmot service not available");
      g_object_unref(task);
      return;
    }

  const gchar *pubkey = gn_marmot_service_get_user_pubkey_hex(service);
  if (pubkey == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "User identity not set");
      g_object_unref(task);
      return;
    }

  /* Get user's relay URLs for the key package tags */
  gsize n_relays = 0;
  g_auto(GStrv) relay_urls =
    gnostr_plugin_context_get_relay_urls(self->context, &n_relays);

  CreateKpData *data = g_new0(CreateKpData, 1);
  data->manager = g_object_ref(self);   /* strong ref for async safety */
  data->task    = task; /* takes ownership */

  MarmotGobjectClient *client = gn_marmot_service_get_client(service);

  /*
   * Step 1: Create the key package via marmot (unsigned variant).
   *
   * We use the _unsigned API because the signer service owns the
   * private key. The returned event will be signed in step 2 via
   * gnostr_plugin_context_request_sign_event().
   */
  marmot_gobject_client_create_key_package_unsigned_async(
    client,
    pubkey,
    (const gchar * const *)relay_urls,
    g_task_get_cancellable(task),
    on_kp_created,
    data);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Auto-rotation timer
 * ══════════════════════════════════════════════════════════════════════════ */

static gboolean
on_rotation_timer(gpointer user_data)
{
  GnKeyPackageManager *self = GN_KEY_PACKAGE_MANAGER(user_data);

  g_info("KeyPackageManager: auto-rotating key package");

  gn_key_package_manager_rotate_async(self, NULL, NULL, NULL);

  return G_SOURCE_CONTINUE;
}

static void
start_auto_rotation(GnKeyPackageManager *self)
{
  if (self->rotation_source_id > 0)
    return; /* already running */

  self->rotation_source_id = g_timeout_add_seconds(
    KP_ROTATION_INTERVAL_SECS,
    on_rotation_timer,
    self);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

GnKeyPackageManager *
gn_key_package_manager_new(GnMarmotService     *service,
                            GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(plugin_context != NULL, NULL);

  GnKeyPackageManager *self = g_object_new(GN_TYPE_KEY_PACKAGE_MANAGER, NULL);
  self->service = g_object_ref(service);   /* strong ref */
  self->context = plugin_context; /* borrowed — valid for plugin lifetime */

  /* Start auto-rotation */
  start_auto_rotation(self);

  return self;
}

void
gn_key_package_manager_ensure_key_package_async(GnKeyPackageManager *self,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data)
{
  g_return_if_fail(GN_IS_KEY_PACKAGE_MANAGER(self));

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_key_package_manager_ensure_key_package_async);

  /*
   * Skip if we published a key package recently (within the rotation
   * interval). This avoids unnecessary relay bandwidth on repeated
   * ensure calls (e.g. app restart, re-login).
   */
  if (self->last_kp_publish_time > 0)
    {
      gint64 elapsed_secs = (g_get_monotonic_time() - self->last_kp_publish_time)
                            / G_USEC_PER_SEC;
      if (elapsed_secs < KP_ROTATION_INTERVAL_SECS)
        {
          g_info("KeyPackageManager: valid key package published %"G_GINT64_FORMAT
                 "s ago, skipping", elapsed_secs);
          g_task_return_boolean(task, TRUE);
          g_object_unref(task);
          return;
        }
    }

  g_info("KeyPackageManager: ensuring key package exists");
  create_and_publish_key_package(self, task);
}

gboolean
gn_key_package_manager_ensure_key_package_finish(GnKeyPackageManager *self,
                                                  GAsyncResult        *result,
                                                  GError             **error)
{
  g_return_val_if_fail(GN_IS_KEY_PACKAGE_MANAGER(self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
gn_key_package_manager_rotate_async(GnKeyPackageManager *self,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  g_return_if_fail(GN_IS_KEY_PACKAGE_MANAGER(self));

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_key_package_manager_rotate_async);

  g_info("KeyPackageManager: rotating key package");
  create_and_publish_key_package(self, task);
}

gboolean
gn_key_package_manager_rotate_finish(GnKeyPackageManager *self,
                                      GAsyncResult        *result,
                                      GError             **error)
{
  g_return_val_if_fail(GN_IS_KEY_PACKAGE_MANAGER(self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
gn_key_package_manager_publish_relay_list_async(GnKeyPackageManager  *self,
                                                 const gchar * const  *relay_urls,
                                                 GCancellable         *cancellable,
                                                 GAsyncReadyCallback   callback,
                                                 gpointer              user_data)
{
  g_return_if_fail(GN_IS_KEY_PACKAGE_MANAGER(self));

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_key_package_manager_publish_relay_list_async);

  g_info("KeyPackageManager: publishing key package relay list (kind:10051)");

  if (relay_urls == NULL || relay_urls[0] == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "No relay URLs provided");
      g_object_unref(task);
      return;
    }

  const gchar *pubkey = gnostr_plugin_context_get_user_pubkey(self->context);
  if (pubkey == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "User identity not set");
      g_object_unref(task);
      return;
    }

  /*
   * Build a kind:10051 event (MLS Key Package Relay List):
   *   {
   *     "pubkey": "<hex>",
   *     "kind": 10051,
   *     "created_at": <now>,
   *     "tags": [["relay","wss://..."], ["relay","wss://..."]],
   *     "content": ""
   *   }
   */
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "pubkey");
  json_builder_add_string_value(builder, pubkey);

  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, 10051);

  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, g_get_real_time() / G_USEC_PER_SEC);

  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);
  for (gsize i = 0; relay_urls[i] != NULL; i++)
    {
      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "relay");
      json_builder_add_string_value(builder, relay_urls[i]);
      json_builder_end_array(builder);
    }
  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, "");

  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  g_autofree gchar *unsigned_json = json_generator_to_data(gen, NULL);

  /* Reuse the CreateKpData + sign → publish chain */
  CreateKpData *data = g_new0(CreateKpData, 1);
  data->manager = g_object_ref(self);
  data->task    = task;

  gnostr_plugin_context_request_sign_event(
    self->context,
    unsigned_json,
    cancellable,
    on_kp_signed,    /* same sign → publish chain as key packages */
    data);
}

gboolean
gn_key_package_manager_publish_relay_list_finish(GnKeyPackageManager *self,
                                                  GAsyncResult        *result,
                                                  GError             **error)
{
  g_return_val_if_fail(GN_IS_KEY_PACKAGE_MANAGER(self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
