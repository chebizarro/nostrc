/* hsm_provider.h - Hardware Security Module provider interface
 *
 * This module provides an abstract GObject interface for HSM operations
 * in gnostr-signer. HSM providers implement key storage and signing
 * operations using hardware security modules.
 *
 * Supported backends:
 *   - Mock provider (for testing)
 *   - PKCS#11 provider (using p11-kit)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* ============================================================================
 * Error Domain
 * ============================================================================ */

/**
 * GnHsmError:
 * @GN_HSM_ERROR_FAILED: General failure
 * @GN_HSM_ERROR_NOT_AVAILABLE: HSM backend not available
 * @GN_HSM_ERROR_NOT_FOUND: Key or device not found
 * @GN_HSM_ERROR_PERMISSION_DENIED: Access denied (PIN required/wrong)
 * @GN_HSM_ERROR_DEVICE_ERROR: Hardware device error
 * @GN_HSM_ERROR_KEY_GENERATION_FAILED: Key generation failed
 * @GN_HSM_ERROR_SIGNING_FAILED: Signing operation failed
 * @GN_HSM_ERROR_ALREADY_EXISTS: Key with same ID already exists
 * @GN_HSM_ERROR_PIN_REQUIRED: PIN/passphrase required
 * @GN_HSM_ERROR_PIN_INCORRECT: PIN/passphrase incorrect
 * @GN_HSM_ERROR_PIN_LOCKED: PIN locked after too many attempts
 * @GN_HSM_ERROR_NOT_INITIALIZED: Provider not initialized
 * @GN_HSM_ERROR_DEVICE_REMOVED: Device was removed during operation
 *
 * Error codes for HSM operations.
 */
typedef enum {
  GN_HSM_ERROR_FAILED,
  GN_HSM_ERROR_NOT_AVAILABLE,
  GN_HSM_ERROR_NOT_FOUND,
  GN_HSM_ERROR_PERMISSION_DENIED,
  GN_HSM_ERROR_DEVICE_ERROR,
  GN_HSM_ERROR_KEY_GENERATION_FAILED,
  GN_HSM_ERROR_SIGNING_FAILED,
  GN_HSM_ERROR_ALREADY_EXISTS,
  GN_HSM_ERROR_PIN_REQUIRED,
  GN_HSM_ERROR_PIN_INCORRECT,
  GN_HSM_ERROR_PIN_LOCKED,
  GN_HSM_ERROR_NOT_INITIALIZED,
  GN_HSM_ERROR_DEVICE_REMOVED
} GnHsmError;

#define GN_HSM_ERROR (gn_hsm_error_quark())
GQuark gn_hsm_error_quark(void);

/* ============================================================================
 * Device Information
 * ============================================================================ */

/**
 * GnHsmDeviceInfo:
 * @slot_id: Device slot identifier
 * @label: User-readable device label
 * @manufacturer: Device manufacturer name
 * @model: Device model name
 * @serial: Device serial number
 * @flags: Device capability flags
 * @is_token_present: Whether a token is present in the slot
 * @is_initialized: Whether the token is initialized
 * @needs_pin: Whether PIN is required for operations
 *
 * Information about a detected HSM device.
 */
typedef struct {
  guint64 slot_id;
  gchar *label;
  gchar *manufacturer;
  gchar *model;
  gchar *serial;
  guint32 flags;
  gboolean is_token_present;
  gboolean is_initialized;
  gboolean needs_pin;
} GnHsmDeviceInfo;

/**
 * gn_hsm_device_info_copy:
 * @info: A #GnHsmDeviceInfo to copy
 *
 * Creates a deep copy of a device info structure.
 *
 * Returns: (transfer full): A new #GnHsmDeviceInfo. Free with gn_hsm_device_info_free().
 */
GnHsmDeviceInfo *gn_hsm_device_info_copy(const GnHsmDeviceInfo *info);

/**
 * gn_hsm_device_info_free:
 * @info: A #GnHsmDeviceInfo to free
 *
 * Frees a #GnHsmDeviceInfo structure and all its members.
 */
void gn_hsm_device_info_free(GnHsmDeviceInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnHsmDeviceInfo, gn_hsm_device_info_free)

/* ============================================================================
 * Key Information
 * ============================================================================ */

/**
 * GnHsmKeyType:
 * @GN_HSM_KEY_TYPE_UNKNOWN: Unknown key type
 * @GN_HSM_KEY_TYPE_SECP256K1: secp256k1 key (Nostr/Bitcoin)
 * @GN_HSM_KEY_TYPE_ED25519: Ed25519 key
 *
 * Types of cryptographic keys supported by HSM providers.
 */
typedef enum {
  GN_HSM_KEY_TYPE_UNKNOWN = 0,
  GN_HSM_KEY_TYPE_SECP256K1,
  GN_HSM_KEY_TYPE_ED25519
} GnHsmKeyType;

/**
 * GnHsmKeyInfo:
 * @key_id: Unique key identifier within the HSM
 * @label: User-defined label for the key
 * @npub: Public key in bech32 (npub1...) format
 * @pubkey_hex: Public key in 64-char hex format
 * @key_type: Type of cryptographic key
 * @created_at: ISO 8601 timestamp of creation (if available)
 * @slot_id: Slot ID where key resides
 * @can_sign: Whether key can be used for signing
 * @is_extractable: Whether private key can be exported (should be FALSE for HSM)
 *
 * Information about a key stored in an HSM.
 */
typedef struct {
  gchar *key_id;
  gchar *label;
  gchar *npub;
  gchar *pubkey_hex;
  GnHsmKeyType key_type;
  gchar *created_at;
  guint64 slot_id;
  gboolean can_sign;
  gboolean is_extractable;
} GnHsmKeyInfo;

/**
 * gn_hsm_key_info_copy:
 * @info: A #GnHsmKeyInfo to copy
 *
 * Creates a deep copy of a key info structure.
 *
 * Returns: (transfer full): A new #GnHsmKeyInfo. Free with gn_hsm_key_info_free().
 */
GnHsmKeyInfo *gn_hsm_key_info_copy(const GnHsmKeyInfo *info);

/**
 * gn_hsm_key_info_free:
 * @info: A #GnHsmKeyInfo to free
 *
 * Frees a #GnHsmKeyInfo structure and all its members.
 */
void gn_hsm_key_info_free(GnHsmKeyInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnHsmKeyInfo, gn_hsm_key_info_free)

/* ============================================================================
 * HSM Provider Interface
 * ============================================================================ */

#define GN_TYPE_HSM_PROVIDER (gn_hsm_provider_get_type())

G_DECLARE_INTERFACE(GnHsmProvider, gn_hsm_provider, GN, HSM_PROVIDER, GObject)

/**
 * GnHsmProviderInterface:
 * @parent_iface: Parent interface
 * @get_name: Get provider name
 * @is_available: Check if provider is available
 * @init_provider: Initialize the provider
 * @shutdown_provider: Shutdown the provider
 * @detect_devices: Detect available HSM devices
 * @list_keys: List keys on a device
 * @get_public_key: Get public key for a key ID
 * @sign_hash: Sign a 32-byte hash
 * @sign_event: Sign a Nostr event (computes hash internally)
 * @generate_key: Generate a new key pair on device
 * @import_key: Import an existing key to device
 * @delete_key: Delete a key from device
 * @login: Authenticate with PIN
 * @logout: End authenticated session
 *
 * Interface for HSM provider implementations.
 */
struct _GnHsmProviderInterface {
  GTypeInterface parent_iface;

  /* Provider info */
  const gchar *(*get_name)(GnHsmProvider *self);
  gboolean (*is_available)(GnHsmProvider *self);

  /* Lifecycle */
  gboolean (*init_provider)(GnHsmProvider *self, GError **error);
  void (*shutdown_provider)(GnHsmProvider *self);

  /* Device operations */
  GPtrArray *(*detect_devices)(GnHsmProvider *self, GError **error);

  /* Key operations */
  GPtrArray *(*list_keys)(GnHsmProvider *self,
                          guint64 slot_id,
                          GError **error);

  GnHsmKeyInfo *(*get_public_key)(GnHsmProvider *self,
                                   guint64 slot_id,
                                   const gchar *key_id,
                                   GError **error);

  gboolean (*sign_hash)(GnHsmProvider *self,
                        guint64 slot_id,
                        const gchar *key_id,
                        const guint8 *hash,
                        gsize hash_len,
                        guint8 *signature,
                        gsize *signature_len,
                        GError **error);

  gchar *(*sign_event)(GnHsmProvider *self,
                       guint64 slot_id,
                       const gchar *key_id,
                       const gchar *event_json,
                       GError **error);

  GnHsmKeyInfo *(*generate_key)(GnHsmProvider *self,
                                 guint64 slot_id,
                                 const gchar *label,
                                 GnHsmKeyType key_type,
                                 GError **error);

  gboolean (*import_key)(GnHsmProvider *self,
                         guint64 slot_id,
                         const gchar *label,
                         const guint8 *private_key,
                         gsize key_len,
                         GnHsmKeyInfo **out_info,
                         GError **error);

  gboolean (*delete_key)(GnHsmProvider *self,
                         guint64 slot_id,
                         const gchar *key_id,
                         GError **error);

  /* Authentication */
  gboolean (*login)(GnHsmProvider *self,
                    guint64 slot_id,
                    const gchar *pin,
                    GError **error);

  void (*logout)(GnHsmProvider *self, guint64 slot_id);

  /* Async variants (optional) */
  void (*detect_devices_async)(GnHsmProvider *self,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);

  GPtrArray *(*detect_devices_finish)(GnHsmProvider *self,
                                      GAsyncResult *result,
                                      GError **error);

  void (*sign_event_async)(GnHsmProvider *self,
                           guint64 slot_id,
                           const gchar *key_id,
                           const gchar *event_json,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data);

  gchar *(*sign_event_finish)(GnHsmProvider *self,
                              GAsyncResult *result,
                              GError **error);
};

/* ============================================================================
 * Provider Interface Methods
 * ============================================================================ */

/**
 * gn_hsm_provider_get_name:
 * @self: A #GnHsmProvider
 *
 * Gets the human-readable name of this provider.
 *
 * Returns: (transfer none): Provider name string
 */
const gchar *gn_hsm_provider_get_name(GnHsmProvider *self);

/**
 * gn_hsm_provider_is_available:
 * @self: A #GnHsmProvider
 *
 * Checks if this provider is available on the current system.
 *
 * Returns: %TRUE if available, %FALSE otherwise
 */
gboolean gn_hsm_provider_is_available(GnHsmProvider *self);

/**
 * gn_hsm_provider_init:
 * @self: A #GnHsmProvider
 * @error: Return location for error, or %NULL
 *
 * Initializes the HSM provider. Must be called before other operations.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_hsm_provider_init(GnHsmProvider *self, GError **error);

/**
 * gn_hsm_provider_shutdown:
 * @self: A #GnHsmProvider
 *
 * Shuts down the HSM provider and releases resources.
 */
void gn_hsm_provider_shutdown(GnHsmProvider *self);

/**
 * gn_hsm_provider_detect_devices:
 * @self: A #GnHsmProvider
 * @error: Return location for error, or %NULL
 *
 * Detects available HSM devices.
 *
 * Returns: (transfer full) (element-type GnHsmDeviceInfo): Array of device
 *          info structures. Free with g_ptr_array_unref().
 */
GPtrArray *gn_hsm_provider_detect_devices(GnHsmProvider *self, GError **error);

/**
 * gn_hsm_provider_list_keys:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot to query
 * @error: Return location for error, or %NULL
 *
 * Lists all keys available on a device slot.
 *
 * Returns: (transfer full) (element-type GnHsmKeyInfo): Array of key info
 *          structures. Free with g_ptr_array_unref().
 */
GPtrArray *gn_hsm_provider_list_keys(GnHsmProvider *self,
                                     guint64 slot_id,
                                     GError **error);

/**
 * gn_hsm_provider_get_public_key:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot containing the key
 * @key_id: Key identifier
 * @error: Return location for error, or %NULL
 *
 * Gets the public key information for a specific key.
 *
 * Returns: (transfer full): Key info structure, or %NULL on error.
 *          Free with gn_hsm_key_info_free().
 */
GnHsmKeyInfo *gn_hsm_provider_get_public_key(GnHsmProvider *self,
                                              guint64 slot_id,
                                              const gchar *key_id,
                                              GError **error);

/**
 * gn_hsm_provider_sign_hash:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot containing the key
 * @key_id: Key identifier
 * @hash: 32-byte hash to sign
 * @hash_len: Length of hash (must be 32)
 * @signature: (out): Buffer to receive signature (64 bytes for secp256k1)
 * @signature_len: (inout): On input, size of buffer; on output, actual length
 * @error: Return location for error, or %NULL
 *
 * Signs a pre-computed hash using the specified key.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_hsm_provider_sign_hash(GnHsmProvider *self,
                                   guint64 slot_id,
                                   const gchar *key_id,
                                   const guint8 *hash,
                                   gsize hash_len,
                                   guint8 *signature,
                                   gsize *signature_len,
                                   GError **error);

/**
 * gn_hsm_provider_sign_event:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot containing the key
 * @key_id: Key identifier
 * @event_json: Unsigned Nostr event JSON
 * @error: Return location for error, or %NULL
 *
 * Signs a Nostr event using the specified key. The provider computes
 * the event hash according to NIP-01 and signs it.
 *
 * Returns: (transfer full): Signed event JSON with id and sig fields,
 *          or %NULL on error. Free with g_free().
 */
gchar *gn_hsm_provider_sign_event(GnHsmProvider *self,
                                  guint64 slot_id,
                                  const gchar *key_id,
                                  const gchar *event_json,
                                  GError **error);

/**
 * gn_hsm_provider_generate_key:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot to store the key
 * @label: User-defined label for the key
 * @key_type: Type of key to generate
 * @error: Return location for error, or %NULL
 *
 * Generates a new key pair on the device.
 *
 * Returns: (transfer full): Key info for the new key, or %NULL on error.
 *          Free with gn_hsm_key_info_free().
 */
GnHsmKeyInfo *gn_hsm_provider_generate_key(GnHsmProvider *self,
                                            guint64 slot_id,
                                            const gchar *label,
                                            GnHsmKeyType key_type,
                                            GError **error);

/**
 * gn_hsm_provider_import_key:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot to store the key
 * @label: User-defined label for the key
 * @private_key: Raw private key bytes
 * @key_len: Length of private key (32 for secp256k1)
 * @out_info: (out) (optional): Location to store key info
 * @error: Return location for error, or %NULL
 *
 * Imports an existing private key to the device.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_hsm_provider_import_key(GnHsmProvider *self,
                                    guint64 slot_id,
                                    const gchar *label,
                                    const guint8 *private_key,
                                    gsize key_len,
                                    GnHsmKeyInfo **out_info,
                                    GError **error);

/**
 * gn_hsm_provider_delete_key:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot containing the key
 * @key_id: Key identifier
 * @error: Return location for error, or %NULL
 *
 * Deletes a key from the device.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_hsm_provider_delete_key(GnHsmProvider *self,
                                    guint64 slot_id,
                                    const gchar *key_id,
                                    GError **error);

/**
 * gn_hsm_provider_login:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot to authenticate
 * @pin: PIN or passphrase
 * @error: Return location for error, or %NULL
 *
 * Authenticates with the HSM device using a PIN.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_hsm_provider_login(GnHsmProvider *self,
                               guint64 slot_id,
                               const gchar *pin,
                               GError **error);

/**
 * gn_hsm_provider_logout:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot to log out
 *
 * Ends an authenticated session with the HSM.
 */
void gn_hsm_provider_logout(GnHsmProvider *self, guint64 slot_id);

/* ============================================================================
 * Async Operations
 * ============================================================================ */

/**
 * gn_hsm_provider_detect_devices_async:
 * @self: A #GnHsmProvider
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when complete
 * @user_data: User data for callback
 *
 * Asynchronously detects available HSM devices.
 */
void gn_hsm_provider_detect_devices_async(GnHsmProvider *self,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

/**
 * gn_hsm_provider_detect_devices_finish:
 * @self: A #GnHsmProvider
 * @result: A #GAsyncResult
 * @error: Return location for error, or %NULL
 *
 * Finishes an async detect_devices operation.
 *
 * Returns: (transfer full) (element-type GnHsmDeviceInfo): Array of devices
 */
GPtrArray *gn_hsm_provider_detect_devices_finish(GnHsmProvider *self,
                                                 GAsyncResult *result,
                                                 GError **error);

/**
 * gn_hsm_provider_sign_event_async:
 * @self: A #GnHsmProvider
 * @slot_id: Device slot containing the key
 * @key_id: Key identifier
 * @event_json: Unsigned Nostr event JSON
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when complete
 * @user_data: User data for callback
 *
 * Asynchronously signs a Nostr event.
 */
void gn_hsm_provider_sign_event_async(GnHsmProvider *self,
                                      guint64 slot_id,
                                      const gchar *key_id,
                                      const gchar *event_json,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/**
 * gn_hsm_provider_sign_event_finish:
 * @self: A #GnHsmProvider
 * @result: A #GAsyncResult
 * @error: Return location for error, or %NULL
 *
 * Finishes an async sign_event operation.
 *
 * Returns: (transfer full): Signed event JSON, or %NULL on error
 */
gchar *gn_hsm_provider_sign_event_finish(GnHsmProvider *self,
                                         GAsyncResult *result,
                                         GError **error);

/* ============================================================================
 * HSM Manager (Provider Registry)
 * ============================================================================ */

#define GN_TYPE_HSM_MANAGER (gn_hsm_manager_get_type())

G_DECLARE_FINAL_TYPE(GnHsmManager, gn_hsm_manager, GN, HSM_MANAGER, GObject)

/**
 * gn_hsm_manager_get_default:
 *
 * Gets the singleton HSM manager instance.
 *
 * Returns: (transfer none): The default #GnHsmManager
 */
GnHsmManager *gn_hsm_manager_get_default(void);

/**
 * gn_hsm_manager_register_provider:
 * @self: A #GnHsmManager
 * @provider: A #GnHsmProvider to register
 *
 * Registers an HSM provider with the manager.
 */
void gn_hsm_manager_register_provider(GnHsmManager *self,
                                      GnHsmProvider *provider);

/**
 * gn_hsm_manager_unregister_provider:
 * @self: A #GnHsmManager
 * @provider: A #GnHsmProvider to unregister
 *
 * Unregisters an HSM provider from the manager.
 */
void gn_hsm_manager_unregister_provider(GnHsmManager *self,
                                        GnHsmProvider *provider);

/**
 * gn_hsm_manager_get_providers:
 * @self: A #GnHsmManager
 *
 * Gets all registered providers.
 *
 * Returns: (transfer none) (element-type GnHsmProvider): List of providers
 */
GList *gn_hsm_manager_get_providers(GnHsmManager *self);

/**
 * gn_hsm_manager_get_available_providers:
 * @self: A #GnHsmManager
 *
 * Gets providers that are available on the current system.
 *
 * Returns: (transfer container) (element-type GnHsmProvider): List of available
 *          providers. Free with g_list_free().
 */
GList *gn_hsm_manager_get_available_providers(GnHsmManager *self);

/**
 * gn_hsm_manager_get_provider_by_name:
 * @self: A #GnHsmManager
 * @name: Provider name
 *
 * Gets a provider by name.
 *
 * Returns: (transfer none) (nullable): The provider, or %NULL if not found
 */
GnHsmProvider *gn_hsm_manager_get_provider_by_name(GnHsmManager *self,
                                                    const gchar *name);

/* ============================================================================
 * Signals
 * ============================================================================ */

/**
 * GnHsmManager::device-added:
 * @manager: The #GnHsmManager
 * @provider: The #GnHsmProvider that detected the device
 * @device_info: The #GnHsmDeviceInfo for the new device
 *
 * Emitted when a new HSM device is detected.
 */

/**
 * GnHsmManager::device-removed:
 * @manager: The #GnHsmManager
 * @provider: The #GnHsmProvider that owned the device
 * @slot_id: The slot ID of the removed device
 *
 * Emitted when an HSM device is removed.
 */

G_END_DECLS
