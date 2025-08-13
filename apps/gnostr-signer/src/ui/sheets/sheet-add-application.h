#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_SHEET_ADD_APPLICATION (sheet_add_application_get_type())
G_DECLARE_FINAL_TYPE(SheetAddApplication, sheet_add_application, SHEET, ADD_APPLICATION, AdwDialog)
SheetAddApplication *sheet_add_application_new(void);
G_END_DECLS
