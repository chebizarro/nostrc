/* hw_keystore_manager.c - Hardware Keystore Manager implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "hw_keystore_manager.h"
#include "settings_manager.h"
#include <string.h>
#include <stdio.h>

/* libnostr for crypto operations */
#include <nostr_keys.h>

/* GSettings keys */
#define GSETTINGS_HW_KEYSTORE_ENABLED "hardware-keystore-enabled"
#define GSETTINGS_HW_KEYSTORE_MODE "hardware-keystore-mode"
#define GSETTINGS_HW_KEYSTORE_FALLBACK "hardware-keystore-fallback"

/* ============================================================================
 * Private Types
 * ============================================================================ */

struct _HwKeystoreManager {
  GObject parent_instance;

  GnHsmProviderTpm *provider;
  HwKeystoreMode mode;
  HwKeystoreSetupStatus setup_status;

  gboolean initialized;
  gchar *backend_name;

  /* Settings */
  GSettings *settings;

  GMutex lock;
};

G_DEFINE_TYPE(HwKeystoreManager, hw_keystore_manager, G_TYPE_OBJECT)

/* Signals */
enum {
  SIGNAL_MODE_CHANGED,
  SIGNAL_SETUP_STATUS_CHANGED,
  SIGNAL_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

/* Singleton instance */
static HwKeystoreManager *default_instance = NULL;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static gchar *
bytes_to_hex(const guint8 *bytes, gsize len)
{
  static const gchar hex_chars[] = "0123456789abcdef";
  gchar *result = g_malloc(len * 2 + 1);
  for (gsize i = 0; i < len; i++) {
    result[i * 2] = hex_chars[(bytes[i] >> 4) & 0xF];
    result[i * 2 + 1] = hex_chars[bytes[i] & 0xF];
  }
  result[len * 2] = '\0';
  return result;
}

static void
update_setup_status(HwKeystoreManager *self)
{
  HwKeystoreSetupStatus old_status = self->setup_status;

  if (self->mode == HW_KEYSTORE_MODE_DISABLED) {
    self->setup_status = HW_KEYSTORE_SETUP_NOT_STARTED;
  } else if (!self->provider) {
    self->setup_status = HW_KEYSTORE_SETUP_FAILED;
  } else if (gn_hsm_provider_tpm_has_master_key(self->provider)) {
    self->setup_status = HW_KEYSTORE_SETUP_READY;
  } else {
    self->setup_status = HW_KEYSTORE_SETUP_NEEDED;
  }

  if (old_status != self->setup_status) {
    g_signal_emit(self, signals[SIGNAL_SETUP_STATUS_CHANGED], 0, self->setup_status);
  }
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

const gchar *
hw_keystore_mode_to_string(HwKeystoreMode mode)
{
  switch (mode) {
    case HW_KEYSTORE_MODE_DISABLED:
      return "Disabled";
    case HW_KEYSTORE_MODE_HARDWARE:
      return "Hardware Only";
    case HW_KEYSTORE_MODE_FALLBACK:
      return "Software Fallback";
    case HW_KEYSTORE_MODE_AUTO:
      return "Automatic";
    default:
      return "Unknown";
  }
}

const gchar *
hw_keystore_setup_status_to_string(HwKeystoreSetupStatus status)
{
  switch (status) {
    case HW_KEYSTORE_SETUP_NOT_STARTED:
      return "Not Started";
    case HW_KEYSTORE_SETUP_READY:
      return "Ready";
    case HW_KEYSTORE_SETUP_NEEDED:
      return "Setup Needed";
    case HW_KEYSTORE_SETUP_FAILED:
      return "Setup Failed";
    default:
      return "Unknown";
  }
}

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
hw_keystore_manager_finalize(GObject *object)
{
  HwKeystoreManager *self = HW_KEYSTORE_MANAGER(object);

  g_clear_object(&self->provider);
  g_clear_object(&self->settings);
  g_free(self->backend_name);

  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(hw_keystore_manager_parent_class)->finalize(object);
}

static void
hw_keystore_manager_class_init(HwKeystoreManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = hw_keystore_manager_finalize;

  /**
   * HwKeystoreManager::mode-changed:
   * @manager: The manager
   * @mode: The new mode
   */
  signals[SIGNAL_MODE_CHANGED] =
    g_signal_new("mode-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_INT);

  /**
   * HwKeystoreManager::setup-status-changed:
   * @manager: The manager
   * @status: The new status
   */
  signals[SIGNAL_SETUP_STATUS_CHANGED] =
    g_signal_new("setup-status-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_INT);

  /**
   * HwKeystoreManager::error:
   * @manager: The manager
   * @message: Error message
   */
  signals[SIGNAL_ERROR] =
    g_signal_new("error",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_STRING);
}

static void
hw_keystore_manager_init(HwKeystoreManager *self)
{
  g_mutex_init(&self->lock);
  self->mode = HW_KEYSTORE_MODE_DISABLED;
  self->setup_status = HW_KEYSTORE_SETUP_NOT_STARTED;
  self->initialized = FALSE;
  self->backend_name = NULL;

  /* Create the TPM provider */
  self->provider = gn_hsm_provider_tpm_new();

  /* Try to get GSettings (may fail if schema not installed) */
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (source) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(source,
                                                               GNOSTR_SIGNER_SCHEMA_ID,
                                                               TRUE);
    if (schema) {
      self->settings = g_settings_new(GNOSTR_SIGNER_SCHEMA_ID);
      g_settings_schema_unref(schema);
    }
  }
}

/* ============================================================================
 * Public API - Creation
 * ============================================================================ */

HwKeystoreManager *
hw_keystore_manager_new(void)
{
  return g_object_new(HW_TYPE_KEYSTORE_MANAGER, NULL);
}

HwKeystoreManager *
hw_keystore_manager_get_default(void)
{
  if (default_instance == NULL) {
    default_instance = hw_keystore_manager_new();
    hw_keystore_manager_load_settings(default_instance);
  }
  return default_instance;
}

/* ============================================================================
 * Public API - Hardware Detection
 * ============================================================================ */

gboolean
hw_keystore_manager_is_hardware_available(HwKeystoreManager *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);

  if (!self->provider)
    return FALSE;

  GnHwKeystoreBackend backend = gn_hsm_provider_tpm_get_backend(self->provider);
  return backend != GN_HW_KEYSTORE_NONE && backend != GN_HW_KEYSTORE_SOFTWARE;
}

GnHwKeystoreInfo *
hw_keystore_manager_get_hardware_info(HwKeystoreManager *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), NULL);

  if (!self->provider)
    return NULL;

  return gn_hsm_provider_tpm_get_keystore_info(self->provider);
}

const gchar *
hw_keystore_manager_get_backend_name(HwKeystoreManager *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), "Unknown");

  if (self->backend_name)
    return self->backend_name;

  if (!self->provider)
    return "Unavailable";

  GnHwKeystoreBackend backend = gn_hsm_provider_tpm_get_backend(self->provider);
  return gn_hw_keystore_backend_to_string(backend);
}

/* ============================================================================
 * Public API - Enable/Disable
 * ============================================================================ */

HwKeystoreMode
hw_keystore_manager_get_mode(HwKeystoreManager *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), HW_KEYSTORE_MODE_DISABLED);
  return self->mode;
}

void
hw_keystore_manager_set_mode(HwKeystoreManager *self, HwKeystoreMode mode)
{
  g_return_if_fail(HW_IS_KEYSTORE_MANAGER(self));

  g_mutex_lock(&self->lock);

  if (self->mode == mode) {
    g_mutex_unlock(&self->lock);
    return;
  }

  HwKeystoreMode old_mode = self->mode;
  self->mode = mode;

  /* Configure provider based on mode */
  if (self->provider) {
    switch (mode) {
      case HW_KEYSTORE_MODE_DISABLED:
        gn_hsm_provider_shutdown(GN_HSM_PROVIDER(self->provider));
        break;

      case HW_KEYSTORE_MODE_HARDWARE:
        gn_hsm_provider_tpm_set_fallback_enabled(self->provider, FALSE);
        if (!self->initialized) {
          GError *err = NULL;
          if (gn_hsm_provider_init(GN_HSM_PROVIDER(self->provider), &err)) {
            self->initialized = TRUE;
          } else {
            g_warning("Failed to init provider: %s", err ? err->message : "unknown");
            g_clear_error(&err);
          }
        }
        break;

      case HW_KEYSTORE_MODE_FALLBACK:
      case HW_KEYSTORE_MODE_AUTO:
        gn_hsm_provider_tpm_set_fallback_enabled(self->provider, TRUE);
        if (!self->initialized) {
          GError *err = NULL;
          if (gn_hsm_provider_init(GN_HSM_PROVIDER(self->provider), &err)) {
            self->initialized = TRUE;
          } else {
            g_warning("Failed to init provider: %s", err ? err->message : "unknown");
            g_clear_error(&err);
          }
        }
        break;
    }
  }

  update_setup_status(self);

  g_mutex_unlock(&self->lock);

  if (old_mode != mode) {
    g_signal_emit(self, signals[SIGNAL_MODE_CHANGED], 0, mode);
    hw_keystore_manager_save_settings(self);
  }
}

gboolean
hw_keystore_manager_is_enabled(HwKeystoreManager *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);
  return self->mode != HW_KEYSTORE_MODE_DISABLED && self->initialized;
}

void
hw_keystore_manager_set_enabled(HwKeystoreManager *self, gboolean enabled)
{
  g_return_if_fail(HW_IS_KEYSTORE_MANAGER(self));

  if (enabled) {
    hw_keystore_manager_set_mode(self, HW_KEYSTORE_MODE_AUTO);
  } else {
    hw_keystore_manager_set_mode(self, HW_KEYSTORE_MODE_DISABLED);
  }
}

/* ============================================================================
 * Public API - Master Key Management
 * ============================================================================ */

HwKeystoreSetupStatus
hw_keystore_manager_get_setup_status(HwKeystoreManager *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), HW_KEYSTORE_SETUP_NOT_STARTED);

  g_mutex_lock(&self->lock);
  update_setup_status(self);
  HwKeystoreSetupStatus status = self->setup_status;
  g_mutex_unlock(&self->lock);

  return status;
}

gboolean
hw_keystore_manager_has_master_key(HwKeystoreManager *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);

  if (!self->provider || !self->initialized)
    return FALSE;

  return gn_hsm_provider_tpm_has_master_key(self->provider);
}

gboolean
hw_keystore_manager_setup_master_key(HwKeystoreManager *self, GError **error)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);

  if (!self->provider) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Hardware keystore not available");
    return FALSE;
  }

  if (self->mode == HW_KEYSTORE_MODE_DISABLED) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Hardware keystore is disabled");
    return FALSE;
  }

  /* Initialize if needed */
  if (!self->initialized) {
    if (!gn_hsm_provider_init(GN_HSM_PROVIDER(self->provider), error)) {
      g_mutex_lock(&self->lock);
      self->setup_status = HW_KEYSTORE_SETUP_FAILED;
      g_mutex_unlock(&self->lock);
      g_signal_emit(self, signals[SIGNAL_SETUP_STATUS_CHANGED], 0, self->setup_status);
      return FALSE;
    }
    self->initialized = TRUE;
  }

  /* Check if master key already exists */
  if (gn_hsm_provider_tpm_has_master_key(self->provider)) {
    g_mutex_lock(&self->lock);
    self->setup_status = HW_KEYSTORE_SETUP_READY;
    g_mutex_unlock(&self->lock);
    g_signal_emit(self, signals[SIGNAL_SETUP_STATUS_CHANGED], 0, self->setup_status);
    return TRUE;
  }

  /* Create master key */
  gboolean result = gn_hsm_provider_tpm_create_master_key(self->provider, error);

  g_mutex_lock(&self->lock);
  if (result) {
    self->setup_status = HW_KEYSTORE_SETUP_READY;
  } else {
    self->setup_status = HW_KEYSTORE_SETUP_FAILED;
  }
  g_mutex_unlock(&self->lock);

  g_signal_emit(self, signals[SIGNAL_SETUP_STATUS_CHANGED], 0, self->setup_status);

  return result;
}

gboolean
hw_keystore_manager_reset_master_key(HwKeystoreManager *self, GError **error)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);

  if (!hw_keystore_manager_delete_master_key(self, error))
    return FALSE;

  return hw_keystore_manager_setup_master_key(self, error);
}

gboolean
hw_keystore_manager_delete_master_key(HwKeystoreManager *self, GError **error)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);

  if (!self->provider) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Hardware keystore not available");
    return FALSE;
  }

  gboolean result = gn_hsm_provider_tpm_delete_master_key(self->provider, error);

  g_mutex_lock(&self->lock);
  if (result) {
    self->setup_status = HW_KEYSTORE_SETUP_NEEDED;
  }
  g_mutex_unlock(&self->lock);

  g_signal_emit(self, signals[SIGNAL_SETUP_STATUS_CHANGED], 0, self->setup_status);

  return result;
}

/* ============================================================================
 * Public API - Key Derivation
 * ============================================================================ */

gboolean
hw_keystore_manager_get_signing_key(HwKeystoreManager *self,
                                     const gchar *npub,
                                     guint8 *private_key_out,
                                     GError **error)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);
  g_return_val_if_fail(npub != NULL, FALSE);
  g_return_val_if_fail(private_key_out != NULL, FALSE);

  if (!self->provider || !self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Hardware keystore not initialized");
    return FALSE;
  }

  if (self->mode == HW_KEYSTORE_MODE_DISABLED) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Hardware keystore is disabled");
    return FALSE;
  }

  return gn_hsm_provider_tpm_derive_signing_key(self->provider, npub,
                                                 private_key_out, error);
}

gboolean
hw_keystore_manager_get_public_key(HwKeystoreManager *self,
                                    const gchar *npub,
                                    guint8 *public_key_out,
                                    GError **error)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);
  g_return_val_if_fail(npub != NULL, FALSE);
  g_return_val_if_fail(public_key_out != NULL, FALSE);

  guint8 private_key[32];
  if (!hw_keystore_manager_get_signing_key(self, npub, private_key, error)) {
    return FALSE;
  }

  /* Derive public key from private key */
  gchar *sk_hex = bytes_to_hex(private_key, 32);
  memset(private_key, 0, sizeof(private_key));

  GNostrKeys *gkeys = gnostr_keys_new_from_hex(sk_hex, NULL);
  memset(sk_hex, 0, strlen(sk_hex));
  g_free(sk_hex);

  if (!gkeys) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Failed to derive public key");
    return FALSE;
  }

  const gchar *pk_hex = gnostr_keys_get_pubkey(gkeys);
  gsize pk_len = strlen(pk_hex);
  if (pk_len != 64) {
    g_object_unref(gkeys);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Invalid public key length");
    return FALSE;
  }

  for (gsize i = 0; i < 32; i++) {
    guint val;
    sscanf(pk_hex + i * 2, "%2x", &val);
    public_key_out[i] = val;
  }

  g_object_unref(gkeys);
  return TRUE;
}

gboolean
hw_keystore_manager_sign_hash(HwKeystoreManager *self,
                               const gchar *npub,
                               const guint8 *hash,
                               guint8 *signature_out,
                               GError **error)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);
  g_return_val_if_fail(npub != NULL, FALSE);
  g_return_val_if_fail(hash != NULL, FALSE);
  g_return_val_if_fail(signature_out != NULL, FALSE);

  if (!self->provider || !self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Hardware keystore not initialized");
    return FALSE;
  }

  gsize sig_len = 64;
  return gn_hsm_provider_sign_hash(GN_HSM_PROVIDER(self->provider),
                                    0, npub, hash, 32,
                                    signature_out, &sig_len, error);
}

gchar *
hw_keystore_manager_sign_event(HwKeystoreManager *self,
                                const gchar *npub,
                                const gchar *event_json,
                                GError **error)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), NULL);
  g_return_val_if_fail(npub != NULL, NULL);
  g_return_val_if_fail(event_json != NULL, NULL);

  if (!self->provider || !self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Hardware keystore not initialized");
    return NULL;
  }

  return gn_hsm_provider_sign_event(GN_HSM_PROVIDER(self->provider),
                                     0, npub, event_json, error);
}

/* ============================================================================
 * Public API - Import/Export
 * ============================================================================ */

gboolean
hw_keystore_manager_can_import_existing_key(HwKeystoreManager *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);
  /* Hardware keystores derive keys - they don't import them */
  return FALSE;
}

gboolean
hw_keystore_manager_migrate_from_software(HwKeystoreManager *self,
                                           const gchar *npub,
                                           const guint8 *software_private_key,
                                           GError **error)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), FALSE);
  g_return_val_if_fail(npub != NULL, FALSE);
  g_return_val_if_fail(software_private_key != NULL, FALSE);

  /* Hardware keystore keys are derived from master key, so we can't
   * actually migrate - we just verify we can derive a key for this npub */
  guint8 derived_key[32];
  if (!hw_keystore_manager_get_signing_key(self, npub, derived_key, error)) {
    return FALSE;
  }

  /* Clear derived key from memory */
  memset(derived_key, 0, sizeof(derived_key));

  g_message("Migration note: npub %s now uses hardware-derived keys "
            "(different from original software key)", npub);

  return TRUE;
}

/* ============================================================================
 * Public API - Settings Integration
 * ============================================================================ */

void
hw_keystore_manager_load_settings(HwKeystoreManager *self)
{
  g_return_if_fail(HW_IS_KEYSTORE_MANAGER(self));

  if (!self->settings) {
    /* No settings available, use defaults */
    return;
  }

  g_mutex_lock(&self->lock);

  /* Load mode setting */
  gint mode_val = g_settings_get_int(self->settings, GSETTINGS_HW_KEYSTORE_MODE);
  if (mode_val >= HW_KEYSTORE_MODE_DISABLED && mode_val <= HW_KEYSTORE_MODE_AUTO) {
    self->mode = (HwKeystoreMode)mode_val;
  }

  /* Load fallback setting */
  gboolean fallback = g_settings_get_boolean(self->settings, GSETTINGS_HW_KEYSTORE_FALLBACK);
  if (self->provider) {
    gn_hsm_provider_tpm_set_fallback_enabled(self->provider, fallback);
  }

  g_mutex_unlock(&self->lock);

  /* Initialize provider if enabled */
  if (self->mode != HW_KEYSTORE_MODE_DISABLED && self->provider && !self->initialized) {
    GError *err = NULL;
    if (gn_hsm_provider_init(GN_HSM_PROVIDER(self->provider), &err)) {
      self->initialized = TRUE;
    } else {
      g_warning("Failed to initialize hardware keystore: %s",
                err ? err->message : "unknown");
      g_clear_error(&err);
    }
  }

  update_setup_status(self);
}

void
hw_keystore_manager_save_settings(HwKeystoreManager *self)
{
  g_return_if_fail(HW_IS_KEYSTORE_MANAGER(self));

  if (!self->settings)
    return;

  g_mutex_lock(&self->lock);

  g_settings_set_int(self->settings, GSETTINGS_HW_KEYSTORE_MODE, (gint)self->mode);

  if (self->provider) {
    gboolean fallback = gn_hsm_provider_tpm_get_fallback_enabled(self->provider);
    g_settings_set_boolean(self->settings, GSETTINGS_HW_KEYSTORE_FALLBACK, fallback);
  }

  g_mutex_unlock(&self->lock);
}

/* ============================================================================
 * Public API - Provider Access
 * ============================================================================ */

GnHsmProviderTpm *
hw_keystore_manager_get_provider(HwKeystoreManager *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_MANAGER(self), NULL);
  return self->provider;
}
