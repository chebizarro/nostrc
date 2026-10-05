#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_CREATE_BUNKER_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_CREATE_BUNKER_H
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_SHEET_CREATE_BUNKER (sheet_create_bunker_get_type())
G_DECLARE_FINAL_TYPE(SheetCreateBunker, sheet_create_bunker, SHEET, CREATE_BUNKER, AdwDialog)
SheetCreateBunker *sheet_create_bunker_new(void);
G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_CREATE_BUNKER_H */
