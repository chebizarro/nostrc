#ifndef GH_GROUP_INFO_DIALOG_H
#define GH_GROUP_INFO_DIALOG_H

#include <adwaita.h>
#include "gh-nip29-service.h"

G_BEGIN_DECLS

/*
 * GhGroupInfoDialog (data/ui/gh-group-info-dialog.blp; privacy charter
 * §7.10, §2.2, §7.15 #18; item G20b): Group Info for one NIP-29 relay group
 * of the account's GhNip29Service.
 *
 *  - The group: the relay-signed name and about (39000), the relay's host,
 *    and the shareable address (gh_group_format_address()).
 *  - Privacy: gh_privacy_summary_new() for a relay group: not end-to-end
 *    encrypted, the relay's operators read every message.
 *  - Group: the account's membership in the honest words of
 *    gh_group_join_copy() (pending review, declined, already a member,
 *    invite only, removed, ...), how reading goes (GhNip29ReadState) and
 *    whether the group state is verified by the relay's key.
 *  - Members: admins first with their roles as badges (39001), then members
 *    (39002); the list always says it may be incomplete and is never a
 *    count as if complete. Names are the ones Groundhog has cached (the
 *    name function), else short npubs; nothing is fetched.
 *  - Admin actions, each only when gh_group_room_gate() allows it (hidden
 *    when the relay's lists say no, or without a relay-signed admin list;
 *    offered with "The relay decides whether you're allowed" when the role's
 *    rights are unknown): Change Role (9000), Remove from Group (9001,
 *    confirmed), Add Member (9000), Name and Settings (9002, keeping every
 *    tag the relay set), Invite Code (9009, a random code from the OS
 *    CSPRNG). Every operation goes through the durable NIP-29 outbox, and
 *    its outcome is the relay's: a toast says it was accepted or quotes the
 *    refusal. Nothing changes locally: the relay's next snapshot is the
 *    truth.
 *  - Leave (9022, confirmed), Ask to Join Again, and for a group the account
 *    is not in, Remove from Conversations (gh_nip29_service_forget()).
 * Relay text is never markup. The dialog follows the room live (its
 * properties and "group-changed") and closes when the room leaves the
 * service (forgotten, account switch).
 *
 * Actions (widget actions of the dialog): group.copy-address,
 * group.copy-invite, group.add-member, group.save-member,
 * group.change-role (s: pubkey hex), group.save-role, group.remove (s:
 * pubkey hex; presents its confirmation), group.edit, group.save-edit,
 * group.create-invite, group.join, group.leave and group.forget (each of
 * the last two presents its confirmation).
 */

/* The name Groundhog has cached for pubkey (e.g. the contact directory's),
 * or NULL. Display only, never fetched. */
typedef const gchar *(*GhGroupNameFunc)(const gchar *pubkey, gpointer user_data);

typedef struct {
  GhNip29Service *service;      /* required: the room's service */
  GhGroupNameFunc display_name; /* nullable */
  gpointer names_data;
} GhGroupInfoConfig;

#define GH_TYPE_GROUP_INFO_DIALOG (gh_group_info_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhGroupInfoDialog, gh_group_info_dialog, GH, GROUP_INFO_DIALOG, AdwDialog)

GhGroupInfoDialog *gh_group_info_dialog_new(GhNip29Room *room, const GhGroupInfoConfig *config);
GhNip29Room *gh_group_info_dialog_get_room(GhGroupInfoDialog *self);

/* The operations this dialog sent that the relay has not answered yet
 * (tests). */
guint gh_group_info_dialog_get_pending_ops(GhGroupInfoDialog *self);

G_END_DECLS
#endif
