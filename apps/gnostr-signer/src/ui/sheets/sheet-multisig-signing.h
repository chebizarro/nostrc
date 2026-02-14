/* sheet-multisig-signing.h - Multi-signature signing progress dialog
 *
 * Displays signing progress during a multi-signature operation:
 * - Shows which signers have signed
 * - Displays progress (e.g., "2 of 3 signatures collected")
 * - Allows cancellation
 * - Shows success when threshold is met
 *
 * Issue: nostrc-orz
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_MULTISIG_SIGNING_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_MULTISIG_SIGNING_H

#include <adwaita.h>
#include "../../multisig_wallet.h"

G_BEGIN_DECLS

#define SHEET_TYPE_MULTISIG_SIGNING (sheet_multisig_signing_get_type())
G_DECLARE_FINAL_TYPE(SheetMultisigSigning, sheet_multisig_signing, SHEET, MULTISIG_SIGNING, AdwDialog)

/**
 * SheetMultisigSigningCallback:
 * @success: Whether signing completed successfully
 * @signature: The final aggregated signature (or NULL on failure)
 * @user_data: User data
 *
 * Callback invoked when signing completes or is canceled.
 */
typedef void (*SheetMultisigSigningCallback)(gboolean success,
                                             const gchar *signature,
                                             gpointer user_data);

/**
 * sheet_multisig_signing_new:
 * @wallet_id: ID of the multisig wallet
 * @event_json: Event JSON to sign
 *
 * Create a new multisig signing progress dialog.
 * Returns: New SheetMultisigSigning instance
 */
SheetMultisigSigning *sheet_multisig_signing_new(const gchar *wallet_id,
                                                  const gchar *event_json);

/**
 * sheet_multisig_signing_set_callback:
 * @self: The dialog
 * @callback: Callback for completion/cancellation
 * @user_data: User data for callback
 *
 * Set the callback invoked when signing completes or is canceled.
 */
void sheet_multisig_signing_set_callback(SheetMultisigSigning *self,
                                         SheetMultisigSigningCallback callback,
                                         gpointer user_data);

/**
 * sheet_multisig_signing_start:
 * @self: The dialog
 *
 * Start the signing process. The dialog will automatically request
 * signatures from all co-signers.
 */
void sheet_multisig_signing_start(SheetMultisigSigning *self);

/**
 * sheet_multisig_signing_update_progress:
 * @self: The dialog
 * @signer_npub: The signer who just signed
 * @status: The signer's new status
 *
 * Update the UI with new progress. Called by the coordinator.
 */
void sheet_multisig_signing_update_progress(SheetMultisigSigning *self,
                                            const gchar *signer_npub,
                                            CosignerStatus status);

/**
 * sheet_multisig_signing_complete:
 * @self: The dialog
 * @success: Whether signing succeeded
 * @signature: The final signature (if success)
 * @error_message: Error message (if failure)
 *
 * Mark signing as complete. Called by the coordinator.
 */
void sheet_multisig_signing_complete(SheetMultisigSigning *self,
                                     gboolean success,
                                     const gchar *signature,
                                     const gchar *error_message);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_MULTISIG_SIGNING_H */
