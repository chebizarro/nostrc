#ifndef GH_MLS_INVITES_PAGE_H
#define GH_MLS_INVITES_PAGE_H

#include <adwaita.h>

#include "gh-mls-context.h"

G_BEGIN_DECLS

/*
 * GhMlsInvitesPage (data/ui/gh-mls-invites-page.blp,
 * gh-mls-invite-row.blp; privacy charter §7.9, PD-8, PT-8; nostrc-9xf5):
 * the account's pending encrypted-group invitations
 * (gh_mls_service_list_invites(), live through "invite-received"): the
 * group's name, who invited (a contact's cached name, else the short npub
 * and "not in your contacts"; nothing is looked up) and the member count at
 * invite time, with Decline (gh_mls_service_decline_invite()) and Accept
 * (gh_mls_service_accept_invite(): the group's conversation is listed and
 * read). Nothing joins before Accept. After joining it emits "open-group"
 * (GhMlsGroup): gh-mls-ui.c closes the panels and opens the group. Declines
 * and dismissals update the list in place, down to its empty state.
 *
 * A sidebar panel (AdwNavigationPage, tag "group-invitations") pushed over
 * the conversation list with gh_window_push_panel(); Back or Escape returns
 * to the list. It leaves when its service goes (account switch).
 *
 * Invitations that arrived but could not be opened
 * (gh_mls_service_list_refused_invites(), live through "invite-refused") are
 * listed under "Could Not Be Opened": who sent it (as above) and why --
 * made for a key this device doesn't have, so only a new invitation can
 * work -- with Dismiss (gh_mls_service_dismiss_refused_invite()).
 *
 * Actions (widget actions): mls-invites.accept (s: wrapper id),
 * mls-invites.decline (s: wrapper id),
 * mls-invites.dismiss (s: wrapper id of a refused invitation).
 */
#define GH_TYPE_MLS_INVITES_PAGE (gh_mls_invites_page_get_type())
G_DECLARE_FINAL_TYPE(GhMlsInvitesPage, gh_mls_invites_page, GH, MLS_INVITES_PAGE,
                     AdwNavigationPage)

GhMlsInvitesPage *gh_mls_invites_page_new(const GhMlsUiContext *context);

/* For tests: the listed invitations (and refused ones), a row's title and
 * subtitle by wrapper id, refused rows included (FALSE: not listed), and
 * the last toast's text (an error, or "Invitation declined"). */
guint gh_mls_invites_page_get_n_invites(GhMlsInvitesPage *self);
guint gh_mls_invites_page_get_n_refused(GhMlsInvitesPage *self);
gboolean gh_mls_invites_page_describe(GhMlsInvitesPage *self, const gchar *wrapper_id,
                                      const gchar **title, const gchar **subtitle);
/* The "This Device" Invitation Key row's text (gh_mls_device_key_copy()). */
const gchar *gh_mls_invites_page_get_key_status(GhMlsInvitesPage *self);
const gchar *gh_mls_invites_page_get_last_toast(GhMlsInvitesPage *self);
/* Old invitation keys (nostrc-ab9kn): the row's subtitle (what the last
 * look found), and whether a look or a removal is in flight. */
const gchar *gh_mls_invites_page_get_old_keys_status(GhMlsInvitesPage *self);
gboolean gh_mls_invites_page_get_old_keys_busy(GhMlsInvitesPage *self);

G_END_DECLS
#endif
