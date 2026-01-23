/* hsm_provider.c - Hardware Security Module provider implementation
 *
 * Implements the GnHsmProvider interface and GnHsmManager registry.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hsm_provider.h"
#include <string.h>

/* ============================================================================
 * Error Domain
 * ============================================================================ */

G_DEFINE_QUARK(gn-hsm-error-quark, gn_hsm_error)

/* ============================================================================
 * Device Info
 * ============================================================================ */

GnHsmDeviceInfo *
gn_hsm_device_info_copy(const GnHsmDeviceInfo *info)
{
  if (!info)
    return NULL;

  GnHsmDeviceInfo *copy = g_new0(GnHsmDeviceInfo, 1);
  copy->slot_id = info->slot_id;
  copy->label = g_strdup(info->label);
  copy->manufacturer = g_strdup(info->manufacturer);
  copy->model = g_strdup(info->model);
  copy->serial = g_strdup(info->serial);
  copy->flags = info->flags;
  copy->is_token_present = info->is_token_present;
  copy->is_initialized = info->is_initialized;
  copy->needs_pin = info->needs_pin;
  return copy;
}

void
gn_hsm_device_info_free(GnHsmDeviceInfo *info)
{
  if (!info)
    return;
  g_free(info->label);
  g_free(info->manufacturer);
  g_free(info->model);
  g_free(info->serial);
  g_free(info);
}

/* ============================================================================
 * Key Info
 * ============================================================================ */

GnHsmKeyInfo *
gn_hsm_key_info_copy(const GnHsmKeyInfo *info)
{
  if (!info)
    return NULL;

  GnHsmKeyInfo *copy = g_new0(GnHsmKeyInfo, 1);
  copy->key_id = g_strdup(info->key_id);
  copy->label = g_strdup(info->label);
  copy->npub = g_strdup(info->npub);
  copy->pubkey_hex = g_strdup(info->pubkey_hex);
  copy->key_type = info->key_type;
  copy->created_at = g_strdup(info->created_at);
  copy->slot_id = info->slot_id;
  copy->can_sign = info->can_sign;
  copy->is_extractable = info->is_extractable;
  return copy;
}

void
gn_hsm_key_info_free(GnHsmKeyInfo *info)
{
  if (!info)
    return;
  g_free(info->key_id);
  g_free(info->label);
  g_free(info->npub);
  g_free(info->pubkey_hex);
  g_free(info->created_at);
  g_free(info);
}

/* ============================================================================
 * HSM Provider Interface
 * ============================================================================ */

G_DEFINE_INTERFACE(GnHsmProvider, gn_hsm_provider, G_TYPE_OBJECT)

static void
gn_hsm_provider_default_init(GnHsmProviderInterface *iface)
{
  /* Interface signals could be defined here */
  (void)iface;
}

const gchar *
gn_hsm_provider_get_name(GnHsmProvider *self)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), NULL);
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->get_name != NULL, NULL);
  return iface->get_name(self);
}

gboolean
gn_hsm_provider_is_available(GnHsmProvider *self)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), FALSE);
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->is_available != NULL, FALSE);
  return iface->is_available(self);
}

gboolean
gn_hsm_provider_init(GnHsmProvider *self, GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), FALSE);
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->init_provider)
    return TRUE; /* Default: no init needed */
  return iface->init_provider(self, error);
}

void
gn_hsm_provider_shutdown(GnHsmProvider *self)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER(self));
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (iface->shutdown_provider)
    iface->shutdown_provider(self);
}

GPtrArray *
gn_hsm_provider_detect_devices(GnHsmProvider *self, GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), NULL);
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->detect_devices) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Provider does not support device detection");
    return NULL;
  }
  return iface->detect_devices(self, error);
}

GPtrArray *
gn_hsm_provider_list_keys(GnHsmProvider *self, guint64 slot_id, GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), NULL);
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->list_keys) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Provider does not support key listing");
    return NULL;
  }
  return iface->list_keys(self, slot_id, error);
}

GnHsmKeyInfo *
gn_hsm_provider_get_public_key(GnHsmProvider *self,
                               guint64 slot_id,
                               const gchar *key_id,
                               GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), NULL);
  g_return_val_if_fail(key_id != NULL, NULL);
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->get_public_key) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Provider does not support get_public_key");
    return NULL;
  }
  return iface->get_public_key(self, slot_id, key_id, error);
}

gboolean
gn_hsm_provider_sign_hash(GnHsmProvider *self,
                          guint64 slot_id,
                          const gchar *key_id,
                          const guint8 *hash,
                          gsize hash_len,
                          guint8 *signature,
                          gsize *signature_len,
                          GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), FALSE);
  g_return_val_if_fail(key_id != NULL, FALSE);
  g_return_val_if_fail(hash != NULL, FALSE);
  g_return_val_if_fail(signature != NULL, FALSE);
  g_return_val_if_fail(signature_len != NULL, FALSE);

  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->sign_hash) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Provider does not support sign_hash");
    return FALSE;
  }
  return iface->sign_hash(self, slot_id, key_id, hash, hash_len,
                          signature, signature_len, error);
}

gchar *
gn_hsm_provider_sign_event(GnHsmProvider *self,
                           guint64 slot_id,
                           const gchar *key_id,
                           const gchar *event_json,
                           GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), NULL);
  g_return_val_if_fail(key_id != NULL, NULL);
  g_return_val_if_fail(event_json != NULL, NULL);

  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->sign_event) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Provider does not support sign_event");
    return NULL;
  }
  return iface->sign_event(self, slot_id, key_id, event_json, error);
}

GnHsmKeyInfo *
gn_hsm_provider_generate_key(GnHsmProvider *self,
                              guint64 slot_id,
                              const gchar *label,
                              GnHsmKeyType key_type,
                              GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), NULL);
  g_return_val_if_fail(label != NULL, NULL);

  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->generate_key) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Provider does not support key generation");
    return NULL;
  }
  return iface->generate_key(self, slot_id, label, key_type, error);
}

gboolean
gn_hsm_provider_import_key(GnHsmProvider *self,
                           guint64 slot_id,
                           const gchar *label,
                           const guint8 *private_key,
                           gsize key_len,
                           GnHsmKeyInfo **out_info,
                           GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), FALSE);
  g_return_val_if_fail(label != NULL, FALSE);
  g_return_val_if_fail(private_key != NULL, FALSE);

  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->import_key) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Provider does not support key import");
    return FALSE;
  }
  return iface->import_key(self, slot_id, label, private_key, key_len,
                           out_info, error);
}

gboolean
gn_hsm_provider_delete_key(GnHsmProvider *self,
                           guint64 slot_id,
                           const gchar *key_id,
                           GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), FALSE);
  g_return_val_if_fail(key_id != NULL, FALSE);

  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->delete_key) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Provider does not support key deletion");
    return FALSE;
  }
  return iface->delete_key(self, slot_id, key_id, error);
}

gboolean
gn_hsm_provider_login(GnHsmProvider *self,
                      guint64 slot_id,
                      const gchar *pin,
                      GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), FALSE);

  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (!iface->login) {
    /* No login required is OK */
    return TRUE;
  }
  return iface->login(self, slot_id, pin, error);
}

void
gn_hsm_provider_logout(GnHsmProvider *self, guint64 slot_id)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER(self));
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);
  if (iface->logout)
    iface->logout(self, slot_id);
}

/* ============================================================================
 * Async Thread Functions (portable - no GCC statement expressions)
 * ============================================================================ */

static void
detect_devices_thread_func(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
  (void)td;
  (void)c;
  GError *error = NULL;
  GPtrArray *devices = gn_hsm_provider_detect_devices(GN_HSM_PROVIDER(src), &error);
  if (error)
    g_task_return_error(task, error);
  else
    g_task_return_pointer(task, devices, (GDestroyNotify)g_ptr_array_unref);
}

static void
sign_event_thread_func(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
  (void)c;
  gchar **p = (gchar **)td;
  guint64 sid = g_ascii_strtoull(p[2], NULL, 10);
  GError *error = NULL;
  gchar *result = gn_hsm_provider_sign_event(GN_HSM_PROVIDER(src),
                                              sid, p[0], p[1], &error);
  if (error)
    g_task_return_error(task, error);
  else
    g_task_return_pointer(task, result, g_free);
}

static gpointer
create_hsm_manager_once(gpointer data)
{
  (void)data;
  return g_object_new(GN_TYPE_HSM_MANAGER, NULL);
}

/* ============================================================================
 * Async Operations
 * ============================================================================ */

void
gn_hsm_provider_detect_devices_async(GnHsmProvider *self,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER(self));
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);

  if (iface->detect_devices_async) {
    iface->detect_devices_async(self, cancellable, callback, user_data);
  } else {
    /* Fallback: run sync in a task */
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_run_in_thread(task, detect_devices_thread_func);
    g_object_unref(task);
  }
}

GPtrArray *
gn_hsm_provider_detect_devices_finish(GnHsmProvider *self,
                                      GAsyncResult *result,
                                      GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), NULL);
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);

  if (iface->detect_devices_finish) {
    return iface->detect_devices_finish(self, result, error);
  }
  return g_task_propagate_pointer(G_TASK(result), error);
}

void
gn_hsm_provider_sign_event_async(GnHsmProvider *self,
                                 guint64 slot_id,
                                 const gchar *key_id,
                                 const gchar *event_json,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER(self));
  g_return_if_fail(key_id != NULL);
  g_return_if_fail(event_json != NULL);

  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);

  if (iface->sign_event_async) {
    iface->sign_event_async(self, slot_id, key_id, event_json,
                            cancellable, callback, user_data);
  } else {
    /* Fallback: create task and run sync version */
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    /* Store parameters in task data */
    gchar **params = g_new0(gchar *, 3);
    params[0] = g_strdup(key_id);
    params[1] = g_strdup(event_json);
    params[2] = g_strdup_printf("%" G_GUINT64_FORMAT, slot_id);
    g_task_set_task_data(task, params, (GDestroyNotify)g_strfreev);

    g_task_run_in_thread(task, sign_event_thread_func);
    g_object_unref(task);
  }
}

gchar *
gn_hsm_provider_sign_event_finish(GnHsmProvider *self,
                                  GAsyncResult *result,
                                  GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER(self), NULL);
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  GnHsmProviderInterface *iface = GN_HSM_PROVIDER_GET_IFACE(self);

  if (iface->sign_event_finish) {
    return iface->sign_event_finish(self, result, error);
  }
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ============================================================================
 * HSM Manager Implementation
 * ============================================================================ */

struct _GnHsmManager {
  GObject parent_instance;
  GList *providers;
  GMutex lock;
};

enum {
  SIGNAL_DEVICE_ADDED,
  SIGNAL_DEVICE_REMOVED,
  N_SIGNALS
};

static guint manager_signals[N_SIGNALS] = {0};

G_DEFINE_TYPE(GnHsmManager, gn_hsm_manager, G_TYPE_OBJECT)

static GnHsmManager *default_manager = NULL;

static void
gn_hsm_manager_finalize(GObject *object)
{
  GnHsmManager *self = GN_HSM_MANAGER(object);

  g_mutex_lock(&self->lock);
  g_list_free_full(self->providers, g_object_unref);
  self->providers = NULL;
  g_mutex_unlock(&self->lock);

  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gn_hsm_manager_parent_class)->finalize(object);
}

static void
gn_hsm_manager_class_init(GnHsmManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_hsm_manager_finalize;

  /**
   * GnHsmManager::device-added:
   * @manager: The manager
   * @provider: The provider that detected the device
   * @device_info: Information about the new device
   *
   * Emitted when a new HSM device is detected.
   */
  manager_signals[SIGNAL_DEVICE_ADDED] =
    g_signal_new("device-added",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 2,
                 GN_TYPE_HSM_PROVIDER,
                 G_TYPE_POINTER);

  /**
   * GnHsmManager::device-removed:
   * @manager: The manager
   * @provider: The provider that owned the device
   * @slot_id: The slot ID of the removed device
   *
   * Emitted when an HSM device is removed.
   */
  manager_signals[SIGNAL_DEVICE_REMOVED] =
    g_signal_new("device-removed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 2,
                 GN_TYPE_HSM_PROVIDER,
                 G_TYPE_UINT64);
}

static void
gn_hsm_manager_init(GnHsmManager *self)
{
  g_mutex_init(&self->lock);
  self->providers = NULL;
}

GnHsmManager *
gn_hsm_manager_get_default(void)
{
  static GOnce once = G_ONCE_INIT;
  g_once(&once, create_hsm_manager_once, NULL);
  return GN_HSM_MANAGER(once.retval);
}

void
gn_hsm_manager_register_provider(GnHsmManager *self, GnHsmProvider *provider)
{
  g_return_if_fail(GN_IS_HSM_MANAGER(self));
  g_return_if_fail(GN_IS_HSM_PROVIDER(provider));

  g_mutex_lock(&self->lock);

  /* Check if already registered */
  for (GList *l = self->providers; l != NULL; l = l->next) {
    if (l->data == provider) {
      g_mutex_unlock(&self->lock);
      return;
    }
  }

  self->providers = g_list_prepend(self->providers, g_object_ref(provider));
  g_mutex_unlock(&self->lock);

  g_message("HSM provider registered: %s", gn_hsm_provider_get_name(provider));
}

void
gn_hsm_manager_unregister_provider(GnHsmManager *self, GnHsmProvider *provider)
{
  g_return_if_fail(GN_IS_HSM_MANAGER(self));
  g_return_if_fail(GN_IS_HSM_PROVIDER(provider));

  g_mutex_lock(&self->lock);
  GList *link = g_list_find(self->providers, provider);
  if (link) {
    self->providers = g_list_delete_link(self->providers, link);
    g_mutex_unlock(&self->lock);
    g_message("HSM provider unregistered: %s", gn_hsm_provider_get_name(provider));
    g_object_unref(provider);
  } else {
    g_mutex_unlock(&self->lock);
  }
}

GList *
gn_hsm_manager_get_providers(GnHsmManager *self)
{
  g_return_val_if_fail(GN_IS_HSM_MANAGER(self), NULL);
  return self->providers;
}

GList *
gn_hsm_manager_get_available_providers(GnHsmManager *self)
{
  g_return_val_if_fail(GN_IS_HSM_MANAGER(self), NULL);

  GList *available = NULL;
  g_mutex_lock(&self->lock);

  for (GList *l = self->providers; l != NULL; l = l->next) {
    GnHsmProvider *provider = GN_HSM_PROVIDER(l->data);
    if (gn_hsm_provider_is_available(provider)) {
      available = g_list_prepend(available, provider);
    }
  }

  g_mutex_unlock(&self->lock);
  return g_list_reverse(available);
}

GnHsmProvider *
gn_hsm_manager_get_provider_by_name(GnHsmManager *self, const gchar *name)
{
  g_return_val_if_fail(GN_IS_HSM_MANAGER(self), NULL);
  g_return_val_if_fail(name != NULL, NULL);

  g_mutex_lock(&self->lock);

  for (GList *l = self->providers; l != NULL; l = l->next) {
    GnHsmProvider *provider = GN_HSM_PROVIDER(l->data);
    const gchar *pname = gn_hsm_provider_get_name(provider);
    if (pname && g_strcmp0(pname, name) == 0) {
      g_mutex_unlock(&self->lock);
      return provider;
    }
  }

  g_mutex_unlock(&self->lock);
  return NULL;
}
