#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_SHEET_SELECT_ACCOUNT (sheet_select_account_get_type())
G_DECLARE_FINAL_TYPE(SheetSelectAccount, sheet_select_account, SHEET, SELECT_ACCOUNT, AdwDialog)
SheetSelectAccount *sheet_select_account_new(void);
G_END_DECLS
