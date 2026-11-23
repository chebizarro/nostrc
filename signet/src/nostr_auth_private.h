/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_NOSTR_AUTH_PRIVATE_H
#define SIGNET_NOSTR_AUTH_PRIVATE_H
#include "signet/nostr_auth.h"

/* Bootstrap possession proof. Separate purpose and signed token hash prevent
 * reuse of a session-auth signature or substitution of another handoff token.
 * All expected bindings are checked before consuming the challenge. */
SignetAuthResult signet_bootstrap_auth_verify(SignetChallengeStore *cs,
    const char *auth_event_json, int64_t now, const char *agent_id,
    const char *bootstrap_pubkey, const char *token_hash);
#endif
