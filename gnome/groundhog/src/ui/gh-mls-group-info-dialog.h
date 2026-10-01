#ifndef GH_MLS_GROUP_INFO_DIALOG_H
#define GH_MLS_GROUP_INFO_DIALOG_H

#include <adwaita.h>

#include "gh-mls-context.h"

G_BEGIN_DECLS

/*
 * GhMlsGroupInfoDialog (data/ui/gh-mls-group-info-dialog.blp,
 * gh-mls-member-row.blp; privacy charter §7.10 "MLS", §2.2, §1.4, D7;
 * nostrc-9xf5): Group Info for one encrypted group of the account's
 * GhMlsService.
 *
 *  - The group: its name and description (from the group's own state),
 *    "Encrypted group · N members".
 *  - Privacy: gh_privacy_summary_new() for an encrypted group.
 *  - Group: how it is read (GhMlsGroup:read-state), a change of the
 *    account's still being sent or invitations not delivered yet, and "On
 *    this device only: messages are kept only on this device; history can't
 *    be restored" (D7).
 *  - Members (gh_mls_group_dup_members()): the Owner first, then Admins
 *    (badges from the GroupData admins, gh_mls_group_dup_ordered_admins()),
 *    then members; names are the cached ones, else short npubs; nothing is
 *    fetched. Only an admin sees Add Members (an "add" page with
 *    GhMlsInviteePicker: accepted contacts not in the group, each with a
 *    fresh KeyPackage check; gh_mls_service_add_members_async()) and, on
 *    every member but themself, Remove from Group (confirmed;
 *    gh_mls_service_remove_members_async()), and Name and Description
 *    (gh_mls_service_update_metadata_async()).
 *  - Picture (W25, nostrc-m6tp; with the window's GhMlsAttachments): the
 *    header's avatar shows the group's encrypted picture once the user chose
 *    Show Picture (the only fetch: from the group's media server, through
 *    GhNetHttp) and it passed the decode guard; otherwise the initials. A
 *    web-address picture is never loaded (a placeholder); a legacy group
 *    says it can't have one. Admins choose a JPEG or PNG on this device
 *    (metadata removed, encrypted, uploaded) or remove the picture; each is
 *    one Commit.
 *  - Group relays (read-only).
 *  - Leave (confirmed; gh_mls_service_leave()): the confirmation says what
 *    it does (gh_mls_service_leave_kind(), gh_mls_leave_copy(); nostrc-2um6):
 *    for everyone where the group supports it -- then the group is "leaving"
 *    until a member confirms it, and says so -- or on this device only, and
 *    why: the other members keep counting the account until an admin
 *    removes it. A left group says so and offers nothing else. A member who
 *    asked to leave and is out is toasted ("Alice left the group").
 * Every change is one Commit; the dialog says it is being sent and toasts
 * its outcome (gh_mls_error_copy() on failure). The dialog follows the
 * group live and closes when its service goes (account switch).
 *
 * Actions (widget actions of the dialog): mls-group.add-members,
 * mls-group.save-add, mls-group.remove (s: pubkey hex; presents its
 * confirmation), mls-group.rename, mls-group.save-rename, mls-group.leave
 * (presents its confirmation), mls-group.show-picture, mls-group.set-picture
 * and mls-group.remove-picture.
 */
#define GH_TYPE_MLS_GROUP_INFO_DIALOG (gh_mls_group_info_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhMlsGroupInfoDialog, gh_mls_group_info_dialog, GH, MLS_GROUP_INFO_DIALOG,
                     AdwDialog)

GhMlsGroupInfoDialog *gh_mls_group_info_dialog_new(GhMlsGroup *group,
                                                   const GhMlsUiContext *context);
GhMlsGroup *gh_mls_group_info_dialog_get_group(GhMlsGroupInfoDialog *self);

/* For tests: the changes this dialog started that have not finished, the
 * last toast's text, the add page's picker, the member rows' (title,
 * badge) and the alert dialogs. */
guint gh_mls_group_info_dialog_get_pending(GhMlsGroupInfoDialog *self);
const gchar *gh_mls_group_info_dialog_get_last_toast(GhMlsGroupInfoDialog *self);
struct _GhMlsInviteePicker *gh_mls_group_info_dialog_get_add_picker(GhMlsGroupInfoDialog *self);
/* The badge of pubkey's row ("Owner", "Admin", "" for a member), NULL when
 * not listed; *removable: whether its Remove is offered. */
const gchar *gh_mls_group_info_dialog_get_member(GhMlsGroupInfoDialog *self,
                                                 const gchar *pubkey, gboolean *removable);
/* The identity badge of pubkey's row ("Identity not verified", "" when it
 * shows none), NULL when not listed; *out_explanation: the explanation
 * ("Added by …"), or NULL (nostrc-6ukh). */
const gchar *gh_mls_group_info_dialog_get_member_identity(GhMlsGroupInfoDialog *self,
                                                          const gchar *pubkey,
                                                          const gchar **out_explanation);
/* Whether pubkey's row offers Verify (W24 review H1), and the confirmation
 * the action shows first. */
gboolean gh_mls_group_info_dialog_get_member_verifiable(GhMlsGroupInfoDialog *self,
                                                        const gchar *pubkey);
AdwAlertDialog *gh_mls_group_info_dialog_get_verify_dialog(GhMlsGroupInfoDialog *self);
/* The Messages row's status (read state, ended, or a refused change). */
const gchar *gh_mls_group_info_dialog_get_messages_status(GhMlsGroupInfoDialog *self);
AdwAlertDialog *gh_mls_group_info_dialog_get_leave_dialog(GhMlsGroupInfoDialog *self);
AdwAlertDialog *gh_mls_group_info_dialog_get_remove_dialog(GhMlsGroupInfoDialog *self);
void gh_mls_group_info_dialog_set_rename(GhMlsGroupInfoDialog *self, const gchar *name,
                                         const gchar *description);
/* The picture row's words (NULL: no picture section), whether the header
 * shows the decrypted picture, and an admin's chosen file as the Choose
 * dialog would give it (tests). */
const gchar *gh_mls_group_info_dialog_get_picture_status(GhMlsGroupInfoDialog *self);
gboolean gh_mls_group_info_dialog_get_picture_shown(GhMlsGroupInfoDialog *self);
void gh_mls_group_info_dialog_set_picture(GhMlsGroupInfoDialog *self, GBytes *file,
                                          const gchar *mime);

G_END_DECLS
#endif
