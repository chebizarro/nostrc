#ifndef GH_MLS_INVITEE_PICKER_H
#define GH_MLS_INVITEE_PICKER_H

#include <adwaita.h>

#include "gh-mls-context.h"
#include "gh-mls-invitee.h"

G_BEGIN_DECLS

/*
 * GhMlsInviteePicker (data/ui/gh-mls-invitee-picker.blp, gh-mls-invitee-row.blp;
 * charter §7.9, §7.10; nostrc-9xf5): the people an encrypted group invites,
 * from the account's accepted contacts only (gh_mls_contacts_dup(); PD-8,
 * PT-8), minus those given as already in the group. Choosing someone starts
 * their KeyPackage check (gh_mls_invitee_check_async(): the explicit act
 * that lets the discovery relays learn whom the account looks up); un-choosing
 * cancels it. Each chosen row shows the state in words
 * (gh_mls_invitee_copy()) with a spinner or an icon, and Check Again where
 * another try can help. At most GH_MLS_SERVICE_MAX_INVITEES can be chosen.
 * Nothing is looked up before someone is chosen. Signal "changed": the
 * choice or a check changed. An account switch cancels every check.
 */
#define GH_TYPE_MLS_INVITEE_PICKER (gh_mls_invitee_picker_get_type())
G_DECLARE_FINAL_TYPE(GhMlsInviteePicker, gh_mls_invitee_picker, GH, MLS_INVITEE_PICKER,
                     AdwPreferencesGroup)

/* Lists the contacts (except members, e.g. a group's) and forgets any choice.
 * context is copied; its objects are referenced. */
void gh_mls_invitee_picker_setup(GhMlsInviteePicker *self, const GhMlsUiContext *context,
                                 const gchar *const *members);
/* The chosen people (hex, in listed order). Transfer full. */
GStrv gh_mls_invitee_picker_dup_selected(GhMlsInviteePicker *self);
guint gh_mls_invitee_picker_get_n_selected(GhMlsInviteePicker *self);
guint gh_mls_invitee_picker_get_n_listed(GhMlsInviteePicker *self);
/* Whether at least one person is chosen and every chosen one is READY. */
gboolean gh_mls_invitee_picker_get_ready(GhMlsInviteePicker *self);
/* Whether any chosen person is still being checked. */
gboolean gh_mls_invitee_picker_get_checking(GhMlsInviteePicker *self);
/* A listed person's state (CHECKING when not chosen), and choosing them as
 * the check button does (FALSE: not listed). */
GhMlsInviteeState gh_mls_invitee_picker_get_state(GhMlsInviteePicker *self, const gchar *pubkey);
gboolean gh_mls_invitee_picker_set_selected(GhMlsInviteePicker *self, const gchar *pubkey,
                                            gboolean selected);
/* The row of a listed person (tests: its words and widgets), or NULL. */
AdwActionRow *gh_mls_invitee_picker_get_row(GhMlsInviteePicker *self, const gchar *pubkey);

G_END_DECLS
#endif
