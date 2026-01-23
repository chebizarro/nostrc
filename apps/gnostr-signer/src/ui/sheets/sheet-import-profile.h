/* sheet-import-profile.h - Import Profile dialog
 *
 * Provides a UI for importing an existing Nostr profile with multiple methods:
 * - NIP-49 Encrypted Backup (ncryptsec)
 * - Mnemonic Seed Phrase (12/24 words)
 * - External Hardware Device (placeholder)
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_IMPORT_PROFILE (sheet_import_profile_get_type())
G_DECLARE_FINAL_TYPE(SheetImportProfile, sheet_import_profile, SHEET, IMPORT_PROFILE, AdwDialog)

/* Import methods supported */
typedef enum {
  IMPORT_METHOD_NIP49 = 0,      /* NIP-49 Encrypted Backup (ncryptsec) */
  IMPORT_METHOD_MNEMONIC = 1,   /* BIP-39 Mnemonic Seed Phrase */
  IMPORT_METHOD_HARDWARE = 2    /* External Hardware Device */
} ImportMethod;

/* Callback invoked when profile is successfully imported.
 * npub: the derived public key string
 * method: the import method used
 */
typedef void (*SheetImportProfileSuccessCb)(const gchar *npub,
                                            ImportMethod method,
                                            gpointer user_data);

/* Create a new Import Profile dialog */
SheetImportProfile *sheet_import_profile_new(void);

/* Set a callback to be invoked on successful profile import */
void sheet_import_profile_set_on_success(SheetImportProfile *self,
                                         SheetImportProfileSuccessCb cb,
                                         gpointer user_data);

G_END_DECLS
