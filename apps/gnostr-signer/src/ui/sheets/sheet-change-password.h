#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SHEET_TYPE_CHANGE_PASSWORD (sheet_change_password_get_type())
G_DECLARE_FINAL_TYPE(SheetChangePassword, sheet_change_password, SHEET, CHANGE_PASSWORD, AdwDialog)

SheetChangePassword *sheet_change_password_new(GtkWindow *parent);
void sheet_change_password_set_account(SheetChangePassword *self, const char *account_id);

G_END_DECLS
