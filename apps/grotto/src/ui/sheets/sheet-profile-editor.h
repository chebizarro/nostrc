/* sheet-profile-editor.h - Profile editing dialog
 *
 * Provides a UI for editing Nostr profile metadata (kind:0 events).
 * Features:
 * - Edit all standard profile fields (name, about, picture, banner, nip05, lud16, website)
 * - Preview changes before publishing
 * - Sign events using the signer's key management
 * - Publish as kind:0 metadata events
 */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_PROFILE_EDITOR_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_PROFILE_EDITOR_H

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_PROFILE_EDITOR (sheet_profile_editor_get_type())
G_DECLARE_FINAL_TYPE(SheetProfileEditor, sheet_profile_editor, SHEET, PROFILE_EDITOR, AdwDialog)

/* Callback invoked when profile is saved (unsigned event JSON) */
typedef void (*SheetProfileEditorSaveCb)(const gchar *npub,
                                         const gchar *event_json,
                                         gpointer user_data);

/* Callback invoked when profile is signed and ready for publishing */
typedef void (*SheetProfileEditorPublishCb)(const gchar *npub,
                                            const gchar *signed_event_json,
                                            gpointer user_data);

/* Create a new profile editor dialog */
SheetProfileEditor *sheet_profile_editor_new(void);

/* Set the npub to edit */
void sheet_profile_editor_set_npub(SheetProfileEditor *self, const gchar *npub);

/* Set callback for save action (provides unsigned event) */
void sheet_profile_editor_set_on_save(SheetProfileEditor *self,
                                      SheetProfileEditorSaveCb cb,
                                      gpointer user_data);

/* Set callback for publish action (provides signed event) */
void sheet_profile_editor_set_on_publish(SheetProfileEditor *self,
                                         SheetProfileEditorPublishCb cb,
                                         gpointer user_data);

/* Load existing profile data */
void sheet_profile_editor_load_profile(SheetProfileEditor *self,
                                       const gchar *name,
                                       const gchar *about,
                                       const gchar *picture,
                                       const gchar *banner,
                                       const gchar *nip05,
                                       const gchar *lud16,
                                       const gchar *website);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_PROFILE_EDITOR_H */
