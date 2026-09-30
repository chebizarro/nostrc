#ifndef GH_MLS_INVITES_DIALOG_H
#define GH_MLS_INVITES_DIALOG_H

#include <adwaita.h>

#include "gh-mls-context.h"

G_BEGIN_DECLS

/*
 * GhMlsInvitesDialog (data/ui/gh-mls-invites-dialog.blp,
 * gh-mls-invite-row.blp; privacy charter §7.9, PD-8, PT-8; nostrc-9xf5):
 * the account's pending encrypted-group invitations
 * (gh_mls_service_list_invites(), live through "invite-received"): the
 * group's name, who invited (a contact's cached name, else the short npub
 * and "not in your contacts"; nothing is looked up) and the member count at
 * invite time, with Decline (gh_mls_service_decline_invite()) and Accept
 * (gh_mls_service_accept_invite(): the group's conversation is listed and
 * read). Nothing joins before Accept. After joining, a toast offers Open,
 * which emits "open-group" (GhMlsGroup) and closes the dialog. The dialog
 * closes when its service goes (account switch).
 *
 * Actions (widget actions): mls-invites.accept (s: wrapper id),
 * mls-invites.decline (s: wrapper id), mls-invites.open (s: group id).
 */
#define GH_TYPE_MLS_INVITES_DIALOG (gh_mls_invites_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhMlsInvitesDialog, gh_mls_invites_dialog, GH, MLS_INVITES_DIALOG,
                     AdwDialog)

GhMlsInvitesDialog *gh_mls_invites_dialog_new(const GhMlsUiContext *context);

/* For tests: the listed invitations, a row's title and subtitle by wrapper
 * id (FALSE: not listed), and the last toast's text. */
guint gh_mls_invites_dialog_get_n_invites(GhMlsInvitesDialog *self);
gboolean gh_mls_invites_dialog_describe(GhMlsInvitesDialog *self, const gchar *wrapper_id,
                                        const gchar **title, const gchar **subtitle);
const gchar *gh_mls_invites_dialog_get_last_toast(GhMlsInvitesDialog *self);

G_END_DECLS
#endif
