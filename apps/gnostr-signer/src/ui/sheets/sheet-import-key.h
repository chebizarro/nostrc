#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_IMPORT_KEY_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_IMPORT_KEY_H
#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_IMPORT_KEY (sheet_import_key_get_type())
G_DECLARE_FINAL_TYPE(SheetImportKey, sheet_import_key, SHEET, IMPORT_KEY, AdwDialog)

SheetImportKey *sheet_import_key_new(void);

/* Notify parent when import succeeds. npub is the derived public key string; label is optional. */
typedef void (*SheetImportKeySuccessCb)(const char *npub, const char *label, gpointer user_data);

/* Set a callback to be invoked on successful import. Optional user_data is passed through. */
void sheet_import_key_set_on_success(SheetImportKey *self,
                                     SheetImportKeySuccessCb cb,
                                     gpointer user_data);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_IMPORT_KEY_H */
