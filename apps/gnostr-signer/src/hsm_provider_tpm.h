/* hsm_provider_tpm.h - TPM/Secure Enclave HSM provider
 *
 * This module provides hardware-backed keystore support using platform
 * secure enclaves:
 *   - Linux: TPM 2.0 via tpm2-tss
 *   - macOS: Secure Enclave via Security.framework
 *   - Windows: TPM via Windows CNG
 *
 * Design:
 * The provider stores a master key in the hardware secure enclave.
 * Signing keys are derived from this master key using HKDF.
 * This approach works around the limitation that most TPMs/Secure Enclaves
 * don't natively support secp256k1 curves used by Nostr.
 *
 * Key derivation:
 *   master_key -> HKDF(SHA256, salt=npub, info="nostr-signing-key") -> signing_key
 *
 * Fallback:
 * If hardware is unavailable, the provider falls back to software keystore
 * using libsecret/Keychain with the same key derivation scheme.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "hsm_provider.h"

G_BEGIN_DECLS

/* ============================================================================
 * Hardware Keystore Backend Types
 * ============================================================================ */

/**
 * GnHwKeystoreBackend:
 * @GN_HW_KEYSTORE_NONE: No hardware keystore available
 * @GN_HW_KEYSTORE_TPM: Linux TPM 2.0
 * @GN_HW_KEYSTORE_SECURE_ENCLAVE: macOS Secure Enclave
 * @GN_HW_KEYSTORE_CNG: Windows CNG (TPM via Windows APIs)
 * @GN_HW_KEYSTORE_SOFTWARE: Software fallback (libsecret/Keychain)
 *
 * Available hardware keystore backends.
 */
typedef enum {
  GN_HW_KEYSTORE_NONE = 0,
  GN_HW_KEYSTORE_TPM,
  GN_HW_KEYSTORE_SECURE_ENCLAVE,
  GN_HW_KEYSTORE_CNG,
  GN_HW_KEYSTORE_SOFTWARE
} GnHwKeystoreBackend;

/**
 * GnHwKeystoreStatus:
 * @GN_HW_KEYSTORE_STATUS_UNKNOWN: Status not yet determined
 * @GN_HW_KEYSTORE_STATUS_AVAILABLE: Hardware keystore is available and ready
 * @GN_HW_KEYSTORE_STATUS_UNAVAILABLE: Hardware keystore is not available
 * @GN_HW_KEYSTORE_STATUS_DISABLED: Hardware keystore is disabled by user
 * @GN_HW_KEYSTORE_STATUS_ERROR: Error accessing hardware keystore
 * @GN_HW_KEYSTORE_STATUS_FALLBACK: Using software fallback
 *
 * Hardware keystore availability status.
 */
typedef enum {
  GN_HW_KEYSTORE_STATUS_UNKNOWN = 0,
  GN_HW_KEYSTORE_STATUS_AVAILABLE,
  GN_HW_KEYSTORE_STATUS_UNAVAILABLE,
  GN_HW_KEYSTORE_STATUS_DISABLED,
  GN_HW_KEYSTORE_STATUS_ERROR,
  GN_HW_KEYSTORE_STATUS_FALLBACK
} GnHwKeystoreStatus;

/* ============================================================================
 * Hardware Keystore Info
 * ============================================================================ */

/**
 * GnHwKeystoreInfo:
 * @backend: The active backend type
 * @status: Current status
 * @backend_name: Human-readable backend name
 * @backend_version: Backend version string (if available)
 * @has_master_key: Whether a master key is stored
 * @master_key_id: Identifier of the stored master key
 * @tpm_manufacturer: TPM manufacturer (Linux/Windows only)
 * @tpm_version: TPM version (Linux/Windows only)
 * @enclave_supported: Whether Secure Enclave is supported (macOS only)
 *
 * Information about the hardware keystore.
 */
typedef struct {
  GnHwKeystoreBackend backend;
  GnHwKeystoreStatus status;
  gchar *backend_name;
  gchar *backend_version;
  gboolean has_master_key;
  gchar *master_key_id;
  gchar *tpm_manufacturer;
  gchar *tpm_version;
  gboolean enclave_supported;
} GnHwKeystoreInfo;

/**
 * gn_hw_keystore_info_copy:
 * @info: A #GnHwKeystoreInfo to copy
 *
 * Creates a deep copy of a keystore info structure.
 *
 * Returns: (transfer full): A new #GnHwKeystoreInfo. Free with gn_hw_keystore_info_free().
 */
GnHwKeystoreInfo *gn_hw_keystore_info_copy(const GnHwKeystoreInfo *info);

/**
 * gn_hw_keystore_info_free:
 * @info: A #GnHwKeystoreInfo to free
 *
 * Frees a #GnHwKeystoreInfo structure and all its members.
 */
void gn_hw_keystore_info_free(GnHwKeystoreInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnHwKeystoreInfo, gn_hw_keystore_info_free)

/* ============================================================================
 * TPM/Secure Enclave Provider
 * ============================================================================ */

#define GN_TYPE_HSM_PROVIDER_TPM (gn_hsm_provider_tpm_get_type())

G_DECLARE_FINAL_TYPE(GnHsmProviderTpm, gn_hsm_provider_tpm, GN, HSM_PROVIDER_TPM, GObject)

/**
 * gn_hsm_provider_tpm_new:
 *
 * Creates a new TPM/Secure Enclave HSM provider instance.
 * The provider will automatically detect the available hardware backend.
 *
 * Returns: (transfer full): A new #GnHsmProviderTpm
 */
GnHsmProviderTpm *gn_hsm_provider_tpm_new(void);

/**
 * gn_hsm_provider_tpm_get_keystore_info:
 * @self: A #GnHsmProviderTpm
 *
 * Gets information about the hardware keystore.
 *
 * Returns: (transfer full): Keystore info. Free with gn_hw_keystore_info_free().
 */
GnHwKeystoreInfo *gn_hsm_provider_tpm_get_keystore_info(GnHsmProviderTpm *self);

/**
 * gn_hsm_provider_tpm_get_backend:
 * @self: A #GnHsmProviderTpm
 *
 * Gets the active backend type.
 *
 * Returns: The active #GnHwKeystoreBackend
 */
GnHwKeystoreBackend gn_hsm_provider_tpm_get_backend(GnHsmProviderTpm *self);

/**
 * gn_hsm_provider_tpm_get_status:
 * @self: A #GnHsmProviderTpm
 *
 * Gets the current status of the hardware keystore.
 *
 * Returns: The current #GnHwKeystoreStatus
 */
GnHwKeystoreStatus gn_hsm_provider_tpm_get_status(GnHsmProviderTpm *self);

/**
 * gn_hsm_provider_tpm_has_master_key:
 * @self: A #GnHsmProviderTpm
 *
 * Checks if a master key is stored in the hardware keystore.
 *
 * Returns: %TRUE if a master key is stored
 */
gboolean gn_hsm_provider_tpm_has_master_key(GnHsmProviderTpm *self);

/**
 * gn_hsm_provider_tpm_create_master_key:
 * @self: A #GnHsmProviderTpm
 * @error: Return location for error, or %NULL
 *
 * Creates a new master key in the hardware keystore. This should only
 * be called once during initial setup. The master key is used to derive
 * all signing keys.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_hsm_provider_tpm_create_master_key(GnHsmProviderTpm *self,
                                                GError **error);

/**
 * gn_hsm_provider_tpm_delete_master_key:
 * @self: A #GnHsmProviderTpm
 * @error: Return location for error, or %NULL
 *
 * Deletes the master key from the hardware keystore. This will make
 * all derived signing keys unusable. Use with caution.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_hsm_provider_tpm_delete_master_key(GnHsmProviderTpm *self,
                                                GError **error);

/**
 * gn_hsm_provider_tpm_derive_signing_key:
 * @self: A #GnHsmProviderTpm
 * @npub: The npub to derive a key for
 * @private_key_out: (out) (array fixed-size=32): Output buffer for 32-byte private key
 * @error: Return location for error, or %NULL
 *
 * Derives a signing key from the master key for a specific npub.
 * The same npub always produces the same derived key.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_hsm_provider_tpm_derive_signing_key(GnHsmProviderTpm *self,
                                                 const gchar *npub,
                                                 guint8 *private_key_out,
                                                 GError **error);

/**
 * gn_hsm_provider_tpm_set_fallback_enabled:
 * @self: A #GnHsmProviderTpm
 * @enabled: Whether to enable software fallback
 *
 * Enables or disables software fallback when hardware is unavailable.
 * Default: enabled
 */
void gn_hsm_provider_tpm_set_fallback_enabled(GnHsmProviderTpm *self,
                                               gboolean enabled);

/**
 * gn_hsm_provider_tpm_get_fallback_enabled:
 * @self: A #GnHsmProviderTpm
 *
 * Gets whether software fallback is enabled.
 *
 * Returns: %TRUE if fallback is enabled
 */
gboolean gn_hsm_provider_tpm_get_fallback_enabled(GnHsmProviderTpm *self);

/**
 * gn_hsm_provider_tpm_is_using_fallback:
 * @self: A #GnHsmProviderTpm
 *
 * Checks if the provider is currently using software fallback.
 *
 * Returns: %TRUE if using software fallback
 */
gboolean gn_hsm_provider_tpm_is_using_fallback(GnHsmProviderTpm *self);

/* ============================================================================
 * Platform Detection
 * ============================================================================ */

/**
 * gn_hw_keystore_detect_backend:
 *
 * Detects the available hardware keystore backend on the current platform.
 *
 * Returns: The detected #GnHwKeystoreBackend
 */
GnHwKeystoreBackend gn_hw_keystore_detect_backend(void);

/**
 * gn_hw_keystore_backend_to_string:
 * @backend: A #GnHwKeystoreBackend
 *
 * Gets a human-readable string for a backend type.
 *
 * Returns: (transfer none): Backend name string
 */
const gchar *gn_hw_keystore_backend_to_string(GnHwKeystoreBackend backend);

/**
 * gn_hw_keystore_status_to_string:
 * @status: A #GnHwKeystoreStatus
 *
 * Gets a human-readable string for a status.
 *
 * Returns: (transfer none): Status string
 */
const gchar *gn_hw_keystore_status_to_string(GnHwKeystoreStatus status);

/**
 * gn_hw_keystore_is_supported:
 *
 * Checks if any hardware keystore backend is supported on this system.
 *
 * Returns: %TRUE if hardware keystore is supported
 */
gboolean gn_hw_keystore_is_supported(void);

/* ============================================================================
 * Signals
 * ============================================================================ */

/**
 * GnHsmProviderTpm::status-changed:
 * @provider: The #GnHsmProviderTpm
 * @status: The new #GnHwKeystoreStatus
 *
 * Emitted when the hardware keystore status changes.
 */

/**
 * GnHsmProviderTpm::master-key-changed:
 * @provider: The #GnHsmProviderTpm
 * @has_master_key: Whether a master key is now present
 *
 * Emitted when the master key is created or deleted.
 */

G_END_DECLS
