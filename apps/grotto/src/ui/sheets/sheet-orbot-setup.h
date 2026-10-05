#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_ORBOT_SETUP_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_ORBOT_SETUP_H
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_SHEET_ORBOT_SETUP (sheet_orbot_setup_get_type())
G_DECLARE_FINAL_TYPE(SheetOrbotSetup, sheet_orbot_setup, SHEET, ORBOT_SETUP, AdwDialog)
SheetOrbotSetup *sheet_orbot_setup_new(void);
G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_ORBOT_SETUP_H */
