#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_ADD_APPLICATION_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_ADD_APPLICATION_H
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_SHEET_ADD_APPLICATION (sheet_add_application_get_type())
G_DECLARE_FINAL_TYPE(SheetAddApplication, sheet_add_application, SHEET, ADD_APPLICATION, AdwDialog)
SheetAddApplication *sheet_add_application_new(void);
G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_ADD_APPLICATION_H */
