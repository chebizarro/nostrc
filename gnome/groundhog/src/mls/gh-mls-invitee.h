#ifndef GH_MLS_INVITEE_H
#define GH_MLS_INVITEE_H

#include "gh-mls-key-packages.h"
#include "gh-mls-service.h"

G_BEGIN_DECLS

/*
 * The encrypted-group UI's view of people (nostrc-9xf5, qp24.13 part 2;
 * privacy charter §7.9, §7.10, PD-8, PT-8). GTK-free, main context only;
 * nothing here changes GhMlsService.
 *
 * Contacts. Only an accepted contact can be invited: a peer of an accepted,
 * non-request NIP-17 conversation, which is GhMlsService's own default
 * consent (GhMlsServiceConfig.may_look_up NULL). The pickers list exactly
 * these people, so a message request is never looked up.
 *
 * The KeyPackage check. Before anyone is invited the pickers show, per
 * person, whether they can be: one gh_mls_key_package_lookup_async() (the
 * discovery-relays setting, then the person's 10002 write relays; ephemeral
 * AUTH only, charter §2.2 "Discovery relays: whose KeyPackages you fetch")
 * and, with libmarmot >= 0.10.0, whether the KeyPackage's leaf carries the
 * account proof (marmot_key_package_event_has_account_proof()). Without it
 * (MDK 0.8, Amethyst/Quartz) they can still be invited (nostrc-6ukh), unless
 * the account requires proofs (the settings key
 * "only-join-verified-mls-groups"). The check is advisory:
 * gh_mls_service_create_group_async() and _add_members_async() look everyone
 * up again (a fresh 30443, charter §7.10) and their errors are the truth.
 *
 * Roles. MIP-01 GroupData has admins only; Groundhog makes a group with its
 * creator as the one admin, so the first admin of the GroupData is the
 * group's Owner and the others are Admins (charter §7.10 badges). The order
 * is libmarmot's (gh_mls_group_dup_admins() is sorted).
 */

typedef enum {
  GH_MLS_INVITEE_CHECKING,     /* the lookup runs */
  GH_MLS_INVITEE_READY,        /* KeyPackages of both formats: joins any group */
  GH_MLS_INVITEE_READY_ADOPTED_ONLY, /* an adopted KeyPackage only (e.g. White Noise on
                                      * MDK 0.11): joins adopted-profile groups only */
  GH_MLS_INVITEE_READY_LEGACY, /* an MDK 0.8 KeyPackage only, with the account proof (an
                                * older app): joins MDK 0.8-profile groups only */
  GH_MLS_INVITEE_READY_UNPROVEN, /* an MDK 0.8 KeyPackage only, and their app can't prove
                                  * their account: other members may see them as not
                                  * verified */
  GH_MLS_INVITEE_NOT_SET_UP,   /* relays answered: no valid KeyPackage */
  GH_MLS_INVITEE_NEEDS_UPDATE, /* a KeyPackage without the account proof, which the account
                                * requires */
  GH_MLS_INVITEE_UNREACHABLE,  /* no relay answered */
  GH_MLS_INVITEE_NO_RELAYS,    /* no usable discovery relay */
  GH_MLS_INVITEE_FAILED        /* anything else */
} GhMlsInviteeState;

/* The state a lookup's result means (key_package NULL: error says why;
 * require_proof: the account requires proofs). The adopted format always
 * carries a verified proof; the MDK 0.8 one may lack it. libmarmot < 0.10.0
 * has no proof: any valid MDK 0.8 KeyPackage is READY_LEGACY. */
GhMlsInviteeState gh_mls_invitee_classify(const GhMlsKeyPackage *key_package,
                                          const GError *error, gboolean require_proof);

/* Checks pubkey (hex) with the discovery-relays of settings, and its
 * "only-join-verified-mls-groups" (deadline:
 * seconds per phase, 0: the lookup's default). Consent is the caller's:
 * only ever for a gh_mls_contacts_dup() person. finish: the state, or
 * CHECKING with error G_IO_ERROR_CANCELLED (the cancellable or an account
 * switch). */
void gh_mls_invitee_check_async(GhAccountController *accounts, GSettings *settings,
                                const gchar *pubkey, guint deadline,
                                GCancellable *cancellable, GAsyncReadyCallback callback,
                                gpointer user_data);
GhMlsInviteeState gh_mls_invitee_check_finish(GAsyncResult *result, GError **error);

/* The accepted contacts of model's account (lowercase hex, sorted, unique;
 * never the account itself). Transfer full. */
GStrv gh_mls_contacts_dup(GhConversationStore *model);
gboolean gh_mls_is_contact(GhConversationStore *model, const gchar *pubkey);

typedef enum {
  GH_MLS_ROLE_MEMBER,
  GH_MLS_ROLE_ADMIN,
  GH_MLS_ROLE_OWNER   /* the first GroupData admin */
} GhMlsRole;

/* The GroupData admins in their order (lowercase hex), from libmarmot;
 * falls back to gh_mls_group_dup_admins() when it can't be read. Transfer
 * full. */
GStrv gh_mls_group_dup_ordered_admins(GhMlsService *service, GhMlsGroup *group);
GhMlsRole gh_mls_role_of(const gchar *const *ordered_admins, const gchar *pubkey);

G_END_DECLS
#endif
