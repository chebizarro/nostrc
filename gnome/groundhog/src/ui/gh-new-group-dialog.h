#ifndef GH_NEW_GROUP_DIALOG_H
#define GH_NEW_GROUP_DIALOG_H

#include <adwaita.h>
#include "gh-nip29-service.h"

G_BEGIN_DECLS

/*
 * GhNewGroupDialog (data/ui/gh-new-group-dialog.blp; privacy charter §7.9;
 * item G20b): creates a NIP-29 relay group on a relay the user names (the
 * relay part of New Group; encrypted groups are not offered before they
 * ship). Create asks gh_nip29_service_create_group() (9007 through the
 * durable outbox, then the chosen name, about and flags as 9002 once the
 * relay created it) and shows the relay's answer (gh_group_create_copy()):
 * a relay that does not let people create groups says so, quoted. The
 * group is "created, waiting for its details" until the relay's own 39000
 * arrives. Nothing contacts the network before Create. Open Group emits
 * "open-group" (GhNip29Room) and closes.
 *
 * Actions (widget actions): new-group.create, new-group.open, new-group.back.
 */
#define GH_TYPE_NEW_GROUP_DIALOG (gh_new_group_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhNewGroupDialog, gh_new_group_dialog, GH, NEW_GROUP_DIALOG, AdwDialog)

GhNewGroupDialog *gh_new_group_dialog_new(GhNip29Service *service);
/* The group asked for, once Create was chosen; NULL before. */
GhNip29Room *gh_new_group_dialog_get_room(GhNewGroupDialog *self);
const gchar *gh_new_group_dialog_get_status_title(GhNewGroupDialog *self);

G_END_DECLS
#endif
