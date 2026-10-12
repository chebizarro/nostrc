#ifndef GH_GROUP_RESTORE_DIALOG_H
#define GH_GROUP_RESTORE_DIALOG_H

#include <adwaita.h>

#include "gh-nip29-group-list.h"

G_BEGIN_DECLS

/*
 * "Restore Groups?" (nostrc-90jof): the groups of the account's group list
 * (kind 10009) that this device does not have, each with a check (all
 * checked at first). "Restore" (disabled while none is checked) means: join
 * the checked ones through the existing join flow; "Not Now" is the default
 * and the close response. Nothing is contacted by the dialog itself.
 */

#define GH_TYPE_GROUP_RESTORE_DIALOG (gh_group_restore_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhGroupRestoreDialog, gh_group_restore_dialog, GH, GROUP_RESTORE_DIALOG,
                     AdwAlertDialog)

#define GH_GROUP_RESTORE_RESPONSE_RESTORE "restore"
#define GH_GROUP_RESTORE_RESPONSE_NOT_NOW "not-now"

/* groups: GhNip29ListedGroup (copied). */
GhGroupRestoreDialog *gh_group_restore_dialog_new(GPtrArray *groups);
/* The checked groups (GhNip29ListedGroup), in list order. Transfer full. */
GPtrArray *gh_group_restore_dialog_dup_selected(GhGroupRestoreDialog *self);
/* The check of the index-th group (for tests and keyboard navigation). */
GtkCheckButton *gh_group_restore_dialog_get_check(GhGroupRestoreDialog *self, guint index);

G_END_DECLS
#endif
