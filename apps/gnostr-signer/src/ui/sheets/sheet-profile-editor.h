/* sheet-profile-editor.h - Profile editing dialog */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_PROFILE_EDITOR (sheet_profile_editor_get_type())
G_DECLARE_FINAL_TYPE(SheetProfileEditor, sheet_profile_editor, SHEET, PROFILE_EDITOR, AdwDialog)

/* Callback invoked when profile is saved */
typedef void (*SheetProfileEditorSaveCb)(const gchar *npub,
                                         const gchar *event_json,
                                         gpointer user_data);

/* Create a new profile editor dialog */
SheetProfileEditor *sheet_profile_editor_new(void);

/* Set the npub to edit */
void sheet_profile_editor_set_npub(SheetProfileEditor *self, const gchar *npub);

/* Set callback for save action */
void sheet_profile_editor_set_on_save(SheetProfileEditor *self,
                                      SheetProfileEditorSaveCb cb,
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
