#ifndef GH_AUTH_POLICY_H
#define GH_AUTH_POLICY_H

#include "gh-account-controller.h"
#include "gh-relay-publish.h"
#include "gh-relay-scope.h"

G_BEGIN_DECLS

/*
 * GhAuthPolicy: the one place that decides which NIP-42 identity, if any,
 * authenticates a Groundhog relay connection (privacy charter §4.3, §4.4).
 * Callers name the connection's purpose and URL; they never pick
 * GH_RELAY_AUTH_* themselves (tests/check_privacy.py rule auth-policy).
 *
 *   purpose               identity   connection
 *   OWN_INBOX_READ        ACCOUNT    kind-1059 #p=me REQs on the own 10050 (GhDmInbox)
 *   OWN_LIST_DISCOVERY    EPHEMERAL  own 10002/10050 on discovery-relays (GhAccountRelays)
 *   OWN_LIST_PUBLISH      ACCOUNT    own 10050/10002/KeyPackage publish (GhInboxSetup)
 *   CONTACT_DIRECTORY     EPHEMERAL  others' 10050/kind 0 on discovery-relays
 *                                    (GhContactDirectory, GhInboxLookup)
 *   RECIPIENT_WRAP        EPHEMERAL  a gift wrap (DM, Welcome) to a recipient's 10050
 *   SELF_WRAP             ACCOUNT    the self-copy, or a note to self, to the own 10050
 *   GROUP                 ACCOUNT    a NIP-29 group relay
 *   MLS_ROUTING           EPHEMERAL  MLS kind-445 routing relays
 *
 * Rules (§4.4):
 *  - R1: the account signs a kind-22242 event only for OWN_INBOX_READ,
 *    OWN_LIST_PUBLISH, SELF_WRAP and GROUP. Everything else is EPHEMERAL: a
 *    fresh key per connection from the relay layer
 *    (gh_relay_auth_sign_ephemeral(); R2, never reused, wiped once signed).
 *    AUTH is lazy either way (gh-relay-auth.h): nothing is signed unless the
 *    relay sent a challenge and then refused the REQ/EVENT as
 *    "auth-required:", so a relay that does not demand AUTH sees the same
 *    traffic as with no AUTH at all.
 *  - Self-copy (W13 review 7a, decided here): SELF_WRAP is ACCOUNT, on
 *    challenge. The own inbox already learns the account from reading it
 *    (OWN_INBOX_READ) and the self-copy is p-tagged to the account, so
 *    signing in there reveals nothing new, while EPHEMERAL made an own inbox
 *    that accepts writes only from signed-in members refuse every self-copy
 *    ("Not saved to your other devices"). The self-copy still never shares a
 *    connection with a recipient wrap (S3), and on overlapping inbox sets it
 *    is delayed by D8. A caller passes SELF_WRAP only for a URL that is (or,
 *    before the own lists are re-discovered this generation, was) one of the
 *    account's own 10050 relays.
 *  - R3/R4: the event binds the connection's URL and the received challenge
 *    (relay layer); ACCOUNT is applied only to a scope or publish of the
 *    current account generation, and the generation's signer is revoked on a
 *    switch, so a stale challenge signs nothing.
 *  - R5/R7: one AUTH per challenge, never escalated: a relay that refuses an
 *    EPHEMERAL AUTH ends as AUTH_REQUIRED (relay layer).
 *  - R6: one GhAccountAuth per account generation, shared by every caller of
 *    the policy, so the inbox, the outbox and the own-list publish together
 *    ask the signer at most once per relay at a time, and a relay the user
 *    declined is not asked again this generation. While the signer asks, a
 *    relay publish's deadline does not run (gh-relay-publish.h), and
 *    gh_auth_policy_get_account_state() is WAITING for "Waiting for
 *    approval" on the affected action.
 *
 * One policy per GhAccountController (gh_auth_policy_get_for_accounts()): it
 * lives as long as the controller and holds it weakly. Main context only.
 */

typedef enum {
  GH_AUTH_PURPOSE_OWN_INBOX_READ,
  GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY,
  GH_AUTH_PURPOSE_OWN_LIST_PUBLISH,
  GH_AUTH_PURPOSE_CONTACT_DIRECTORY,
  GH_AUTH_PURPOSE_RECIPIENT_WRAP,
  GH_AUTH_PURPOSE_SELF_WRAP,
  GH_AUTH_PURPOSE_GROUP,
  GH_AUTH_PURPOSE_MLS_ROUTING,
  GH_AUTH_N_PURPOSES
} GhAuthPurpose;

/* The account-AUTH state of one relay in the current generation (R6), for
 * status UI ("Waiting for approval", "Signed in as you", declined). */
typedef enum {
  GH_AUTH_ACCOUNT_STATE_NONE,     /* not asked this generation */
  GH_AUTH_ACCOUNT_STATE_WAITING,  /* the signer is asking the user */
  GH_AUTH_ACCOUNT_STATE_APPROVED, /* signed at least once this generation */
  GH_AUTH_ACCOUNT_STATE_DECLINED  /* the user declined; not asked again */
} GhAuthAccountState;

/* The identity §4.3 assigns to purpose: GH_RELAY_AUTH_ACCOUNT or
 * GH_RELAY_AUTH_EPHEMERAL (never NONE). An unknown purpose is EPHEMERAL. */
GhRelayAuthMode gh_auth_policy_decide(GhAuthPurpose purpose);
/* "own-inbox-read", "recipient-wrap", ...; NULL for an unknown purpose. */
const gchar *gh_auth_purpose_to_string(GhAuthPurpose purpose);

#define GH_TYPE_AUTH_POLICY (gh_auth_policy_get_type())
G_DECLARE_FINAL_TYPE(GhAuthPolicy, gh_auth_policy, GH, AUTH_POLICY, GObject)

/* The policy of accounts (transfer none), created on first use. */
GhAuthPolicy *gh_auth_policy_get_for_accounts(GhAccountController *accounts);

/* Sets the AUTH identity of url, already added to scope/publish (before its
 * start), for purpose. EPHEMERAL purposes always succeed for a known URL.
 * ACCOUNT purposes also install the generation's account signer; they fail
 * with G_IO_ERROR_PERMISSION_DENIED when no account is active, the account
 * is gone, or the scope/publish belongs to another generation, and then
 * leave url unauthenticated (NONE): nothing is ever signed as the account for
 * a stale generation. A URL the scope/publish does not hold is
 * G_IO_ERROR_NOT_FOUND. */
gboolean gh_auth_policy_apply_scope(GhAuthPolicy *self, GhRelayScope *scope,
                                    GhAuthPurpose purpose, const gchar *url,
                                    GError **error);
gboolean gh_auth_policy_apply_publish(GhAuthPolicy *self, GhRelayPublish *publish,
                                      GhAuthPurpose purpose, const gchar *url,
                                      GError **error);

/* Account-AUTH state of url (the exact URL the AUTH event names) in the
 * current generation. "account-state-changed" (url) is emitted on every
 * change. */
GhAuthAccountState gh_auth_policy_get_account_state(GhAuthPolicy *self, const gchar *url);
/* The account generation whose signer the policy holds; 0 when none. */
guint64 gh_auth_policy_get_generation(GhAuthPolicy *self);

G_END_DECLS
#endif
