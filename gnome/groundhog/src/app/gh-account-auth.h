#ifndef GH_ACCOUNT_AUTH_H
#define GH_ACCOUNT_AUTH_H

#include "gh-account-controller.h"
#include "gh-relay-auth.h"

G_BEGIN_DECLS

/*
 * GhAccountAuth answers NIP-42 challenges as the active account: it is the
 * GhRelayAuthSigner that relay scopes and publishes use for URLs set to
 * GH_RELAY_AUTH_ACCOUNT, and it signs only through the account's external
 * signer (gh_account_controller_sign_with_cancellable_async).
 *
 * Generation. It belongs to the account generation current at creation. Its
 * signer is bound to that generation, revoked with it (the controller's
 * generation cancellable) and by gh_account_auth_revoke(), and never signs
 * for another account; the signed event must carry the account pubkey.
 *
 * Purpose (privacy charter §4.3, §4.4 R1). It never decides where account
 * AUTH is used: GhAuthPolicy (gh-auth-policy.h) does, and is its only user.
 * The policy holds one adapter per generation for the whole process and sets
 * GH_RELAY_AUTH_ACCOUNT only on connections whose purpose allows it: own
 * inbox read, own list publish, own self-copy publish and NIP-29 group
 * relays; never another user's relay (tests/check_privacy.py rules
 * account-auth-purpose and auth-policy). The controller is held weakly.
 *
 * Prompts (§4.4 R6: at most one account-AUTH signer prompt per relay per
 * session). Keyed by the exact relay URL the AUTH event names, for this
 * generation:
 *  - one signer request per relay at a time; a challenge from another
 *    connection to the same relay (a reconnect, a second REQ) waits for it;
 *  - after an approval, later challenges on that relay are signed again (the
 *    signer may remember the approval; Groundhog cannot see whether it will
 *    prompt again);
 *  - after a denial (GH_SIGNER_ERROR_DENIED) the relay is DECLINED for the
 *    rest of the generation: requests waiting behind it and every later
 *    challenge fail at once with that error, without a signer call;
 *  - any other signer failure (outage, approval timeout, no approval agent)
 *    is not remembered, but also fails the requests waiting behind it rather
 *    than asking again at once; a cancelled request lets the next one ask.
 * "relay-changed" (url) is emitted whenever a relay's state changes.
 * Main context only.
 */

typedef enum {
  GH_ACCOUNT_AUTH_RELAY_NONE,     /* not asked this session */
  GH_ACCOUNT_AUTH_RELAY_WAITING,  /* the signer is asking the user */
  GH_ACCOUNT_AUTH_RELAY_APPROVED, /* signed at least once this session */
  GH_ACCOUNT_AUTH_RELAY_DECLINED  /* the user declined; not asked again */
} GhAccountAuthRelayState;

#define GH_TYPE_ACCOUNT_AUTH (gh_account_auth_get_type())
G_DECLARE_FINAL_TYPE(GhAccountAuth, gh_account_auth, GH, ACCOUNT_AUTH, GObject)

/* Bound to the current generation; NULL unless an account is active. */
GhAccountAuth *gh_account_auth_new(GhAccountController *accounts);
/* Borrowed; bound to gh_account_auth_get_generation(). */
GhRelayAuthSigner *gh_account_auth_get_signer(GhAccountAuth *self);
guint64 gh_account_auth_get_generation(GhAccountAuth *self);
GhAccountAuthRelayState gh_account_auth_get_relay_state(GhAccountAuth *self,
                                                        const gchar *url);
/* Nothing is signed any more: the signer is revoked, requests in flight are
 * cancelled and waiting ones fail. Idempotent; also run by dispose. */
void gh_account_auth_revoke(GhAccountAuth *self);

G_END_DECLS
#endif
