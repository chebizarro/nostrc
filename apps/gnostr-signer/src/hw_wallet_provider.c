/* hw_wallet_provider.c - Hardware wallet provider implementation
 *
 * Implements the GnHwWalletProvider interface and GnHwWalletManager registry.
 * Uses hidapi for USB HID communication with hardware wallets.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hw_wallet_provider.h"
#include <string.h>

#ifdef GNOSTR_HAVE_HIDAPI
#include <hidapi/hidapi.h>
#endif

/* ============================================================================
 * Error Domain
 * ============================================================================ */

G_DEFINE_QUARK(gn-hw-wallet-error-quark, gn_hw_wallet_error)

/* ============================================================================
 * Device Info
 * ============================================================================ */

GnHwWalletDeviceInfo *
gn_hw_wallet_device_info_copy(const GnHwWalletDeviceInfo *info)
{
  if (!info)
    return NULL;

  GnHwWalletDeviceInfo *copy = g_new0(GnHwWalletDeviceInfo, 1);
  copy->device_id = g_strdup(info->device_id);
  copy->type = info->type;
  copy->manufacturer = g_strdup(info->manufacturer);
  copy->product = g_strdup(info->product);
  copy->serial = g_strdup(info->serial);
  copy->firmware_version = g_strdup(info->firmware_version);
  copy->state = info->state;
  copy->app_name = g_strdup(info->app_name);
  copy->app_version = g_strdup(info->app_version);
  copy->needs_pin = info->needs_pin;
  copy->has_nostr_app = info->has_nostr_app;
  return copy;
}

void
gn_hw_wallet_device_info_free(GnHwWalletDeviceInfo *info)
{
  if (!info)
    return;
  g_free(info->device_id);
  g_free(info->manufacturer);
  g_free(info->product);
  g_free(info->serial);
  g_free(info->firmware_version);
  g_free(info->app_name);
  g_free(info->app_version);
  g_free(info);
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

const gchar *
gn_hw_wallet_type_to_string(GnHwWalletType type)
{
  switch (type) {
    case GN_HW_WALLET_TYPE_LEDGER_NANO_S:
      return "Ledger Nano S";
    case GN_HW_WALLET_TYPE_LEDGER_NANO_X:
      return "Ledger Nano X";
    case GN_HW_WALLET_TYPE_LEDGER_NANO_S_PLUS:
      return "Ledger Nano S Plus";
    case GN_HW_WALLET_TYPE_TREZOR_ONE:
      return "Trezor One";
    case GN_HW_WALLET_TYPE_TREZOR_T:
      return "Trezor Model T";
    case GN_HW_WALLET_TYPE_TREZOR_SAFE_3:
      return "Trezor Safe 3";
    default:
      return "Unknown";
  }
}

const gchar *
gn_hw_wallet_state_to_string(GnHwWalletState state)
{
  switch (state) {
    case GN_HW_WALLET_STATE_DISCONNECTED:
      return "Disconnected";
    case GN_HW_WALLET_STATE_CONNECTED:
      return "Connected";
    case GN_HW_WALLET_STATE_APP_CLOSED:
      return "App Closed";
    case GN_HW_WALLET_STATE_READY:
      return "Ready";
    case GN_HW_WALLET_STATE_BUSY:
      return "Busy";
    case GN_HW_WALLET_STATE_ERROR:
      return "Error";
    default:
      return "Unknown";
  }
}

gboolean
gn_hw_wallet_type_is_ledger(GnHwWalletType type)
{
  return type == GN_HW_WALLET_TYPE_LEDGER_NANO_S ||
         type == GN_HW_WALLET_TYPE_LEDGER_NANO_X ||
         type == GN_HW_WALLET_TYPE_LEDGER_NANO_S_PLUS;
}

gboolean
gn_hw_wallet_type_is_trezor(GnHwWalletType type)
{
  return type == GN_HW_WALLET_TYPE_TREZOR_ONE ||
         type == GN_HW_WALLET_TYPE_TREZOR_T ||
         type == GN_HW_WALLET_TYPE_TREZOR_SAFE_3;
}

/* ============================================================================
 * Hardware Wallet Provider Interface
 * ============================================================================ */

G_DEFINE_INTERFACE(GnHwWalletProvider, gn_hw_wallet_provider, G_TYPE_OBJECT)

static void
gn_hw_wallet_provider_default_init(GnHwWalletProviderInterface *iface)
{
  (void)iface;
}

GnHwWalletType
gn_hw_wallet_provider_get_device_type(GnHwWalletProvider *self)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_PROVIDER(self), GN_HW_WALLET_TYPE_UNKNOWN);
  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->get_device_type != NULL, GN_HW_WALLET_TYPE_UNKNOWN);
  return iface->get_device_type(self);
}

GPtrArray *
gn_hw_wallet_provider_enumerate_devices(GnHwWalletProvider *self, GError **error)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_PROVIDER(self), NULL);
  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (!iface->enumerate_devices) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_UNSUPPORTED,
                "Provider does not support device enumeration");
    return NULL;
  }
  return iface->enumerate_devices(self, error);
}

gboolean
gn_hw_wallet_provider_open_device(GnHwWalletProvider *self,
                                  const gchar *device_id,
                                  GError **error)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_PROVIDER(self), FALSE);
  g_return_val_if_fail(device_id != NULL, FALSE);
  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (!iface->open_device) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_UNSUPPORTED,
                "Provider does not support opening devices");
    return FALSE;
  }
  return iface->open_device(self, device_id, error);
}

void
gn_hw_wallet_provider_close_device(GnHwWalletProvider *self,
                                   const gchar *device_id)
{
  g_return_if_fail(GN_IS_HW_WALLET_PROVIDER(self));
  g_return_if_fail(device_id != NULL);
  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (iface->close_device)
    iface->close_device(self, device_id);
}

GnHwWalletState
gn_hw_wallet_provider_get_device_state(GnHwWalletProvider *self,
                                       const gchar *device_id)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_PROVIDER(self), GN_HW_WALLET_STATE_DISCONNECTED);
  g_return_val_if_fail(device_id != NULL, GN_HW_WALLET_STATE_DISCONNECTED);
  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (!iface->get_device_state)
    return GN_HW_WALLET_STATE_DISCONNECTED;
  return iface->get_device_state(self, device_id);
}

gboolean
gn_hw_wallet_provider_get_public_key(GnHwWalletProvider *self,
                                     const gchar *device_id,
                                     const gchar *derivation_path,
                                     guint8 *pubkey_out,
                                     gsize *pubkey_len,
                                     gboolean confirm_on_device,
                                     GError **error)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_PROVIDER(self), FALSE);
  g_return_val_if_fail(device_id != NULL, FALSE);
  g_return_val_if_fail(derivation_path != NULL, FALSE);
  g_return_val_if_fail(pubkey_out != NULL, FALSE);
  g_return_val_if_fail(pubkey_len != NULL, FALSE);

  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (!iface->get_public_key) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_UNSUPPORTED,
                "Provider does not support getting public key");
    return FALSE;
  }
  return iface->get_public_key(self, device_id, derivation_path,
                               pubkey_out, pubkey_len, confirm_on_device, error);
}

gboolean
gn_hw_wallet_provider_sign_hash(GnHwWalletProvider *self,
                                const gchar *device_id,
                                const gchar *derivation_path,
                                const guint8 *hash,
                                gsize hash_len,
                                guint8 *signature_out,
                                gsize *signature_len,
                                GError **error)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_PROVIDER(self), FALSE);
  g_return_val_if_fail(device_id != NULL, FALSE);
  g_return_val_if_fail(derivation_path != NULL, FALSE);
  g_return_val_if_fail(hash != NULL, FALSE);
  g_return_val_if_fail(signature_out != NULL, FALSE);
  g_return_val_if_fail(signature_len != NULL, FALSE);

  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (!iface->sign_hash) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_UNSUPPORTED,
                "Provider does not support signing");
    return FALSE;
  }
  return iface->sign_hash(self, device_id, derivation_path,
                          hash, hash_len, signature_out, signature_len, error);
}

/* Async operations */
void
gn_hw_wallet_provider_get_public_key_async(GnHwWalletProvider *self,
                                           const gchar *device_id,
                                           const gchar *derivation_path,
                                           gboolean confirm_on_device,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data)
{
  g_return_if_fail(GN_IS_HW_WALLET_PROVIDER(self));
  g_return_if_fail(device_id != NULL);
  g_return_if_fail(derivation_path != NULL);

  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (iface->get_public_key_async) {
    iface->get_public_key_async(self, device_id, derivation_path,
                                confirm_on_device, cancellable, callback, user_data);
  } else {
    /* Fallback: run sync in a task */
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    gchar **params = g_new0(gchar *, 3);
    params[0] = g_strdup(device_id);
    params[1] = g_strdup(derivation_path);
    params[2] = g_strdup_printf("%d", confirm_on_device ? 1 : 0);
    g_task_set_task_data(task, params, (GDestroyNotify)g_strfreev);

    g_task_run_in_thread(task, (GTaskThreadFunc)({
      void inner(GTask *t, gpointer src, gpointer td, GCancellable *c) {
        (void)c;
        gchar **p = (gchar **)td;
        gboolean confirm = (p[2][0] == '1');
        guint8 pubkey[33];
        gsize pubkey_len = sizeof(pubkey);
        GError *error = NULL;
        if (gn_hw_wallet_provider_get_public_key(GN_HW_WALLET_PROVIDER(src),
                                                  p[0], p[1], pubkey, &pubkey_len,
                                                  confirm, &error)) {
          GBytes *bytes = g_bytes_new(pubkey, pubkey_len);
          g_task_return_pointer(t, bytes, (GDestroyNotify)g_bytes_unref);
        } else {
          g_task_return_error(t, error);
        }
      }
      inner;
    }));
    g_object_unref(task);
  }
}

gboolean
gn_hw_wallet_provider_get_public_key_finish(GnHwWalletProvider *self,
                                            GAsyncResult *result,
                                            guint8 *pubkey_out,
                                            gsize *pubkey_len,
                                            GError **error)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_PROVIDER(self), FALSE);
  g_return_val_if_fail(G_IS_TASK(result), FALSE);

  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (iface->get_public_key_finish) {
    return iface->get_public_key_finish(self, result, pubkey_out, pubkey_len, error);
  }

  GBytes *bytes = g_task_propagate_pointer(G_TASK(result), error);
  if (!bytes)
    return FALSE;

  gsize len;
  const guint8 *data = g_bytes_get_data(bytes, &len);
  if (pubkey_len)
    *pubkey_len = MIN(len, *pubkey_len);
  if (pubkey_out)
    memcpy(pubkey_out, data, MIN(len, *pubkey_len));
  g_bytes_unref(bytes);
  return TRUE;
}

void
gn_hw_wallet_provider_sign_hash_async(GnHwWalletProvider *self,
                                      const gchar *device_id,
                                      const gchar *derivation_path,
                                      const guint8 *hash,
                                      gsize hash_len,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  g_return_if_fail(GN_IS_HW_WALLET_PROVIDER(self));
  g_return_if_fail(device_id != NULL);
  g_return_if_fail(derivation_path != NULL);
  g_return_if_fail(hash != NULL);

  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (iface->sign_hash_async) {
    iface->sign_hash_async(self, device_id, derivation_path,
                           hash, hash_len, cancellable, callback, user_data);
  } else {
    /* Fallback: run sync in a task */
    GTask *task = g_task_new(self, cancellable, callback, user_data);

    /* Pack parameters: device_id, path, hash_hex */
    gchar *hash_hex = g_malloc(hash_len * 2 + 1);
    for (gsize i = 0; i < hash_len; i++)
      snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);
    hash_hex[hash_len * 2] = '\0';

    gchar **params = g_new0(gchar *, 4);
    params[0] = g_strdup(device_id);
    params[1] = g_strdup(derivation_path);
    params[2] = hash_hex;
    params[3] = NULL;
    g_task_set_task_data(task, params, (GDestroyNotify)g_strfreev);

    g_task_run_in_thread(task, (GTaskThreadFunc)({
      void inner(GTask *t, gpointer src, gpointer td, GCancellable *c) {
        (void)c;
        gchar **p = (gchar **)td;
        /* Decode hash from hex */
        gsize hlen = strlen(p[2]) / 2;
        guint8 *h = g_malloc(hlen);
        for (gsize i = 0; i < hlen; i++) {
          guint hi, lo;
          sscanf(p[2] + i * 2, "%1x%1x", &hi, &lo);
          h[i] = (hi << 4) | lo;
        }

        guint8 sig[64];
        gsize sig_len = sizeof(sig);
        GError *error = NULL;
        if (gn_hw_wallet_provider_sign_hash(GN_HW_WALLET_PROVIDER(src),
                                            p[0], p[1], h, hlen,
                                            sig, &sig_len, &error)) {
          GBytes *bytes = g_bytes_new(sig, sig_len);
          g_task_return_pointer(t, bytes, (GDestroyNotify)g_bytes_unref);
        } else {
          g_task_return_error(t, error);
        }
        g_free(h);
      }
      inner;
    }));
    g_object_unref(task);
  }
}

gboolean
gn_hw_wallet_provider_sign_hash_finish(GnHwWalletProvider *self,
                                       GAsyncResult *result,
                                       guint8 *signature_out,
                                       gsize *signature_len,
                                       GError **error)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_PROVIDER(self), FALSE);
  g_return_val_if_fail(G_IS_TASK(result), FALSE);

  GnHwWalletProviderInterface *iface = GN_HW_WALLET_PROVIDER_GET_IFACE(self);
  if (iface->sign_hash_finish) {
    return iface->sign_hash_finish(self, result, signature_out, signature_len, error);
  }

  GBytes *bytes = g_task_propagate_pointer(G_TASK(result), error);
  if (!bytes)
    return FALSE;

  gsize len;
  const guint8 *data = g_bytes_get_data(bytes, &len);
  if (signature_len)
    *signature_len = MIN(len, *signature_len);
  if (signature_out)
    memcpy(signature_out, data, MIN(len, *signature_len));
  g_bytes_unref(bytes);
  return TRUE;
}

/* ============================================================================
 * Hardware Wallet Manager Implementation
 * ============================================================================ */

struct _GnHwWalletManager {
  GObject parent_instance;
  GList *providers;
  GMutex lock;
  guint monitor_source_id;
  GnHwWalletPromptCallback prompt_callback;
  gpointer prompt_callback_data;
  GHashTable *device_providers; /* device_id -> provider mapping */
};

enum {
  SIGNAL_DEVICE_CONNECTED,
  SIGNAL_DEVICE_DISCONNECTED,
  SIGNAL_DEVICE_STATE_CHANGED,
  SIGNAL_PROMPT_REQUIRED,
  N_SIGNALS
};

static guint manager_signals[N_SIGNALS] = {0};

G_DEFINE_TYPE(GnHwWalletManager, gn_hw_wallet_manager, G_TYPE_OBJECT)

static GnHwWalletManager *default_manager = NULL;

static void
gn_hw_wallet_manager_finalize(GObject *object)
{
  GnHwWalletManager *self = GN_HW_WALLET_MANAGER(object);

  gn_hw_wallet_manager_stop_monitoring(self);

  g_mutex_lock(&self->lock);
  g_list_free_full(self->providers, g_object_unref);
  self->providers = NULL;
  g_hash_table_unref(self->device_providers);
  g_mutex_unlock(&self->lock);

  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gn_hw_wallet_manager_parent_class)->finalize(object);
}

static void
gn_hw_wallet_manager_class_init(GnHwWalletManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_hw_wallet_manager_finalize;

  /**
   * GnHwWalletManager::device-connected:
   * @manager: The manager
   * @device_info: Information about the connected device
   *
   * Emitted when a hardware wallet device is connected.
   */
  manager_signals[SIGNAL_DEVICE_CONNECTED] =
    g_signal_new("device-connected",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_POINTER);

  /**
   * GnHwWalletManager::device-disconnected:
   * @manager: The manager
   * @device_id: The device ID of the disconnected device
   *
   * Emitted when a hardware wallet device is disconnected.
   */
  manager_signals[SIGNAL_DEVICE_DISCONNECTED] =
    g_signal_new("device-disconnected",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_STRING);

  /**
   * GnHwWalletManager::device-state-changed:
   * @manager: The manager
   * @device_id: The device ID
   * @state: The new state
   *
   * Emitted when a device's state changes.
   */
  manager_signals[SIGNAL_DEVICE_STATE_CHANGED] =
    g_signal_new("device-state-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 2,
                 G_TYPE_STRING,
                 G_TYPE_INT);

  /**
   * GnHwWalletManager::prompt-required:
   * @manager: The manager
   * @prompt_type: The type of prompt required
   * @device_info: The device requiring the prompt
   * @message: Human-readable message
   *
   * Emitted when user interaction is needed on a device.
   */
  manager_signals[SIGNAL_PROMPT_REQUIRED] =
    g_signal_new("prompt-required",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 3,
                 G_TYPE_INT,
                 G_TYPE_POINTER,
                 G_TYPE_STRING);
}

static void
gn_hw_wallet_manager_init(GnHwWalletManager *self)
{
  g_mutex_init(&self->lock);
  self->providers = NULL;
  self->monitor_source_id = 0;
  self->prompt_callback = NULL;
  self->prompt_callback_data = NULL;
  self->device_providers = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
}

GnHwWalletManager *
gn_hw_wallet_manager_get_default(void)
{
  static GOnce once = G_ONCE_INIT;
  g_once(&once, (GThreadFunc)({
    gpointer inner(gpointer data) {
      (void)data;
      return g_object_new(GN_TYPE_HW_WALLET_MANAGER, NULL);
    }
    inner;
  }), NULL);
  return GN_HW_WALLET_MANAGER(once.retval);
}

void
gn_hw_wallet_manager_register_provider(GnHwWalletManager *self,
                                       GnHwWalletProvider *provider)
{
  g_return_if_fail(GN_IS_HW_WALLET_MANAGER(self));
  g_return_if_fail(GN_IS_HW_WALLET_PROVIDER(provider));

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

  GnHwWalletType type = gn_hw_wallet_provider_get_device_type(provider);
  g_message("Hardware wallet provider registered: %s",
            gn_hw_wallet_type_to_string(type));
}

GList *
gn_hw_wallet_manager_get_providers(GnHwWalletManager *self)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_MANAGER(self), NULL);
  return self->providers;
}

GPtrArray *
gn_hw_wallet_manager_enumerate_all_devices(GnHwWalletManager *self,
                                            GError **error)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_MANAGER(self), NULL);

  GPtrArray *all_devices = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gn_hw_wallet_device_info_free);

  g_mutex_lock(&self->lock);

  for (GList *l = self->providers; l != NULL; l = l->next) {
    GnHwWalletProvider *provider = GN_HW_WALLET_PROVIDER(l->data);
    GError *local_error = NULL;
    GPtrArray *devices = gn_hw_wallet_provider_enumerate_devices(provider, &local_error);

    if (local_error) {
      g_warning("Error enumerating devices from provider: %s", local_error->message);
      g_clear_error(&local_error);
      continue;
    }

    if (devices) {
      for (guint i = 0; i < devices->len; i++) {
        GnHwWalletDeviceInfo *info = g_ptr_array_index(devices, i);
        g_ptr_array_add(all_devices, gn_hw_wallet_device_info_copy(info));

        /* Map device to provider */
        g_hash_table_insert(self->device_providers,
                           g_strdup(info->device_id),
                           provider);
      }
      g_ptr_array_unref(devices);
    }
  }

  g_mutex_unlock(&self->lock);

  if (all_devices->len == 0 && error) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_DEVICE_NOT_FOUND,
                "No hardware wallets found");
  }

  return all_devices;
}

GnHwWalletProvider *
gn_hw_wallet_manager_get_provider_for_device(GnHwWalletManager *self,
                                              const gchar *device_id)
{
  g_return_val_if_fail(GN_IS_HW_WALLET_MANAGER(self), NULL);
  g_return_val_if_fail(device_id != NULL, NULL);

  g_mutex_lock(&self->lock);
  GnHwWalletProvider *provider = g_hash_table_lookup(self->device_providers, device_id);
  g_mutex_unlock(&self->lock);

  return provider;
}

/* Device monitoring callback */
static gboolean
monitor_devices_callback(gpointer user_data)
{
  GnHwWalletManager *self = GN_HW_WALLET_MANAGER(user_data);

  /* Re-enumerate devices and emit signals for changes */
  GError *error = NULL;
  GPtrArray *devices = gn_hw_wallet_manager_enumerate_all_devices(self, &error);

  if (error) {
    g_clear_error(&error);
  }

  if (devices) {
    /* TODO: Compare with previous enumeration and emit signals */
    g_ptr_array_unref(devices);
  }

  return G_SOURCE_CONTINUE;
}

void
gn_hw_wallet_manager_start_monitoring(GnHwWalletManager *self)
{
  g_return_if_fail(GN_IS_HW_WALLET_MANAGER(self));

  if (self->monitor_source_id != 0)
    return;

  /* Poll every 2 seconds for device changes */
  self->monitor_source_id = g_timeout_add_seconds(2, monitor_devices_callback, self);
  g_message("Hardware wallet device monitoring started");
}

void
gn_hw_wallet_manager_stop_monitoring(GnHwWalletManager *self)
{
  g_return_if_fail(GN_IS_HW_WALLET_MANAGER(self));

  if (self->monitor_source_id != 0) {
    g_source_remove(self->monitor_source_id);
    self->monitor_source_id = 0;
    g_message("Hardware wallet device monitoring stopped");
  }
}

void
gn_hw_wallet_manager_set_prompt_callback(GnHwWalletManager *self,
                                         GnHwWalletPromptCallback callback,
                                         gpointer user_data)
{
  g_return_if_fail(GN_IS_HW_WALLET_MANAGER(self));
  self->prompt_callback = callback;
  self->prompt_callback_data = user_data;
}

/* ============================================================================
 * Provider Initialization
 * ============================================================================ */

/* Forward declarations for provider constructors */
extern GnHwWalletProvider *gn_hw_wallet_ledger_provider_new(void);
extern GnHwWalletProvider *gn_hw_wallet_trezor_provider_new(void);

void
gn_hw_wallet_providers_init(void)
{
  static gboolean initialized = FALSE;
  if (initialized)
    return;
  initialized = TRUE;

#ifdef GNOSTR_HAVE_HIDAPI
  /* Initialize hidapi */
  if (hid_init() != 0) {
    g_warning("Failed to initialize hidapi");
    return;
  }
#endif

  GnHwWalletManager *manager = gn_hw_wallet_manager_get_default();

#ifdef GNOSTR_HAVE_HIDAPI
  /* Register Ledger provider */
  GnHwWalletProvider *ledger = gn_hw_wallet_ledger_provider_new();
  if (ledger) {
    gn_hw_wallet_manager_register_provider(manager, ledger);
    g_object_unref(ledger);
  }

  /* Register Trezor provider */
  GnHwWalletProvider *trezor = gn_hw_wallet_trezor_provider_new();
  if (trezor) {
    gn_hw_wallet_manager_register_provider(manager, trezor);
    g_object_unref(trezor);
  }
#endif

  g_message("Hardware wallet providers initialized");
}
