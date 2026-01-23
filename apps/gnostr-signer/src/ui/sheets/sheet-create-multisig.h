/* sheet-create-multisig.h - Multi-signature wallet creation dialog
 *
 * Multi-step wizard for creating a new multisig wallet:
 * - Step 1: Configure threshold (m-of-n)
 * - Step 2: Add local co-signers (from existing accounts)
 * - Step 3: Add remote co-signers (via NIP-46 bunker URI)
 * - Step 4: Review and confirm configuration
 * - Step 5: Success
 *
 * Issue: nostrc-orz
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SHEET_TYPE_CREATE_MULTISIG (sheet_create_multisig_get_type())
G_DECLARE_FINAL_TYPE(SheetCreateMultisig, sheet_create_multisig, SHEET, CREATE_MULTISIG, AdwDialog)

/**
 * SheetCreateMultisigCallback:
 * @wallet_id: The created wallet's ID
 * @user_data: User data
 *
 * Callback invoked when a multisig wallet is successfully created.
 */
typedef void (*SheetCreateMultisigCallback)(const gchar *wallet_id, gpointer user_data);

/**
 * sheet_create_multisig_new:
 *
 * Create a new multisig wallet creation dialog.
 * Returns: New SheetCreateMultisig instance
 */
SheetCreateMultisig *sheet_create_multisig_new(void);

/**
 * sheet_create_multisig_set_on_created:
 * @self: The dialog
 * @callback: Callback for successful creation
 * @user_data: User data for callback
 *
 * Set the callback invoked when wallet creation succeeds.
 */
void sheet_create_multisig_set_on_created(SheetCreateMultisig *self,
                                          SheetCreateMultisigCallback callback,
                                          gpointer user_data);

/**
 * sheet_create_multisig_set_default_threshold:
 * @self: The dialog
 * @m: Required signatures
 * @n: Total signers
 *
 * Pre-set the threshold configuration.
 */
void sheet_create_multisig_set_default_threshold(SheetCreateMultisig *self,
                                                 guint m,
                                                 guint n);

G_END_DECLS
