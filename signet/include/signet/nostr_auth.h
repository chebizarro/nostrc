/* SPDX-License-Identifier: MIT
 *
 * nostr_auth.h - Shared Nostr-signed challenge validator for Signet v2.
 *
 * Single challenge validator used by all transports:
 *   - HTTP /bootstrap and /auth endpoints
 *   - D-Bus TCP (post-SASL handshake)
 *   - NIP-5L Unix socket
 *
 * Auth event structure (custom kind, NOT NIP-98):
 *   kind: SIGNET_AUTH_KIND
 *   content: ""
 *   tags: [["challenge", "<hex>"], ["agent", "<agent_id>"], ["purpose", "signet-auth"]]
 *
 * Verification steps:
 *   1. Signature valid (secp256k1 via nostr_event_check_signature)
 *   2. event.pubkey matches stored pubkey for agent_id
 *   3. Pubkey in fleet registry (NIP-51 fleet list)
 *   4. Challenge tag matches issued challenge (30s TTL, single-use)
 *   5. Purpose tag = "signet-auth"
 *   6. Agent NOT in deny list
 */

#ifndef SIGNET_NOSTR_AUTH_H
#define SIGNET_NOSTR_AUTH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* Custom event kind for Signet authentication.
 * Not 27235 (NIP-98 HTTP Auth) to avoid cross-protocol confusion. */
/**
 * SIGNET_AUTH_KIND:
 *
 * Custom Nostr event kind used for Signet authentication challenges.
 *
 * Since: 1.0
 */
#define SIGNET_AUTH_KIND 28100

/* Challenge TTL in seconds. */
/**
 * SIGNET_CHALLENGE_TTL_S:
 *
 * Authentication challenge time-to-live in seconds.
 *
 * Since: 1.0
 */
#define SIGNET_CHALLENGE_TTL_S 30

/* Session TTL in seconds (24 hours). */
/**
 * SIGNET_SESSION_TTL_S:
 *
 * Default authenticated session time-to-live in seconds.
 *
 * Since: 1.0
 */
#define SIGNET_SESSION_TTL_S (24 * 60 * 60)

/**
 * SignetAuthResult:
 * @SIGNET_AUTH_OK: signet auth ok
 * @SIGNET_AUTH_ERR_INVALID_EVENT: signet auth err invalid event
 * @SIGNET_AUTH_ERR_WRONG_KIND: signet auth err wrong kind
 * @SIGNET_AUTH_ERR_BAD_SIGNATURE: signet auth err bad signature
 * @SIGNET_AUTH_ERR_MISSING_CHALLENGE: signet auth err missing challenge
 * @SIGNET_AUTH_ERR_CHALLENGE_MISMATCH: signet auth err challenge mismatch
 * @SIGNET_AUTH_ERR_CHALLENGE_EXPIRED: signet auth err challenge expired
 * @SIGNET_AUTH_ERR_CHALLENGE_REPLAYED: signet auth err challenge replayed
 * @SIGNET_AUTH_ERR_MISSING_AGENT: signet auth err missing agent
 * @SIGNET_AUTH_ERR_MISSING_PURPOSE: signet auth err missing purpose
 * @SIGNET_AUTH_ERR_WRONG_PURPOSE: signet auth err wrong purpose
 * @SIGNET_AUTH_ERR_PUBKEY_MISMATCH: signet auth err pubkey mismatch
 * @SIGNET_AUTH_ERR_NOT_IN_FLEET: signet auth err not in fleet
 * @SIGNET_AUTH_ERR_DENIED: signet auth err denied
 * @SIGNET_AUTH_ERR_INTERNAL: signet auth err internal
 *
 * Result codes returned by Nostr challenge-response authentication.
 *
 * Since: 1.0
 */
typedef enum {
  SIGNET_AUTH_OK = 0,
  SIGNET_AUTH_ERR_INVALID_EVENT = -1,
  SIGNET_AUTH_ERR_WRONG_KIND = -2,
  SIGNET_AUTH_ERR_BAD_SIGNATURE = -3,
  SIGNET_AUTH_ERR_MISSING_CHALLENGE = -4,
  SIGNET_AUTH_ERR_CHALLENGE_MISMATCH = -5,
  SIGNET_AUTH_ERR_CHALLENGE_EXPIRED = -6,
  SIGNET_AUTH_ERR_CHALLENGE_REPLAYED = -7,
  SIGNET_AUTH_ERR_MISSING_AGENT = -8,
  SIGNET_AUTH_ERR_MISSING_PURPOSE = -9,
  SIGNET_AUTH_ERR_WRONG_PURPOSE = -10,
  SIGNET_AUTH_ERR_PUBKEY_MISMATCH = -11,
  SIGNET_AUTH_ERR_NOT_IN_FLEET = -12,
  SIGNET_AUTH_ERR_DENIED = -13,
  SIGNET_AUTH_ERR_INTERNAL = -14,
} SignetAuthResult;

/* Issued challenge record. */
/**
 * SignetChallenge:
 * @challenge_hex: 32-byte random as 64-char hex.
 * @agent_id: bound to this agent_id.
 * @issued_at: issue time as Unix seconds.
 * @consumed: consumed value.
 *
 * Issued single-use authentication challenge.
 *
 * Since: 1.0
 */
typedef struct {
  char *challenge_hex;   /* 32-byte random as 64-char hex */
  char *agent_id;        /* bound to this agent_id */
  int64_t issued_at;
  bool consumed;
} SignetChallenge;

/* Challenge store — manages issued challenges (in-memory, TTL-cleaned). */
/**
 * SignetChallengeStore:
 * Opaque in-memory challenge store.
 *
 * Since: 1.0
 */
typedef struct SignetChallengeStore SignetChallengeStore;

/* Fleet registry interface — callbacks to check authorization.
 * Implementations can query NIP-51 lists, internal mint tables, deny lists. */
/**
 * SignetFleetRegistry:
 * @is_in_fleet: is in fleet value.
 * @is_denied: is denied value.
 * @get_agent_pubkey: get agent pubkey value.
 * @user_data: caller data passed to callbacks.
 *
 * Callback interface used to verify fleet membership and denial state.
 *
 * Since: 1.0
 */
typedef struct SignetFleetRegistry {
  /* Returns true if pubkey_hex is authorized (in fleet list or mint table). */
  bool (*is_in_fleet)(const char *pubkey_hex, void *user_data);

  /* Returns true if pubkey_hex is in the deny list. */
  bool (*is_denied)(const char *pubkey_hex, void *user_data);

  /* Returns the expected pubkey_hex for an agent_id, or NULL if unknown.
   * Caller frees the returned string. */
  char *(*get_agent_pubkey)(const char *agent_id, void *user_data);

  void *user_data;
} SignetFleetRegistry;

/* Create a challenge store. Returns NULL on OOM. */
/**
 * signet_challenge_store_new:
 *
 * Create a challenge store. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetChallengeStore *signet_challenge_store_new(void);

/* Free a challenge store. Safe on NULL. */
/**
 * signet_challenge_store_free:
 * @cs: (nullable): a #SignetChallengeStore
 *
 * Free a challenge store. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_challenge_store_free(SignetChallengeStore *cs);

/* Issue a new challenge for an agent_id.
 * Returns heap-allocated 64-char hex challenge string (caller frees with g_free).
 * Returns NULL on error. */
/**
 * signet_challenge_issue:
 * @cs: (not nullable): a #SignetChallengeStore
 * @agent_id: (not nullable): agent identifier
 * @now: current Unix time in seconds
 *
 * Issue a new challenge for an agent_id. Returns heap-allocated 64-char hex challenge string (caller frees with g_free). Returns NULL on error.
 *
 * Returns: (transfer full) (nullable): a newly allocated string, or %NULL on failure
 *
 * Since: 1.0
 */
char *signet_challenge_issue(SignetChallengeStore *cs,
                              const char *agent_id,
                              int64_t now);

/* Verify a signed auth event against an issued challenge.
 * auth_event_json: the complete signed Nostr event JSON.
 * Returns SIGNET_AUTH_OK if all checks pass. */
/**
 * signet_auth_verify:
 * @cs: (not nullable): a #SignetChallengeStore
 * @fleet: (nullable): fleet
 * @auth_event_json: (not nullable): auth event json
 * @now: current Unix time in seconds
 * @out_agent_id: (out) (transfer full) (not nullable): return location for agent id
 * @out_pubkey_hex: (out) (transfer full) (not nullable): return location for pubkey hex
 *
 * Verify a signed auth event against an issued challenge. auth_event_json: the complete signed Nostr event JSON. Returns SIGNET_AUTH_OK if all checks pass.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetAuthResult signet_auth_verify(SignetChallengeStore *cs,
                                    const SignetFleetRegistry *fleet,
                                    const char *auth_event_json,
                                    int64_t now,
                                    char **out_agent_id,
                                    char **out_pubkey_hex);

/* Clean up expired challenges (older than TTL). */
/**
 * signet_challenge_store_cleanup:
 * @cs: (not nullable): a #SignetChallengeStore
 * @now: current Unix time in seconds
 *
 * Clean up expired challenges (older than TTL).
 *
 * Since: 1.0
 */
void signet_challenge_store_cleanup(SignetChallengeStore *cs, int64_t now);

/* Get human-readable error string. Returns static string. */
/**
 * signet_auth_result_string:
 * @r: r
 *
 * Get human-readable error string. Returns static string.
 *
 * Returns: (transfer none) (nullable): a borrowed pointer owned by the callee
 *
 * Since: 1.0
 */
const char *signet_auth_result_string(SignetAuthResult r);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_NOSTR_AUTH_H */
