/* sheet-user-list.h - User list management dialog (follows/mutes) */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_USER_LIST_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_USER_LIST_H

#include <adwaita.h>
#include "../../user_list_store.h"

G_BEGIN_DECLS

#define TYPE_SHEET_USER_LIST (sheet_user_list_get_type())
G_DECLARE_FINAL_TYPE(SheetUserList, sheet_user_list, SHEET, USER_LIST, AdwDialog)

/* Callback invoked when list is saved/published */
typedef void (*SheetUserListSaveCb)(UserListType type,
                                    const gchar *event_json,
                                    gpointer user_data);

/* Create a new user list dialog */
SheetUserList *sheet_user_list_new(UserListType type);

/* Set callback for publish action */
void sheet_user_list_set_on_publish(SheetUserList *self,
                                    SheetUserListSaveCb cb,
                                    gpointer user_data);

/* Get the underlying user list store */
UserListStore *sheet_user_list_get_store(SheetUserList *self);

/* Refresh the list from the store */
void sheet_user_list_refresh(SheetUserList *self);

/* Set profile info for a user (updates display) */
void sheet_user_list_update_user_profile(SheetUserList *self,
                                         const gchar *pubkey,
                                         const gchar *display_name,
                                         const gchar *avatar_url,
                                         const gchar *nip05);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_USER_LIST_H */
