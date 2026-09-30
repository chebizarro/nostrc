#ifndef GH_MLS_COPY_H
#define GH_MLS_COPY_H

#include "gh-mls-invitee.h"

G_BEGIN_DECLS

/*
 * The encrypted-group UI's words (nostrc-9xf5; privacy charter §7.1 "always
 * an explanation", §7.9, §7.10, §7.15 #13, §1.4, D7), GTK-free so the
 * display-free test reads exactly what the dialogs show. Every state has its
 * own copy; nothing says more than GhMlsService knows.
 */

/* A person's KeyPackage check, as a row subtitle ("Ready to invite",
 * "Hasn't set up encrypted groups", "Needs to update their app: it can't
 * prove their account yet", ...). */
const gchar *gh_mls_invitee_copy(GhMlsInviteeState state);
/* Whether a person in this state can be invited now (READY only). */
gboolean gh_mls_invitee_can_invite(GhMlsInviteeState state);

/* This device's enrollment (the account proof, GhMlsIdentityState). NULL
 * title: nothing to show (enrolled, or not required). ready: groups can be
 * created. busy: waiting on the signer (a spinner). can_retry: offer Try
 * Again (gh_mls_service_retry_identity()). */
typedef struct {
  const gchar *title;
  const gchar *description;
  gboolean ready;
  gboolean busy;
  gboolean can_retry;
} GhMlsIdentityCopy;

GhMlsIdentityCopy gh_mls_identity_copy(GhMlsIdentityState state);

/* A failed create, add, remove, rename or accept, in plain words (transfer
 * full). */
gchar *gh_mls_error_copy(const GError *error);

/* Why the composer can't send to group (transfer full), or NULL when it can:
 * no service (encrypted groups aren't running for this account), left,
 * removed by an admin (nostrc-xrya; remover: the admin's cached name or
 * short npub, NULL when not known), or not read (offline or not the active
 * account: the service refuses sends then). */
gchar *gh_mls_send_reason(GhMlsService *service, GhMlsGroup *group, const gchar *remover);

/* An ended group's state for Group Info (transfer full): "You left…",
 * "You were removed from this group by …", or, when why can't be read,
 * "This group has ended…", with where its messages are. */
gchar *gh_mls_end_copy(GhMlsGroupEnd end, const gchar *remover);

/* "Owner" / "Admin" badge text, NULL for a member. */
const gchar *gh_mls_role_copy(GhMlsRole role);

/* How the group is read now (GhMlsReadState), for Group Info. */
const gchar *gh_mls_read_copy(GhMlsReadState read);

/* An invitation's subtitle: who invited ("From Alice", or the short npub
 * with "not in your contacts") and the member count (transfer full).
 * inviter_name: the cached name of a contact, or NULL. */
gchar *gh_mls_invite_subtitle(const gchar *inviter_npub_short, const gchar *inviter_name,
                              gboolean contact, guint member_count);

/* A group relay the user typed: trimmed, the secure websocket scheme added
 * when none is given, and valid for GhMlsService (gh_relay_url_validate()); plain ws only
 * for a loopback or .onion host. NULL with error (plain words). */
gchar *gh_mls_parse_relay(const gchar *text, GError **error);

G_END_DECLS
#endif
