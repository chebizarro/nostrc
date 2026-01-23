/* hw_wallet_provider.h - Hardware wallet provider interface for gnostr-signer
 *
 * This module provides an abstract GObject interface for hardware wallet
 * operations, supporting Ledger and Trezor devices with Nostr signing
 * capabilities.
 *
 * Supported devices:
 *   - Ledger Nano S/X with Nostr app
 *   - Trezor Model T/One with Nostr support
 *
 * The hardware wallet provider extends GnHsmProvider to leverage the
 * existing HSM infrastructure while adding USB HID-specific functionality.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include "hsm_provider.h"

G_BEGIN_DECLS

/* ============================================================================
 * Hardware Wallet Types and Constants
 * ============================================================================ */

/**
 * GnHwWalletType:
 * @GN_HW_WALLET_TYPE_UNKNOWN: Unknown device type
 * @GN_HW_WALLET_TYPE_LEDGER_NANO_S: Ledger Nano S
 * @GN_HW_WALLET_TYPE_LEDGER_NANO_X: Ledger Nano X
 * @GN_HW_WALLET_TYPE_LEDGER_NANO_S_PLUS: Ledger Nano S Plus
 * @GN_HW_WALLET_TYPE_TREZOR_ONE: Trezor Model One
 * @GN_HW_WALLET_TYPE_TREZOR_T: Trezor Model T
 * @GN_HW_WALLET_TYPE_TREZOR_SAFE_3: Trezor Safe 3
 *
 * Supported hardware wallet device types.
 */
typedef enum {
  GN_HW_WALLET_TYPE_UNKNOWN = 0,
  GN_HW_WALLET_TYPE_LEDGER_NANO_S,
  GN_HW_WALLET_TYPE_LEDGER_NANO_X,
  GN_HW_WALLET_TYPE_LEDGER_NANO_S_PLUS,
  GN_HW_WALLET_TYPE_TREZOR_ONE,
  GN_HW_WALLET_TYPE_TREZOR_T,
  GN_HW_WALLET_TYPE_TREZOR_SAFE_3
} GnHwWalletType;

/**
 * GnHwWalletState:
 * @GN_HW_WALLET_STATE_DISCONNECTED: Device not connected
 * @GN_HW_WALLET_STATE_CONNECTED: Device connected but not ready
 * @GN_HW_WALLET_STATE_APP_CLOSED: Device connected, app not open
 * @GN_HW_WALLET_STATE_READY: Device ready for operations
 * @GN_HW_WALLET_STATE_BUSY: Device busy (user interaction needed)
 * @GN_HW_WALLET_STATE_ERROR: Device in error state
 *
 * Hardware wallet connection state.
 */
typedef enum {
  GN_HW_WALLET_STATE_DISCONNECTED = 0,
  GN_HW_WALLET_STATE_CONNECTED,
  GN_HW_WALLET_STATE_APP_CLOSED,
  GN_HW_WALLET_STATE_READY,
  GN_HW_WALLET_STATE_BUSY,
  GN_HW_WALLET_STATE_ERROR
} GnHwWalletState;

/**
 * GnHwWalletPromptType:
 * @GN_HW_WALLET_PROMPT_NONE: No prompt needed
 * @GN_HW_WALLET_PROMPT_CONFIRM_ADDRESS: Confirm address on device
 * @GN_HW_WALLET_PROMPT_CONFIRM_SIGN: Confirm signing on device
 * @GN_HW_WALLET_PROMPT_ENTER_PIN: Enter PIN on device
 * @GN_HW_WALLET_PROMPT_OPEN_APP: Open Nostr app on device
 * @GN_HW_WALLET_PROMPT_CONNECT: Connect device
 *
 * Type of user prompt required on the hardware device.
 */
typedef enum {
  GN_HW_WALLET_PROMPT_NONE = 0,
  GN_HW_WALLET_PROMPT_CONFIRM_ADDRESS,
  GN_HW_WALLET_PROMPT_CONFIRM_SIGN,
  GN_HW_WALLET_PROMPT_ENTER_PIN,
  GN_HW_WALLET_PROMPT_OPEN_APP,
  GN_HW_WALLET_PROMPT_CONNECT
} GnHwWalletPromptType;

/* USB Vendor/Product IDs */
#define GN_HW_WALLET_LEDGER_VID       0x2C97
#define GN_HW_WALLET_LEDGER_NANO_S_PID    0x0001
#define GN_HW_WALLET_LEDGER_NANO_X_PID    0x0004
#define GN_HW_WALLET_LEDGER_NANO_S_PLUS_PID 0x0005

#define GN_HW_WALLET_TREZOR_VID       0x1209
#define GN_HW_WALLET_TREZOR_ONE_PID   0x53C0
#define GN_HW_WALLET_TREZOR_T_PID     0x53C1

/* Nostr app BIP-44 derivation path: m/44'/1237'/0'/0/0 */
#define GN_HW_WALLET_NOSTR_PATH "m/44'/1237'/0'/0/0"
#define GN_HW_WALLET_NOSTR_PATH_ELEMENTS 5

/* ============================================================================
 * Error Domain
 * ============================================================================ */

/**
 * GnHwWalletError:
 * @GN_HW_WALLET_ERROR_FAILED: General failure
 * @GN_HW_WALLET_ERROR_DEVICE_NOT_FOUND: No hardware wallet connected
 * @GN_HW_WALLET_ERROR_APP_NOT_OPEN: Nostr app not open on device
 * @GN_HW_WALLET_ERROR_USER_REJECTED: User rejected on device
 * @GN_HW_WALLET_ERROR_COMMUNICATION: USB communication error
 * @GN_HW_WALLET_ERROR_TIMEOUT: Operation timed out
 * @GN_HW_WALLET_ERROR_UNSUPPORTED: Operation not supported by device
 * @GN_HW_WALLET_ERROR_LOCKED: Device is locked
 * @GN_HW_WALLET_ERROR_BUSY: Device is busy
 *
 * Error codes for hardware wallet operations.
 */
typedef enum {
  GN_HW_WALLET_ERROR_FAILED,
  GN_HW_WALLET_ERROR_DEVICE_NOT_FOUND,
  GN_HW_WALLET_ERROR_APP_NOT_OPEN,
  GN_HW_WALLET_ERROR_USER_REJECTED,
  GN_HW_WALLET_ERROR_COMMUNICATION,
  GN_HW_WALLET_ERROR_TIMEOUT,
  GN_HW_WALLET_ERROR_UNSUPPORTED,
  GN_HW_WALLET_ERROR_LOCKED,
  GN_HW_WALLET_ERROR_BUSY
} GnHwWalletError;

#define GN_HW_WALLET_ERROR (gn_hw_wallet_error_quark())
GQuark gn_hw_wallet_error_quark(void);

/* ============================================================================
 * Hardware Wallet Device Info
 * ============================================================================ */

/**
 * GnHwWalletDeviceInfo:
 * @device_id: Unique device identifier (USB path)
 * @type: Device type (Ledger/Trezor model)
 * @manufacturer: Manufacturer name
 * @product: Product name
 * @serial: Serial number (if available)
 * @firmware_version: Firmware version string
 * @state: Current device state
 * @app_name: Name of currently open app (if applicable)
 * @app_version: Version of currently open app
 * @needs_pin: Whether PIN entry is required
 * @has_nostr_app: Whether Nostr app is installed
 *
 * Information about a detected hardware wallet device.
 */
typedef struct {
  gchar *device_id;
  GnHwWalletType type;
  gchar *manufacturer;
  gchar *product;
  gchar *serial;
  gchar *firmware_version;
  GnHwWalletState state;
  gchar *app_name;
  gchar *app_version;
  gboolean needs_pin;
  gboolean has_nostr_app;
} GnHwWalletDeviceInfo;

/**
 * gn_hw_wallet_device_info_copy:
 * @info: A #GnHwWalletDeviceInfo to copy
 *
 * Creates a deep copy of device info.
 *
 * Returns: (transfer full): A new #GnHwWalletDeviceInfo
 */
GnHwWalletDeviceInfo *gn_hw_wallet_device_info_copy(const GnHwWalletDeviceInfo *info);

/**
 * gn_hw_wallet_device_info_free:
 * @info: A #GnHwWalletDeviceInfo to free
 *
 * Frees device info and all its members.
 */
void gn_hw_wallet_device_info_free(GnHwWalletDeviceInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnHwWalletDeviceInfo, gn_hw_wallet_device_info_free)

/* ============================================================================
 * Hardware Wallet Provider Interface
 * ============================================================================ */

#define GN_TYPE_HW_WALLET_PROVIDER (gn_hw_wallet_provider_get_type())

G_DECLARE_INTERFACE(GnHwWalletProvider, gn_hw_wallet_provider, GN, HW_WALLET_PROVIDER, GObject)

/**
 * GnHwWalletProviderInterface:
 * @parent_iface: Parent interface
 * @get_device_type: Get the device type this provider handles
 * @enumerate_devices: Find all connected devices of this type
 * @open_device: Open a connection to a device
 * @close_device: Close device connection
 * @get_device_state: Get current device state
 * @get_public_key: Get public key from device at given derivation path
 * @sign_hash: Sign a 32-byte hash on the device
 * @prompt_callback: Set callback for device prompts
 *
 * Interface for hardware wallet provider implementations.
 */
struct _GnHwWalletProviderInterface {
  GTypeInterface parent_iface;

  /* Provider info */
  GnHwWalletType (*get_device_type)(GnHwWalletProvider *self);

  /* Device discovery */
  GPtrArray *(*enumerate_devices)(GnHwWalletProvider *self, GError **error);

  /* Connection management */
  gboolean (*open_device)(GnHwWalletProvider *self,
                          const gchar *device_id,
                          GError **error);

  void (*close_device)(GnHwWalletProvider *self,
                       const gchar *device_id);

  GnHwWalletState (*get_device_state)(GnHwWalletProvider *self,
                                       const gchar *device_id);

  /* Key operations */
  gboolean (*get_public_key)(GnHwWalletProvider *self,
                             const gchar *device_id,
                             const gchar *derivation_path,
                             guint8 *pubkey_out,
                             gsize *pubkey_len,
                             gboolean confirm_on_device,
                             GError **error);

  gboolean (*sign_hash)(GnHwWalletProvider *self,
                        const gchar *device_id,
                        const gchar *derivation_path,
                        const guint8 *hash,
                        gsize hash_len,
                        guint8 *signature_out,
                        gsize *signature_len,
                        GError **error);

  /* Async operations */
  void (*get_public_key_async)(GnHwWalletProvider *self,
                               const gchar *device_id,
                               const gchar *derivation_path,
                               gboolean confirm_on_device,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);

  gboolean (*get_public_key_finish)(GnHwWalletProvider *self,
                                    GAsyncResult *result,
                                    guint8 *pubkey_out,
                                    gsize *pubkey_len,
                                    GError **error);

  void (*sign_hash_async)(GnHwWalletProvider *self,
                          const gchar *device_id,
                          const gchar *derivation_path,
                          const guint8 *hash,
                          gsize hash_len,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer user_data);

  gboolean (*sign_hash_finish)(GnHwWalletProvider *self,
                               GAsyncResult *result,
                               guint8 *signature_out,
                               gsize *signature_len,
                               GError **error);
};

/* ============================================================================
 * Provider Interface Methods
 * ============================================================================ */

GnHwWalletType gn_hw_wallet_provider_get_device_type(GnHwWalletProvider *self);

GPtrArray *gn_hw_wallet_provider_enumerate_devices(GnHwWalletProvider *self,
                                                    GError **error);

gboolean gn_hw_wallet_provider_open_device(GnHwWalletProvider *self,
                                           const gchar *device_id,
                                           GError **error);

void gn_hw_wallet_provider_close_device(GnHwWalletProvider *self,
                                        const gchar *device_id);

GnHwWalletState gn_hw_wallet_provider_get_device_state(GnHwWalletProvider *self,
                                                        const gchar *device_id);

gboolean gn_hw_wallet_provider_get_public_key(GnHwWalletProvider *self,
                                              const gchar *device_id,
                                              const gchar *derivation_path,
                                              guint8 *pubkey_out,
                                              gsize *pubkey_len,
                                              gboolean confirm_on_device,
                                              GError **error);

gboolean gn_hw_wallet_provider_sign_hash(GnHwWalletProvider *self,
                                         const gchar *device_id,
                                         const gchar *derivation_path,
                                         const guint8 *hash,
                                         gsize hash_len,
                                         guint8 *signature_out,
                                         gsize *signature_len,
                                         GError **error);

/* Async operations */
void gn_hw_wallet_provider_get_public_key_async(GnHwWalletProvider *self,
                                                const gchar *device_id,
                                                const gchar *derivation_path,
                                                gboolean confirm_on_device,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);

gboolean gn_hw_wallet_provider_get_public_key_finish(GnHwWalletProvider *self,
                                                     GAsyncResult *result,
                                                     guint8 *pubkey_out,
                                                     gsize *pubkey_len,
                                                     GError **error);

void gn_hw_wallet_provider_sign_hash_async(GnHwWalletProvider *self,
                                           const gchar *device_id,
                                           const gchar *derivation_path,
                                           const guint8 *hash,
                                           gsize hash_len,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);

gboolean gn_hw_wallet_provider_sign_hash_finish(GnHwWalletProvider *self,
                                                GAsyncResult *result,
                                                guint8 *signature_out,
                                                gsize *signature_len,
                                                GError **error);

/* ============================================================================
 * Hardware Wallet Manager
 * ============================================================================ */

#define GN_TYPE_HW_WALLET_MANAGER (gn_hw_wallet_manager_get_type())

G_DECLARE_FINAL_TYPE(GnHwWalletManager, gn_hw_wallet_manager, GN, HW_WALLET_MANAGER, GObject)

/**
 * gn_hw_wallet_manager_get_default:
 *
 * Gets the singleton hardware wallet manager instance.
 *
 * Returns: (transfer none): The default #GnHwWalletManager
 */
GnHwWalletManager *gn_hw_wallet_manager_get_default(void);

/**
 * gn_hw_wallet_manager_register_provider:
 * @self: A #GnHwWalletManager
 * @provider: A #GnHwWalletProvider to register
 *
 * Registers a hardware wallet provider with the manager.
 */
void gn_hw_wallet_manager_register_provider(GnHwWalletManager *self,
                                            GnHwWalletProvider *provider);

/**
 * gn_hw_wallet_manager_get_providers:
 * @self: A #GnHwWalletManager
 *
 * Gets all registered providers.
 *
 * Returns: (transfer none) (element-type GnHwWalletProvider): List of providers
 */
GList *gn_hw_wallet_manager_get_providers(GnHwWalletManager *self);

/**
 * gn_hw_wallet_manager_enumerate_all_devices:
 * @self: A #GnHwWalletManager
 * @error: Return location for error, or %NULL
 *
 * Enumerates all connected hardware wallet devices across all providers.
 *
 * Returns: (transfer full) (element-type GnHwWalletDeviceInfo): Array of devices
 */
GPtrArray *gn_hw_wallet_manager_enumerate_all_devices(GnHwWalletManager *self,
                                                       GError **error);

/**
 * gn_hw_wallet_manager_start_monitoring:
 * @self: A #GnHwWalletManager
 *
 * Starts monitoring for device connect/disconnect events.
 */
void gn_hw_wallet_manager_start_monitoring(GnHwWalletManager *self);

/**
 * gn_hw_wallet_manager_stop_monitoring:
 * @self: A #GnHwWalletManager
 *
 * Stops monitoring for device events.
 */
void gn_hw_wallet_manager_stop_monitoring(GnHwWalletManager *self);

/**
 * gn_hw_wallet_manager_get_provider_for_device:
 * @self: A #GnHwWalletManager
 * @device_id: Device identifier
 *
 * Gets the provider that handles a specific device.
 *
 * Returns: (transfer none) (nullable): The provider, or %NULL
 */
GnHwWalletProvider *gn_hw_wallet_manager_get_provider_for_device(GnHwWalletManager *self,
                                                                  const gchar *device_id);

/* ============================================================================
 * Prompt Callback
 * ============================================================================ */

/**
 * GnHwWalletPromptCallback:
 * @prompt_type: Type of prompt required
 * @device_info: Device requiring the prompt
 * @message: Human-readable message to display
 * @user_data: User data
 *
 * Callback invoked when user interaction is needed on the hardware device.
 */
typedef void (*GnHwWalletPromptCallback)(GnHwWalletPromptType prompt_type,
                                          const GnHwWalletDeviceInfo *device_info,
                                          const gchar *message,
                                          gpointer user_data);

/**
 * gn_hw_wallet_manager_set_prompt_callback:
 * @self: A #GnHwWalletManager
 * @callback: Callback function
 * @user_data: Data for callback
 *
 * Sets the callback for device prompts.
 */
void gn_hw_wallet_manager_set_prompt_callback(GnHwWalletManager *self,
                                              GnHwWalletPromptCallback callback,
                                              gpointer user_data);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * gn_hw_wallet_type_to_string:
 * @type: A #GnHwWalletType
 *
 * Gets a human-readable name for the device type.
 *
 * Returns: (transfer none): Device type name
 */
const gchar *gn_hw_wallet_type_to_string(GnHwWalletType type);

/**
 * gn_hw_wallet_state_to_string:
 * @state: A #GnHwWalletState
 *
 * Gets a human-readable name for the device state.
 *
 * Returns: (transfer none): State name
 */
const gchar *gn_hw_wallet_state_to_string(GnHwWalletState state);

/**
 * gn_hw_wallet_type_is_ledger:
 * @type: A #GnHwWalletType
 *
 * Checks if the type is a Ledger device.
 *
 * Returns: %TRUE if Ledger device
 */
gboolean gn_hw_wallet_type_is_ledger(GnHwWalletType type);

/**
 * gn_hw_wallet_type_is_trezor:
 * @type: A #GnHwWalletType
 *
 * Checks if the type is a Trezor device.
 *
 * Returns: %TRUE if Trezor device
 */
gboolean gn_hw_wallet_type_is_trezor(GnHwWalletType type);

/**
 * gn_hw_wallet_providers_init:
 *
 * Initialize and register all built-in hardware wallet providers.
 * Should be called once during application startup.
 */
void gn_hw_wallet_providers_init(void);

/* ============================================================================
 * Signals (emitted by GnHwWalletManager)
 * ============================================================================ */

/**
 * GnHwWalletManager::device-connected:
 * @manager: The #GnHwWalletManager
 * @device_info: The #GnHwWalletDeviceInfo for the connected device
 *
 * Emitted when a hardware wallet device is connected.
 */

/**
 * GnHwWalletManager::device-disconnected:
 * @manager: The #GnHwWalletManager
 * @device_id: The device ID of the disconnected device
 *
 * Emitted when a hardware wallet device is disconnected.
 */

/**
 * GnHwWalletManager::device-state-changed:
 * @manager: The #GnHwWalletManager
 * @device_id: The device ID
 * @state: The new #GnHwWalletState
 *
 * Emitted when a device's state changes.
 */

/**
 * GnHwWalletManager::prompt-required:
 * @manager: The #GnHwWalletManager
 * @prompt_type: The #GnHwWalletPromptType
 * @device_info: The device requiring prompt
 * @message: The prompt message
 *
 * Emitted when user interaction is needed on a device.
 */

G_END_DECLS
