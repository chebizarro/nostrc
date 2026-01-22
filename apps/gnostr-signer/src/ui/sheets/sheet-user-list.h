/* sheet-user-list.h - User list management dialog (follows/mutes) */
#pragma once

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

G_END_DECLS
