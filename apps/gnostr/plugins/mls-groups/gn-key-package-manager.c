/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-key-package-manager.c - MLS Key Package Lifecycle Manager
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-key-package-manager.h"
#include <gnostr-plugin-api.h>

struct _GnKeyPackageManager
{
  GObject parent_instance;

  GnMarmotService     *service;     /* weak ref */
  GnostrPluginContext *context;     /* borrowed */

  /* Last published key package reference (for rotation tracking) */
  gchar *last_kp_event_id;

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

  /* Don't unref service — we hold a weak reference */
  self->service = NULL;
  self->context = NULL;

  G_OBJECT_CLASS(gn_key_package_manager_parent_class)->dispose(object);
}

static void
gn_key_package_manager_finalize(GObject *object)
{
  GnKeyPackageManager *self = GN_KEY_PACKAGE_MANAGER(object);

  g_clear_pointer(&self->last_kp_event_id, g_free);

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
  self->last_kp_event_id   = NULL;
  self->rotation_source_id = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Internal: Key Package Creation Flow
 *
 * 1. marmot_gobject_client_create_key_package_async() → unsigned kind:443 event JSON
 * 2. gnostr_plugin_context_request_sign_event() → signed event JSON
 * 3. gnostr_plugin_context_publish_event_async() → publish to relays
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  GnKeyPackageManager *manager;
  GTask               *task;
} CreateKpData;

static void
on_kp_published(GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  CreateKpData *data = user_data;
  g_autoptr(GError) error = NULL;

  gboolean ok = gnostr_plugin_context_publish_event_finish(
    data->manager->context, result, &error);

  if (ok)
    {
      g_info("KeyPackageManager: key package published successfully");
      g_task_return_boolean(data->task, TRUE);
    }
  else
    {
      g_warning("KeyPackageManager: failed to publish key package: %s",
                error ? error->message : "unknown");
      g_task_return_error(data->task, g_steal_pointer(&error));
    }

  g_object_unref(data->task);
  g_free(data);
}

static void
on_kp_signed(GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  CreateKpData *data = user_data;
  g_autoptr(GError) error = NULL;

  g_autofree gchar *signed_json =
    gnostr_plugin_context_request_sign_event_finish(
      data->manager->context, result, &error);

  if (signed_json == NULL)
    {
      g_warning("KeyPackageManager: signer refused key package: %s",
                error ? error->message : "unknown");
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      g_free(data);
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
    marmot_gobject_client_create_key_package_finish(client, result, &error);

  if (unsigned_json == NULL)
    {
      g_warning("KeyPackageManager: failed to create key package: %s",
                error ? error->message : "unknown");
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      g_free(data);
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
  data->manager = self;
  data->task    = task; /* takes ownership */

  MarmotGobjectClient *client = gn_marmot_service_get_client(service);

  /* Step 1: Create the key package via marmot */
  marmot_gobject_client_create_key_package_async(
    client,
    pubkey,
    NULL, /* sk_hex — provided separately or via service identity */
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
  self->service = service; /* weak ref */
  self->context = plugin_context; /* borrowed */

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
   * TODO: Check if a valid key package already exists on relays
   * before creating a new one. For now, always create fresh.
   *
   * Future: Query kind:443 from relays with {"authors": [pubkey]}
   * and check if any are still valid (not consumed, not expired).
   */

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

  /*
   * TODO: Build a kind:10051 event with relay URLs:
   *   {
   *     "kind": 10051,
   *     "tags": [
   *       ["relay", "wss://relay1.example.com"],
   *       ["relay", "wss://relay2.example.com"]
   *     ],
   *     "content": ""
   *   }
   *
   * Sign and publish via plugin context.
   */

  g_info("KeyPackageManager: publishing key package relay list (kind:10051)");

  /* Placeholder - return success for now */
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

gboolean
gn_key_package_manager_publish_relay_list_finish(GnKeyPackageManager *self,
                                                  GAsyncResult        *result,
                                                  GError             **error)
{
  g_return_val_if_fail(GN_IS_KEY_PACKAGE_MANAGER(self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
