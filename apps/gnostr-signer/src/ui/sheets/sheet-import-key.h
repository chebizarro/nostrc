#pragma once
#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_IMPORT_KEY (sheet_import_key_get_type())
G_DECLARE_FINAL_TYPE(SheetImportKey, sheet_import_key, SHEET, IMPORT_KEY, AdwDialog)

SheetImportKey *sheet_import_key_new(void);

G_END_DECLS
