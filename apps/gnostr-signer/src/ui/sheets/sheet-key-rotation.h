/* sheet-key-rotation.h - Key rotation dialog for gnostr-signer
 *
 * Provides UI for:
 * - Initiating key rotation
 * - Showing progress during rotation
 * - Displaying migration event for manual publishing
 * - Confirming completion
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_KEY_ROTATION (sheet_key_rotation_get_type())
G_DECLARE_FINAL_TYPE(SheetKeyRotation, sheet_key_rotation, SHEET, KEY_ROTATION, AdwDialog)

/**
 * sheet_key_rotation_new:
 *
 * Creates a new Key Rotation dialog.
 *
 * Returns: (transfer full): a new #SheetKeyRotation
 */
SheetKeyRotation *sheet_key_rotation_new(void);

/**
 * sheet_key_rotation_set_account:
 * @self: the dialog instance
 * @npub: the npub of the account to rotate
 *
 * Sets the account (npub) to rotate.
 * This should be called before presenting the dialog.
 */
void sheet_key_rotation_set_account(SheetKeyRotation *self, const gchar *npub);

/**
 * SheetKeyRotationCompleteCb:
 * @old_npub: the old public key that was rotated from
 * @new_npub: the new public key that was rotated to
 * @user_data: user data passed to the callback
 *
 * Callback type for successful rotation operations.
 */
typedef void (*SheetKeyRotationCompleteCb)(const gchar *old_npub,
                                            const gchar *new_npub,
                                            gpointer user_data);

/**
 * sheet_key_rotation_set_on_complete:
 * @self: the dialog instance
 * @callback: function to call on successful rotation
 * @user_data: data to pass to callback
 *
 * Sets a callback to be invoked when key rotation completes successfully.
 */
void sheet_key_rotation_set_on_complete(SheetKeyRotation *self,
                                         SheetKeyRotationCompleteCb callback,
                                         gpointer user_data);

G_END_DECLS
