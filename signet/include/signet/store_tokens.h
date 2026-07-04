/* SPDX-License-Identifier: MIT
 *
 * store_tokens.h - Bootstrap token store for Signet v2.
 *
 * Single-use, time-limited, attempt-capped bootstrap tokens bound to
 * an agent_id and bootstrap_pubkey. Token hashes are stored (never raw tokens).
 */

#ifndef SIGNET_STORE_TOKENS_H
#define SIGNET_STORE_TOKENS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

struct SignetStore;

/**
 * SignetTokenResult:
 * @SIGNET_TOKEN_OK: signet token ok
 * @SIGNET_TOKEN_NOT_FOUND: signet token not found
 * @SIGNET_TOKEN_EXPIRED: signet token expired
 * @SIGNET_TOKEN_ALREADY_USED: signet token already used
 * @SIGNET_TOKEN_MAX_ATTEMPTS: signet token max attempts
 * @SIGNET_TOKEN_PUBKEY_MISMATCH: signet token pubkey mismatch
 * @SIGNET_TOKEN_AGENT_MISMATCH: signet token agent mismatch
 * @SIGNET_TOKEN_ERROR: signet token error
 *
 * Result codes returned by bootstrap-token verification.
 *
 * Since: 1.0
 */
typedef enum {
  SIGNET_TOKEN_OK = 0,
  SIGNET_TOKEN_NOT_FOUND,
  SIGNET_TOKEN_EXPIRED,
  SIGNET_TOKEN_ALREADY_USED,
  SIGNET_TOKEN_MAX_ATTEMPTS,
  SIGNET_TOKEN_PUBKEY_MISMATCH,
  SIGNET_TOKEN_AGENT_MISMATCH,
  SIGNET_TOKEN_ERROR = -1,
} SignetTokenResult;

/**
 * SignetBootstrapToken:
 * @token_hash: SHA-256 hash of the raw token as hex.
 * @agent_id: agent identifier.
 * @bootstrap_pubkey: bootstrap public key bound to the token.
 * @issued_at: issue time as Unix seconds.
 * @expires_at: expiry time as Unix seconds, or 0 for no expiry.
 * @used_at: 0 if not yet used.
 * @attempt_count: number of verification attempts.
 *
 * Stored bootstrap token metadata; raw tokens are never stored.
 *
 * Ownership: clear instances with the corresponding *_clear() function to release heap data and wipe secrets where applicable.
 *
 * Since: 1.0
 */
typedef struct {
  char *token_hash;
  char *agent_id;
  char *bootstrap_pubkey;
  int64_t issued_at;
  int64_t expires_at;
  int64_t used_at;        /* 0 if not yet used */
  int attempt_count;
} SignetBootstrapToken;

/* Store a new bootstrap token record.
 * token_hash = SHA256(raw_token) as hex string.
 * Returns 0 on success, -1 on error. */
/**
 * signet_store_put_bootstrap_token:
 * @store: (nullable): a #SignetStore
 * @token_hash: (not nullable): token hash
 * @agent_id: (not nullable): agent identifier
 * @bootstrap_pubkey: (not nullable): bootstrap pubkey
 * @issued_at: issued at
 * @expires_at: expires at
 *
 * Store a new bootstrap token record. token_hash = SHA256(raw_token) as hex string. Returns 0 on success, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_put_bootstrap_token(struct SignetStore *store,
                                     const char *token_hash,
                                     const char *agent_id,
                                     const char *bootstrap_pubkey,
                                     int64_t issued_at,
                                     int64_t expires_at);

/* Verify a bootstrap token.
 * Checks: hash match, TTL, used_at, attempt_count, agent_id, bootstrap_pubkey.
 * Increments attempt_count on each call.
 * Returns SIGNET_TOKEN_OK if valid. */
/**
 * signet_store_verify_bootstrap_token:
 * @store: (nullable): a #SignetStore
 * @token_hash: (not nullable): token hash
 * @agent_id: (not nullable): agent identifier
 * @bootstrap_pubkey: (not nullable): bootstrap pubkey
 * @now: current Unix time in seconds
 *
 * Verify a bootstrap token. Checks: hash match, TTL, used_at, attempt_count, agent_id, bootstrap_pubkey. Increments attempt_count on each call. Returns SIGNET_TOKEN_OK if valid.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetTokenResult signet_store_verify_bootstrap_token(struct SignetStore *store,
                                                      const char *token_hash,
                                                      const char *agent_id,
                                                      const char *bootstrap_pubkey,
                                                      int64_t now);

/* Mark a token as consumed (set used_at) by token_hash.
 * Call only after session is fully established.
 * Returns 0 on success, -1 on error. */
/**
 * signet_store_consume_bootstrap_token:
 * @store: (nullable): a #SignetStore
 * @token_hash: (not nullable): token hash
 * @now: current Unix time in seconds
 *
 * Mark a token as consumed (set used_at) by token_hash. Call only after session is fully established. Returns 0 on success, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_consume_bootstrap_token(struct SignetStore *store,
                                         const char *token_hash,
                                         int64_t now);

/* Bind a verified bootstrap token to the connect_secret handed back to the
 * client. This lets the later NIP-46 connect consume the exact token that
 * produced the handoff URI rather than any token for the agent.
 * Returns 0 on success, -1 on error. */
/**
 * signet_store_bind_bootstrap_token_handoff:
 * @store: (nullable): a #SignetStore
 * @token_hash: (not nullable): token hash
 * @connect_secret: (not nullable): connect secret
 *
 * Bind a verified bootstrap token to the connect_secret handed back to the client. This lets the later NIP-46 connect consume the exact token that produced the handoff URI rather than any token for the agent. Returns 0 on success, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_bind_bootstrap_token_handoff(struct SignetStore *store,
                                              const char *token_hash,
                                              const char *connect_secret);

/* Delete expired tokens older than cutoff. Returns count deleted, -1 on error. */
/**
 * signet_store_cleanup_expired_tokens:
 * @store: (nullable): a #SignetStore
 * @cutoff: cutoff
 *
 * Delete expired tokens older than cutoff. Returns count deleted, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_cleanup_expired_tokens(struct SignetStore *store, int64_t cutoff);

/* Free a token record. Safe on NULL. */
/**
 * signet_bootstrap_token_clear:
 * @tok: (nullable): tok
 *
 * Free a token record. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_bootstrap_token_clear(SignetBootstrapToken *tok);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_TOKENS_H */
