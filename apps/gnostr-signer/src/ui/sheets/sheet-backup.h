/* sheet-backup.h - Backup and Recovery dialog for gnostr-signer
 *
 * Provides comprehensive UI for:
 * - Exporting identity as NIP-49 encrypted backup (ncryptsec)
 * - Saving backup to file with file chooser dialog
 * - Displaying QR code for backup string
 * - Importing from ncryptsec string
 * - Importing from BIP-39 mnemonic phrase
 * - Verification before importing
 */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_BACKUP_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_BACKUP_H

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_BACKUP (sheet_backup_get_type())
G_DECLARE_FINAL_TYPE(SheetBackup, sheet_backup, SHEET, BACKUP, AdwDialog)

/**
 * sheet_backup_new:
 *
 * Creates a new Backup & Recovery dialog.
 *
 * Returns: (transfer full): a new #SheetBackup
 */
SheetBackup *sheet_backup_new(void);

/**
 * sheet_backup_set_account:
 * @self: the dialog instance
 * @npub: the npub of the account to backup
 *
 * Sets the account (npub) to backup.
 * This should be called before presenting the dialog.
 */
void sheet_backup_set_account(SheetBackup *self, const gchar *npub);

/**
 * sheet_backup_show_backup_tab:
 * @self: the dialog instance
 *
 * Switch to the backup tab.
 */
void sheet_backup_show_backup_tab(SheetBackup *self);

/**
 * sheet_backup_show_recovery_tab:
 * @self: the dialog instance
 *
 * Switch to the recovery tab.
 */
void sheet_backup_show_recovery_tab(SheetBackup *self);

/**
 * SheetBackupImportCallback:
 * @npub: the public key of the imported identity
 * @user_data: user data passed to the callback
 *
 * Callback type for successful import operations.
 */
typedef void (*SheetBackupImportCallback)(const gchar *npub, gpointer user_data);

/**
 * sheet_backup_set_on_import:
 * @self: the dialog instance
 * @callback: function to call on successful import
 * @user_data: data to pass to callback
 *
 * Sets a callback to be invoked when a key is successfully imported.
 */
void sheet_backup_set_on_import(SheetBackup *self,
                                 SheetBackupImportCallback callback,
                                 gpointer user_data);

/**
 * sheet_backup_trigger_reminder:
 * @parent: the parent window
 * @npub: the npub of the newly created identity
 *
 * Shows a backup reminder dialog for a newly created key.
 * This should be called after first key creation.
 */
void sheet_backup_trigger_reminder(GtkWindow *parent, const gchar *npub);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_BACKUP_H */
