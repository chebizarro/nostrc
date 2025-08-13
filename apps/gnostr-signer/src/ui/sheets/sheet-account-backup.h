#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_SHEET_ACCOUNT_BACKUP (sheet_account_backup_get_type())
G_DECLARE_FINAL_TYPE(SheetAccountBackup, sheet_account_backup, SHEET, ACCOUNT_BACKUP, AdwDialog)
SheetAccountBackup *sheet_account_backup_new(void);
G_END_DECLS
