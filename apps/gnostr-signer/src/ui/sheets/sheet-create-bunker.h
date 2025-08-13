#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_SHEET_CREATE_BUNKER (sheet_create_bunker_get_type())
G_DECLARE_FINAL_TYPE(SheetCreateBunker, sheet_create_bunker, SHEET, CREATE_BUNKER, AdwDialog)
SheetCreateBunker *sheet_create_bunker_new(void);
G_END_DECLS
