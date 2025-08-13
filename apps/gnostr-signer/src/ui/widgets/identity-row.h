#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_IDENTITY_ROW (identity_row_get_type())
G_DECLARE_FINAL_TYPE(IdentityRow, identity_row, IDENTITY, ROW, AdwActionRow)
GtkWidget *identity_row_new(void);
G_END_DECLS
