/* hw_wallet_trezor.h - Trezor hardware wallet provider
 *
 * Implementation of GnHwWalletProvider for Trezor Model One/T/Safe 3 devices.
 * Uses the Trezor wire protocol over USB HID.
 *
 * Supported devices:
 *   - Trezor Model One (firmware 1.12+)
 *   - Trezor Model T (all firmware)
 *   - Trezor Safe 3 (all firmware)
 *
 * The Trezor firmware implements:
 *   - GetPublicKey: Derive and return secp256k1 public key
 *   - SignMessage: Sign arbitrary message with Schnorr
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_HW_WALLET_TREZOR_H
#define APPS_GNOSTR_SIGNER_HW_WALLET_TREZOR_H

#include "hw_wallet_provider.h"

G_BEGIN_DECLS

#define GN_TYPE_HW_WALLET_TREZOR_PROVIDER (gn_hw_wallet_trezor_provider_get_type())

G_DECLARE_FINAL_TYPE(GnHwWalletTrezorProvider, gn_hw_wallet_trezor_provider,
                     GN, HW_WALLET_TREZOR_PROVIDER, GObject)

/**
 * gn_hw_wallet_trezor_provider_new:
 *
 * Creates a new Trezor hardware wallet provider.
 *
 * Returns: (transfer full): A new #GnHwWalletTrezorProvider
 */
GnHwWalletProvider *gn_hw_wallet_trezor_provider_new(void);

/* ============================================================================
 * Trezor Protocol Constants
 * ============================================================================ */

/* Trezor message types (subset for Nostr operations) */
#define TREZOR_MSG_INITIALIZE           0
#define TREZOR_MSG_PING                 1
#define TREZOR_MSG_SUCCESS              2
#define TREZOR_MSG_FAILURE              3
#define TREZOR_MSG_FEATURES             17
#define TREZOR_MSG_BUTTON_REQUEST       26
#define TREZOR_MSG_BUTTON_ACK           27
#define TREZOR_MSG_PIN_MATRIX_REQUEST   18
#define TREZOR_MSG_PIN_MATRIX_ACK       19
#define TREZOR_MSG_PASSPHRASE_REQUEST   41
#define TREZOR_MSG_PASSPHRASE_ACK       42
#define TREZOR_MSG_GET_PUBLIC_KEY       11
#define TREZOR_MSG_PUBLIC_KEY           12
#define TREZOR_MSG_SIGN_MESSAGE         38
#define TREZOR_MSG_MESSAGE_SIGNATURE    40

/* Trezor curve names */
#define TREZOR_CURVE_SECP256K1 "secp256k1"
#define TREZOR_CURVE_ED25519   "ed25519"

/* HID packet size */
#define TREZOR_HID_PACKET_SIZE 64
#define TREZOR_HID_HEADER_SIZE 3

/* Magic bytes for packet framing */
#define TREZOR_MAGIC_V1 '?'  /* 0x3F */
#define TREZOR_MAGIC_V2 '#'  /* 0x23 */

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_HW_WALLET_TREZOR_H */
