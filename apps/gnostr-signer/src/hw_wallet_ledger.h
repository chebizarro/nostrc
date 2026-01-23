/* hw_wallet_ledger.h - Ledger hardware wallet provider
 *
 * Implementation of GnHwWalletProvider for Ledger Nano S/X/S+ devices.
 * Uses APDU commands over USB HID to communicate with the Nostr app.
 *
 * Supported devices:
 *   - Ledger Nano S (firmware 2.0+)
 *   - Ledger Nano X (all firmware)
 *   - Ledger Nano S Plus (all firmware)
 *
 * The Nostr app on Ledger implements:
 *   - GET_PUBLIC_KEY: Derive and return secp256k1 public key
 *   - SIGN_HASH: Sign a 32-byte Schnorr hash
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "hw_wallet_provider.h"

G_BEGIN_DECLS

#define GN_TYPE_HW_WALLET_LEDGER_PROVIDER (gn_hw_wallet_ledger_provider_get_type())

G_DECLARE_FINAL_TYPE(GnHwWalletLedgerProvider, gn_hw_wallet_ledger_provider,
                     GN, HW_WALLET_LEDGER_PROVIDER, GObject)

/**
 * gn_hw_wallet_ledger_provider_new:
 *
 * Creates a new Ledger hardware wallet provider.
 *
 * Returns: (transfer full): A new #GnHwWalletLedgerProvider
 */
GnHwWalletProvider *gn_hw_wallet_ledger_provider_new(void);

/* ============================================================================
 * Ledger APDU Constants
 * ============================================================================ */

/* Ledger Nostr App CLA (Class byte) */
#define LEDGER_NOSTR_CLA 0xE0

/* Ledger Nostr App INS (Instruction codes) */
#define LEDGER_NOSTR_INS_GET_VERSION      0x00
#define LEDGER_NOSTR_INS_GET_PUBLIC_KEY   0x02
#define LEDGER_NOSTR_INS_SIGN_HASH        0x04
#define LEDGER_NOSTR_INS_GET_APP_NAME     0x06

/* Ledger status words */
#define LEDGER_SW_OK                      0x9000
#define LEDGER_SW_USER_REJECTED           0x6985
#define LEDGER_SW_WRONG_LENGTH            0x6700
#define LEDGER_SW_INVALID_DATA            0x6A80
#define LEDGER_SW_INS_NOT_SUPPORTED       0x6D00
#define LEDGER_SW_CLA_NOT_SUPPORTED       0x6E00
#define LEDGER_SW_APP_NOT_OPEN            0x6E01
#define LEDGER_SW_LOCKED                  0x5515

/* P1/P2 parameters */
#define LEDGER_P1_CONFIRM_OFF             0x00
#define LEDGER_P1_CONFIRM_ON              0x01
#define LEDGER_P2_UNUSED                  0x00

/* HID packet size */
#define LEDGER_HID_PACKET_SIZE            64
#define LEDGER_HID_HEADER_SIZE            5

G_END_DECLS
