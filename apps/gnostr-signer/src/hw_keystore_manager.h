/* hw_keystore_manager.h - Hardware Keystore Manager
 *
 * High-level manager for hardware-backed key storage. Provides:
 * - Automatic hardware detection and fallback
 * - Integration with existing secret store
 * - Key derivation for Nostr identities
 * - Settings persistence
 *
 * Usage:
 *   1. Create manager with hw_keystore_manager_new()
 *   2. Check hardware availability with hw_keystore_manager_is_hardware_available()
 *   3. Enable/disable with hw_keystore_manager_set_enabled()
 *   4. Create master key with hw_keystore_manager_setup_master_key()
 *   5. Get signing keys with hw_keystore_manager_get_signing_key()
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib.h>
#include <glib-object.h>
#include "hsm_provider_tpm.h"

G_BEGIN_DECLS

#define HW_TYPE_KEYSTORE_MANAGER (hw_keystore_manager_get_type())

G_DECLARE_FINAL_TYPE(HwKeystoreManager, hw_keystore_manager, HW, KEYSTORE_MANAGER, GObject)

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * HwKeystoreMode:
 * @HW_KEYSTORE_MODE_DISABLED: Hardware keystore is disabled
 * @HW_KEYSTORE_MODE_HARDWARE: Using hardware-backed keystore
 * @HW_KEYSTORE_MODE_FALLBACK: Using software fallback
 * @HW_KEYSTORE_MODE_AUTO: Automatically choose best available
 *
 * Hardware keystore operation mode.
 */
typedef enum {
  HW_KEYSTORE_MODE_DISABLED = 0,
  HW_KEYSTORE_MODE_HARDWARE,
  HW_KEYSTORE_MODE_FALLBACK,
  HW_KEYSTORE_MODE_AUTO
} HwKeystoreMode;

/**
 * HwKeystoreSetupStatus:
 * @HW_KEYSTORE_SETUP_NOT_STARTED: Setup not started
 * @HW_KEYSTORE_SETUP_READY: Master key exists and is ready
 * @HW_KEYSTORE_SETUP_NEEDED: Master key needs to be created
 * @HW_KEYSTORE_SETUP_FAILED: Setup failed
 *
 * Hardware keystore setup status.
 */
typedef enum {
  HW_KEYSTORE_SETUP_NOT_STARTED = 0,
  HW_KEYSTORE_SETUP_READY,
  HW_KEYSTORE_SETUP_NEEDED,
  HW_KEYSTORE_SETUP_FAILED
} HwKeystoreSetupStatus;

/* ============================================================================
 * Manager Creation
 * ============================================================================ */

/**
 * hw_keystore_manager_new:
 *
 * Creates a new hardware keystore manager instance.
 * Automatically detects available hardware.
 *
 * Returns: (transfer full): A new #HwKeystoreManager
 */
HwKeystoreManager *hw_keystore_manager_new(void);

/**
 * hw_keystore_manager_get_default:
 *
 * Gets the default singleton instance of the keystore manager.
 * Creates one if it doesn't exist.
 *
 * Returns: (transfer none): The default #HwKeystoreManager
 */
HwKeystoreManager *hw_keystore_manager_get_default(void);

/* ============================================================================
 * Hardware Detection
 * ============================================================================ */

/**
 * hw_keystore_manager_is_hardware_available:
 * @self: A #HwKeystoreManager
 *
 * Checks if hardware keystore is available on this system.
 *
 * Returns: %TRUE if hardware keystore is available
 */
gboolean hw_keystore_manager_is_hardware_available(HwKeystoreManager *self);

/**
 * hw_keystore_manager_get_hardware_info:
 * @self: A #HwKeystoreManager
 *
 * Gets information about the hardware keystore.
 *
 * Returns: (transfer full) (nullable): Hardware info, or %NULL if not available.
 *          Free with gn_hw_keystore_info_free().
 */
GnHwKeystoreInfo *hw_keystore_manager_get_hardware_info(HwKeystoreManager *self);

/**
 * hw_keystore_manager_get_backend_name:
 * @self: A #HwKeystoreManager
 *
 * Gets a human-readable name of the current backend.
 *
 * Returns: (transfer none): Backend name string
 */
const gchar *hw_keystore_manager_get_backend_name(HwKeystoreManager *self);

/* ============================================================================
 * Enable/Disable
 * ============================================================================ */

/**
 * hw_keystore_manager_get_mode:
 * @self: A #HwKeystoreManager
 *
 * Gets the current keystore mode.
 *
 * Returns: The current #HwKeystoreMode
 */
HwKeystoreMode hw_keystore_manager_get_mode(HwKeystoreManager *self);

/**
 * hw_keystore_manager_set_mode:
 * @self: A #HwKeystoreManager
 * @mode: The mode to set
 *
 * Sets the keystore mode. Changes take effect immediately.
 */
void hw_keystore_manager_set_mode(HwKeystoreManager *self, HwKeystoreMode mode);

/**
 * hw_keystore_manager_is_enabled:
 * @self: A #HwKeystoreManager
 *
 * Checks if hardware keystore is currently enabled and active.
 *
 * Returns: %TRUE if enabled and active
 */
gboolean hw_keystore_manager_is_enabled(HwKeystoreManager *self);

/**
 * hw_keystore_manager_set_enabled:
 * @self: A #HwKeystoreManager
 * @enabled: Whether to enable hardware keystore
 *
 * Convenience method to enable/disable hardware keystore.
 * Equivalent to setting mode to AUTO or DISABLED.
 */
void hw_keystore_manager_set_enabled(HwKeystoreManager *self, gboolean enabled);

/* ============================================================================
 * Master Key Management
 * ============================================================================ */

/**
 * hw_keystore_manager_get_setup_status:
 * @self: A #HwKeystoreManager
 *
 * Gets the current setup status.
 *
 * Returns: The current #HwKeystoreSetupStatus
 */
HwKeystoreSetupStatus hw_keystore_manager_get_setup_status(HwKeystoreManager *self);

/**
 * hw_keystore_manager_has_master_key:
 * @self: A #HwKeystoreManager
 *
 * Checks if a master key has been set up.
 *
 * Returns: %TRUE if master key exists
 */
gboolean hw_keystore_manager_has_master_key(HwKeystoreManager *self);

/**
 * hw_keystore_manager_setup_master_key:
 * @self: A #HwKeystoreManager
 * @error: Return location for error, or %NULL
 *
 * Creates a new master key in the hardware keystore.
 * If a master key already exists, this returns success without
 * creating a new one.
 *
 * Returns: %TRUE on success
 */
gboolean hw_keystore_manager_setup_master_key(HwKeystoreManager *self,
                                               GError **error);

/**
 * hw_keystore_manager_reset_master_key:
 * @self: A #HwKeystoreManager
 * @error: Return location for error, or %NULL
 *
 * Deletes the existing master key and creates a new one.
 * WARNING: This will make all existing derived keys unusable!
 *
 * Returns: %TRUE on success
 */
gboolean hw_keystore_manager_reset_master_key(HwKeystoreManager *self,
                                               GError **error);

/**
 * hw_keystore_manager_delete_master_key:
 * @self: A #HwKeystoreManager
 * @error: Return location for error, or %NULL
 *
 * Deletes the master key without creating a new one.
 * WARNING: This will make all existing derived keys unusable!
 *
 * Returns: %TRUE on success
 */
gboolean hw_keystore_manager_delete_master_key(HwKeystoreManager *self,
                                                GError **error);

/* ============================================================================
 * Key Derivation
 * ============================================================================ */

/**
 * hw_keystore_manager_get_signing_key:
 * @self: A #HwKeystoreManager
 * @npub: The npub to derive a signing key for
 * @private_key_out: (out) (array fixed-size=32): Output buffer for 32-byte private key
 * @error: Return location for error, or %NULL
 *
 * Derives a signing key for the given npub. The same npub always
 * produces the same key (deterministic derivation).
 *
 * The key is derived using HKDF-SHA256:
 *   signing_key = HKDF(master_key, salt=npub, info="nostr-signing-key-v1")
 *
 * Returns: %TRUE on success
 */
gboolean hw_keystore_manager_get_signing_key(HwKeystoreManager *self,
                                              const gchar *npub,
                                              guint8 *private_key_out,
                                              GError **error);

/**
 * hw_keystore_manager_get_public_key:
 * @self: A #HwKeystoreManager
 * @npub: The npub to derive a public key for
 * @public_key_out: (out) (array fixed-size=32): Output buffer for 32-byte public key
 * @error: Return location for error, or %NULL
 *
 * Derives the public key corresponding to the signing key for npub.
 *
 * Returns: %TRUE on success
 */
gboolean hw_keystore_manager_get_public_key(HwKeystoreManager *self,
                                             const gchar *npub,
                                             guint8 *public_key_out,
                                             GError **error);

/**
 * hw_keystore_manager_sign_hash:
 * @self: A #HwKeystoreManager
 * @npub: The npub to sign with
 * @hash: (array fixed-size=32): The 32-byte hash to sign
 * @signature_out: (out) (array fixed-size=64): Output buffer for 64-byte signature
 * @error: Return location for error, or %NULL
 *
 * Signs a hash using the derived signing key for npub.
 *
 * Returns: %TRUE on success
 */
gboolean hw_keystore_manager_sign_hash(HwKeystoreManager *self,
                                        const gchar *npub,
                                        const guint8 *hash,
                                        guint8 *signature_out,
                                        GError **error);

/**
 * hw_keystore_manager_sign_event:
 * @self: A #HwKeystoreManager
 * @npub: The npub to sign with
 * @event_json: The event JSON to sign
 * @error: Return location for error, or %NULL
 *
 * Signs a Nostr event using the derived signing key for npub.
 *
 * Returns: (transfer full) (nullable): Signed event JSON, or %NULL on error.
 *          Free with g_free().
 */
gchar *hw_keystore_manager_sign_event(HwKeystoreManager *self,
                                       const gchar *npub,
                                       const gchar *event_json,
                                       GError **error);

/* ============================================================================
 * Import/Export
 * ============================================================================ */

/**
 * hw_keystore_manager_can_import_existing_key:
 * @self: A #HwKeystoreManager
 *
 * Checks if the hardware keystore can import existing keys.
 * Note: Hardware keystores typically don't support this - keys
 * must be derived from the master key.
 *
 * Returns: %TRUE if import is supported
 */
gboolean hw_keystore_manager_can_import_existing_key(HwKeystoreManager *self);

/**
 * hw_keystore_manager_migrate_from_software:
 * @self: A #HwKeystoreManager
 * @npub: The npub of the key to migrate
 * @software_private_key: (array fixed-size=32): The existing private key
 * @error: Return location for error, or %NULL
 *
 * Migrates a key from software storage to hardware-backed derivation.
 * Note: The derived key will be different from the original!
 * This is primarily for record-keeping that the npub now uses
 * hardware-backed keys.
 *
 * Returns: %TRUE on success
 */
gboolean hw_keystore_manager_migrate_from_software(HwKeystoreManager *self,
                                                    const gchar *npub,
                                                    const guint8 *software_private_key,
                                                    GError **error);

/* ============================================================================
 * Settings Integration
 * ============================================================================ */

/**
 * hw_keystore_manager_load_settings:
 * @self: A #HwKeystoreManager
 *
 * Loads settings from GSettings.
 */
void hw_keystore_manager_load_settings(HwKeystoreManager *self);

/**
 * hw_keystore_manager_save_settings:
 * @self: A #HwKeystoreManager
 *
 * Saves current settings to GSettings.
 */
void hw_keystore_manager_save_settings(HwKeystoreManager *self);

/* ============================================================================
 * Provider Access
 * ============================================================================ */

/**
 * hw_keystore_manager_get_provider:
 * @self: A #HwKeystoreManager
 *
 * Gets the underlying TPM provider.
 *
 * Returns: (transfer none): The #GnHsmProviderTpm
 */
GnHsmProviderTpm *hw_keystore_manager_get_provider(HwKeystoreManager *self);

/* ============================================================================
 * Signals
 * ============================================================================ */

/**
 * HwKeystoreManager::mode-changed:
 * @manager: The #HwKeystoreManager
 * @mode: The new #HwKeystoreMode
 *
 * Emitted when the keystore mode changes.
 */

/**
 * HwKeystoreManager::setup-status-changed:
 * @manager: The #HwKeystoreManager
 * @status: The new #HwKeystoreSetupStatus
 *
 * Emitted when the setup status changes.
 */

/**
 * HwKeystoreManager::error:
 * @manager: The #HwKeystoreManager
 * @error_message: The error message
 *
 * Emitted when an error occurs.
 */

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * hw_keystore_mode_to_string:
 * @mode: A #HwKeystoreMode
 *
 * Gets a human-readable string for a mode.
 *
 * Returns: (transfer none): Mode name string
 */
const gchar *hw_keystore_mode_to_string(HwKeystoreMode mode);

/**
 * hw_keystore_setup_status_to_string:
 * @status: A #HwKeystoreSetupStatus
 *
 * Gets a human-readable string for a setup status.
 *
 * Returns: (transfer none): Status string
 */
const gchar *hw_keystore_setup_status_to_string(HwKeystoreSetupStatus status);

G_END_DECLS
