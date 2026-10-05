/* hsm_provider_pkcs11.h - PKCS#11 HSM provider
 *
 * This module provides HSM support via the PKCS#11 standard interface.
 * It uses p11-kit for module loading and token discovery.
 *
 * Supported PKCS#11 tokens:
 *   - YubiKey (with PIV or OpenPGP applet)
 *   - Nitrokey
 *   - SoftHSM (for testing)
 *   - Any PKCS#11 compatible device with secp256k1 support
 *
 * Note: Most PKCS#11 tokens don't natively support secp256k1.
 * This implementation handles that by:
 *   1. Looking for tokens with raw ECDSA signing capability
 *   2. Falling back to secure key storage with software signing
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_HSM_PROVIDER_PKCS11_H
#define APPS_GNOSTR_SIGNER_HSM_PROVIDER_PKCS11_H

#include "hsm_provider.h"

G_BEGIN_DECLS

#define GN_TYPE_HSM_PROVIDER_PKCS11 (gn_hsm_provider_pkcs11_get_type())

G_DECLARE_FINAL_TYPE(GnHsmProviderPkcs11, gn_hsm_provider_pkcs11, GN, HSM_PROVIDER_PKCS11, GObject)

/**
 * gn_hsm_provider_pkcs11_new:
 *
 * Creates a new PKCS#11 HSM provider instance.
 * The provider will use p11-kit to discover available PKCS#11 modules.
 *
 * Returns: (transfer full): A new #GnHsmProviderPkcs11, or %NULL if
 *          PKCS#11 support is not available
 */
GnHsmProviderPkcs11 *gn_hsm_provider_pkcs11_new(void);

/**
 * gn_hsm_provider_pkcs11_is_supported:
 *
 * Checks if PKCS#11 support is available on this system.
 * This requires p11-kit to be installed and configured.
 *
 * Returns: %TRUE if PKCS#11 is supported
 */
gboolean gn_hsm_provider_pkcs11_is_supported(void);

/**
 * gn_hsm_provider_pkcs11_add_module:
 * @self: A #GnHsmProviderPkcs11
 * @module_path: Path to a PKCS#11 module (.so/.dylib)
 * @error: Return location for error, or %NULL
 *
 * Explicitly loads a PKCS#11 module. This is in addition to modules
 * discovered automatically via p11-kit.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_hsm_provider_pkcs11_add_module(GnHsmProviderPkcs11 *self,
                                           const gchar *module_path,
                                           GError **error);

/**
 * gn_hsm_provider_pkcs11_remove_module:
 * @self: A #GnHsmProviderPkcs11
 * @module_path: Path to remove
 *
 * Removes a manually added PKCS#11 module.
 */
void gn_hsm_provider_pkcs11_remove_module(GnHsmProviderPkcs11 *self,
                                          const gchar *module_path);

/**
 * gn_hsm_provider_pkcs11_get_module_info:
 * @self: A #GnHsmProviderPkcs11
 * @module_path: Path to query
 * @out_description: (out) (optional): Module description
 * @out_manufacturer: (out) (optional): Module manufacturer
 * @out_version: (out) (optional): Module version string
 *
 * Gets information about a loaded PKCS#11 module.
 *
 * Returns: %TRUE if module is loaded, %FALSE otherwise
 */
gboolean gn_hsm_provider_pkcs11_get_module_info(GnHsmProviderPkcs11 *self,
                                                const gchar *module_path,
                                                gchar **out_description,
                                                gchar **out_manufacturer,
                                                gchar **out_version);

/**
 * gn_hsm_provider_pkcs11_set_pin_callback:
 * @self: A #GnHsmProviderPkcs11
 * @callback: Callback function to request PIN from user
 * @user_data: User data for callback
 * @destroy: Destroy notify for user_data
 *
 * Sets a callback that will be invoked when a PIN is needed.
 * This allows integration with the UI for PIN prompts.
 *
 * Callback signature:
 *   gchar *callback(guint64 slot_id, const gchar *token_label,
 *                   gboolean is_retry, gpointer user_data)
 *
 * The callback should return a newly allocated PIN string, or %NULL to cancel.
 */
typedef gchar *(*GnHsmPinCallback)(guint64 slot_id,
                                   const gchar *token_label,
                                   gboolean is_retry,
                                   gpointer user_data);

void gn_hsm_provider_pkcs11_set_pin_callback(GnHsmProviderPkcs11 *self,
                                             GnHsmPinCallback callback,
                                             gpointer user_data,
                                             GDestroyNotify destroy);

/**
 * gn_hsm_provider_pkcs11_has_secp256k1_support:
 * @self: A #GnHsmProviderPkcs11
 * @slot_id: Slot to check
 *
 * Checks if a token has native secp256k1 ECDSA support.
 * Most tokens don't support this curve natively.
 *
 * Returns: %TRUE if native secp256k1 is supported
 */
gboolean gn_hsm_provider_pkcs11_has_secp256k1_support(GnHsmProviderPkcs11 *self,
                                                       guint64 slot_id);

/**
 * gn_hsm_provider_pkcs11_enable_software_signing:
 * @self: A #GnHsmProviderPkcs11
 * @enable: Whether to enable software signing fallback
 *
 * Enables or disables software signing fallback for tokens that don't
 * support secp256k1. When enabled, keys are stored securely on the token
 * but signing is performed in software using libsecp256k1.
 *
 * Default: enabled
 */
void gn_hsm_provider_pkcs11_enable_software_signing(GnHsmProviderPkcs11 *self,
                                                    gboolean enable);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_HSM_PROVIDER_PKCS11_H */
