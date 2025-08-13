#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_SHEET_ORBOT_SETUP (sheet_orbot_setup_get_type())
G_DECLARE_FINAL_TYPE(SheetOrbotSetup, sheet_orbot_setup, SHEET, ORBOT_SETUP, AdwDialog)
SheetOrbotSetup *sheet_orbot_setup_new(void);
G_END_DECLS
