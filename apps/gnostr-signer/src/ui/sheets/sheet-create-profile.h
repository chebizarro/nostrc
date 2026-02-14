/* sheet-create-profile.h - Create Profile dialog
 *
 * Provides a UI for creating a new Nostr profile with passphrase protection.
 * Features:
 * - Display name input
 * - Passphrase input with visibility toggle
 * - Confirm passphrase input
 * - Recovery hint input (optional)
 * - Hardware key checkbox
 * - Passphrase strength validation
 * - Passphrase match validation
 */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_CREATE_PROFILE_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_CREATE_PROFILE_H

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_CREATE_PROFILE (sheet_create_profile_get_type())
G_DECLARE_FINAL_TYPE(SheetCreateProfile, sheet_create_profile, SHEET, CREATE_PROFILE, AdwDialog)

/* Callback invoked when profile is successfully created.
 * npub: the derived public key string
 * display_name: the user-provided display name
 * use_hardware_key: whether hardware key was selected
 */
typedef void (*SheetCreateProfileSuccessCb)(const gchar *npub,
                                             const gchar *display_name,
                                             gboolean use_hardware_key,
                                             gpointer user_data);

/* Create a new Create Profile dialog */
SheetCreateProfile *sheet_create_profile_new(void);

/* Set a callback to be invoked on successful profile creation */
void sheet_create_profile_set_on_success(SheetCreateProfile *self,
                                          SheetCreateProfileSuccessCb cb,
                                          gpointer user_data);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_CREATE_PROFILE_H */
