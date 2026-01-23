/* sheet-social-recovery.h - Social Recovery setup and management dialog
 *
 * Provides UI for:
 * - Setting up social recovery with guardian selection
 * - Configuring threshold (k-of-n)
 * - Distributing shares to guardians
 * - Recovering key from collected shares
 * - Managing existing recovery configuration
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_SOCIAL_RECOVERY (sheet_social_recovery_get_type())
G_DECLARE_FINAL_TYPE(SheetSocialRecovery, sheet_social_recovery, SHEET, SOCIAL_RECOVERY, AdwDialog)

/* Tab/mode enum for the dialog */
typedef enum {
  SHEET_SOCIAL_RECOVERY_MODE_SETUP,    /* Set up new recovery */
  SHEET_SOCIAL_RECOVERY_MODE_MANAGE,   /* Manage existing recovery */
  SHEET_SOCIAL_RECOVERY_MODE_RECOVER   /* Recover key from shares */
} SheetSocialRecoveryMode;

/**
 * sheet_social_recovery_new:
 *
 * Creates a new Social Recovery dialog.
 *
 * Returns: (transfer full): a new #SheetSocialRecovery
 */
SheetSocialRecovery *sheet_social_recovery_new(void);

/**
 * sheet_social_recovery_set_account:
 * @self: the dialog instance
 * @npub: the npub of the account to set up recovery for
 *
 * Sets the account (npub) for recovery setup.
 * Call this before presenting the dialog for setup mode.
 */
void sheet_social_recovery_set_account(SheetSocialRecovery *self, const gchar *npub);

/**
 * sheet_social_recovery_set_mode:
 * @self: the dialog instance
 * @mode: the mode to display
 *
 * Sets the dialog mode (setup, manage, or recover).
 */
void sheet_social_recovery_set_mode(SheetSocialRecovery *self, SheetSocialRecoveryMode mode);

/**
 * SheetSocialRecoveryCallback:
 * @npub: the public key of the recovered/setup identity
 * @user_data: user data passed to the callback
 *
 * Callback for successful recovery or setup completion.
 */
typedef void (*SheetSocialRecoveryCallback)(const gchar *npub, gpointer user_data);

/**
 * sheet_social_recovery_set_on_complete:
 * @self: the dialog instance
 * @callback: function to call on successful recovery
 * @user_data: data to pass to callback
 *
 * Sets a callback to be invoked when recovery/setup completes successfully.
 */
void sheet_social_recovery_set_on_complete(SheetSocialRecovery *self,
                                           SheetSocialRecoveryCallback callback,
                                           gpointer user_data);

G_END_DECLS
