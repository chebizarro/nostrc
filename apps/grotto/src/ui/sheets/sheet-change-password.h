#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_CHANGE_PASSWORD_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_CHANGE_PASSWORD_H

#include <adwaita.h>

G_BEGIN_DECLS

#define SHEET_TYPE_CHANGE_PASSWORD (sheet_change_password_get_type())
G_DECLARE_FINAL_TYPE(SheetChangePassword, sheet_change_password, SHEET, CHANGE_PASSWORD, AdwDialog)

SheetChangePassword *sheet_change_password_new(GtkWindow *parent);
void sheet_change_password_set_account(SheetChangePassword *self, const char *account_id);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_CHANGE_PASSWORD_H */
