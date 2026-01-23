#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SHEET_TYPE_SIGN_MESSAGE (sheet_sign_message_get_type())
G_DECLARE_FINAL_TYPE(SheetSignMessage, sheet_sign_message, SHEET, SIGN_MESSAGE, AdwDialog)

SheetSignMessage *sheet_sign_message_new(GtkWindow *parent);
void sheet_sign_message_set_profile(SheetSignMessage *self, const char *profile_name, const char *account_id);

G_END_DECLS
