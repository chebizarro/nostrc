/* sheet-account-backup.h - Account backup dialog
 *
 * Provides UI for backing up Nostr identity keys:
 * - Show/copy raw nsec (with warnings)
 * - Create NIP-49 encrypted backup (ncryptsec)
 * - Show mnemonic seed words (if applicable)
 * - QR code display for scanning
 */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_ACCOUNT_BACKUP_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_ACCOUNT_BACKUP_H

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_ACCOUNT_BACKUP (sheet_account_backup_get_type())
G_DECLARE_FINAL_TYPE(SheetAccountBackup, sheet_account_backup, SHEET, ACCOUNT_BACKUP, AdwDialog)

/* Create a new Account Backup dialog */
SheetAccountBackup *sheet_account_backup_new(void);

/* Set the account (npub) to backup.
 * This should be called before presenting the dialog.
 *
 * @self: the dialog instance
 * @npub: the npub of the account to backup
 */
void sheet_account_backup_set_account(SheetAccountBackup *self, const gchar *npub);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_ACCOUNT_BACKUP_H */
