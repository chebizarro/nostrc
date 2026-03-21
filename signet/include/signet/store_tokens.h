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
SignetTokenResult signet_store_verify_bootstrap_token(struct SignetStore *store,
                                                      const char *token_hash,
                                                      const char *agent_id,
                                                      const char *bootstrap_pubkey,
                                                      int64_t now);

/* Mark a token as consumed (set used_at) by token_hash.
 * Call only after session is fully established.
 * Returns 0 on success, -1 on error. */
int signet_store_consume_bootstrap_token(struct SignetStore *store,
                                         const char *token_hash,
                                         int64_t now);

/* Bind a verified bootstrap token to the connect_secret handed back to the
 * client. This lets the later NIP-46 connect consume the exact token that
 * produced the handoff URI rather than any token for the agent.
 * Returns 0 on success, -1 on error. */
int signet_store_bind_bootstrap_token_handoff(struct SignetStore *store,
                                              const char *token_hash,
                                              const char *connect_secret);

/* Delete expired tokens older than cutoff. Returns count deleted, -1 on error. */
int signet_store_cleanup_expired_tokens(struct SignetStore *store, int64_t cutoff);

/* Free a token record. Safe on NULL. */
void signet_bootstrap_token_clear(SignetBootstrapToken *tok);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_TOKENS_H */
