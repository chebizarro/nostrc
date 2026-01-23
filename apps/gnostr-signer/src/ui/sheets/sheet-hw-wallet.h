/* sheet-hw-wallet.h - Hardware wallet connection and selection sheet
 *
 * This sheet provides UI for:
 *   - Detecting connected hardware wallets
 *   - Selecting a device for signing
 *   - Showing device status and prompts
 *   - Importing hardware wallet-backed accounts
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>
#include "../../hw_wallet_provider.h"

G_BEGIN_DECLS

#define TYPE_SHEET_HW_WALLET (sheet_hw_wallet_get_type())

G_DECLARE_FINAL_TYPE(SheetHwWallet, sheet_hw_wallet, SHEET, HW_WALLET, AdwDialog)

/**
 * SheetHwWalletMode:
 * @SHEET_HW_WALLET_MODE_SELECT: Select a device for signing
 * @SHEET_HW_WALLET_MODE_IMPORT: Import account from hardware wallet
 * @SHEET_HW_WALLET_MODE_SIGN: Sign a specific message/event
 *
 * Operating mode for the hardware wallet sheet.
 */
typedef enum {
  SHEET_HW_WALLET_MODE_SELECT,
  SHEET_HW_WALLET_MODE_IMPORT,
  SHEET_HW_WALLET_MODE_SIGN
} SheetHwWalletMode;

/**
 * sheet_hw_wallet_new:
 * @mode: Operating mode for the sheet
 *
 * Creates a new hardware wallet sheet.
 *
 * Returns: (transfer full): A new #SheetHwWallet
 */
SheetHwWallet *sheet_hw_wallet_new(SheetHwWalletMode mode);

/**
 * SheetHwWalletSuccessCb:
 * @npub: The public key in bech32 format
 * @device_id: The device identifier
 * @label: Optional label for the account
 * @user_data: User data
 *
 * Callback invoked when a hardware wallet account is successfully imported
 * or selected for signing.
 */
typedef void (*SheetHwWalletSuccessCb)(const char *npub,
                                        const char *device_id,
                                        const char *label,
                                        gpointer user_data);

/**
 * sheet_hw_wallet_set_on_success:
 * @self: A #SheetHwWallet
 * @cb: Callback function
 * @user_data: Data for callback
 *
 * Sets the callback for successful operations.
 */
void sheet_hw_wallet_set_on_success(SheetHwWallet *self,
                                     SheetHwWalletSuccessCb cb,
                                     gpointer user_data);

/**
 * SheetHwWalletSignCb:
 * @signature: The 64-byte Schnorr signature (hex string)
 * @user_data: User data
 *
 * Callback invoked when signing is complete.
 */
typedef void (*SheetHwWalletSignCb)(const char *signature,
                                     gpointer user_data);

/**
 * sheet_hw_wallet_set_on_signed:
 * @self: A #SheetHwWallet
 * @cb: Callback function
 * @user_data: Data for callback
 *
 * Sets the callback for completed signing.
 */
void sheet_hw_wallet_set_on_signed(SheetHwWallet *self,
                                    SheetHwWalletSignCb cb,
                                    gpointer user_data);

/**
 * sheet_hw_wallet_set_hash_to_sign:
 * @self: A #SheetHwWallet
 * @hash: 32-byte hash to sign
 * @hash_len: Length of hash (must be 32)
 *
 * Sets the hash to sign when in SIGN mode.
 */
void sheet_hw_wallet_set_hash_to_sign(SheetHwWallet *self,
                                       const guint8 *hash,
                                       gsize hash_len);

/**
 * sheet_hw_wallet_set_device_filter:
 * @self: A #SheetHwWallet
 * @device_id: Device ID to show, or %NULL for all
 *
 * Filters the device list to show only a specific device.
 */
void sheet_hw_wallet_set_device_filter(SheetHwWallet *self,
                                        const char *device_id);

/**
 * sheet_hw_wallet_refresh_devices:
 * @self: A #SheetHwWallet
 *
 * Manually triggers a device refresh.
 */
void sheet_hw_wallet_refresh_devices(SheetHwWallet *self);

G_END_DECLS
