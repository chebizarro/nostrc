/* sheet-select-account.h - Account selection and management dialog */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_SELECT_ACCOUNT_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_SELECT_ACCOUNT_H

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_SELECT_ACCOUNT (sheet_select_account_get_type())
G_DECLARE_FINAL_TYPE(SheetSelectAccount, sheet_select_account, SHEET, SELECT_ACCOUNT, AdwDialog)

/* Callback when account is selected */
typedef void (*SheetSelectAccountCb)(const gchar *npub, gpointer user_data);

/* Create a new account selection dialog */
SheetSelectAccount *sheet_select_account_new(void);

/* Set callback for account selection */
void sheet_select_account_set_on_select(SheetSelectAccount *self,
                                         SheetSelectAccountCb cb,
                                         gpointer user_data);

/* Refresh the account list */
void sheet_select_account_refresh(SheetSelectAccount *self);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_SELECT_ACCOUNT_H */
