/* SPDX-License-Identifier: MIT
 *
 * nip46_server.h - NIP-46 bunker request handling for Signet.
 *
 * This module is responsible for:
 * - validating and replay-protecting incoming NIP-46 events
 * - decrypting/parsing NIP-46 requests
 * - policy evaluation (per client/method/kind)
 * - loading custody keys (hot cache) and producing signatures
 * - emitting encrypted NIP-46 responses to relays
 *
 * Full implementation: replay protection, NIP-44 v2 encryption,
 * policy gating, event signing, and NIP-46 message handling.
 */

#ifndef SIGNET_NIP46_SERVER_H
#define SIGNET_NIP46_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#include <nostr/nip46/nip46_types.h>

struct SignetRelayPool;
struct SignetPolicyEngine;
struct SignetKeyStore;
struct SignetReplayCache;
struct SignetAuditLogger;
struct SignetFidoService;

/**
 * SignetNip46Server:
 * Opaque NIP-46 bunker request handler.
 *
 * Since: 1.0
 */
typedef struct SignetNip46Server SignetNip46Server;

/**
 * SignetNip46ServerConfig:
 * @identity: borrowed.
 * @fido: borrowed; may be NULL/disabled.
 *
 * Configuration for optional NIP-46 handler integrations.
 *
 * Since: 1.0
 */
typedef struct {
  const char *identity; /* borrowed */
  struct SignetFidoService *fido; /* borrowed; may be NULL/disabled */
} SignetNip46ServerConfig;

/* Create NIP-46 server. Returns NULL on OOM. */
/**
 * signet_nip46_server_new:
 * @relays: (not nullable): relays
 * @policy: (not nullable): policy
 * @keys: (not nullable): keys
 * @replay: (not nullable): replay
 * @audit: (not nullable): audit
 * @cfg: (nullable): configuration to use
 *
 * Create NIP-46 server. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetNip46Server *signet_nip46_server_new(struct SignetRelayPool *relays,
                                           struct SignetPolicyEngine *policy,
                                           struct SignetKeyStore *keys,
                                           struct SignetReplayCache *replay,
                                           struct SignetAuditLogger *audit,
                                           const SignetNip46ServerConfig *cfg);

/* Attach the daemon's LIVE deny list so suspended (deny-listed) agent
 * identities are refused on every path — pairing, binding reconnect, and
 * per-request resolution — before policy evaluation. Pass the SAME instance
 * consulted by the auth/fleet is_denied callback. Safe on NULL h or deny. */
/**
 * signet_nip46_server_set_deny_list:
 * @s: (not nullable): a #SignetNip46Server
 * @deny: (nullable): live deny list
 *
 * Attach the daemon deny list; deny-listed agent identities are refused before policy evaluation.
 *
 * Since: 1.2
 */
struct SignetDenyList;
void signet_nip46_server_set_deny_list(SignetNip46Server *s,
                                       struct SignetDenyList *deny);

/* Free server. Safe on NULL. */
/**
 * signet_nip46_server_free:
 * @s: (nullable): a #SignetNip46Server
 *
 * Free server. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_nip46_server_free(SignetNip46Server *s);

/* Handle an incoming relay event view. Returns true if the event was recognized/handled. */
/**
 * signet_nip46_server_handle_event:
 * @s: (not nullable): a #SignetNip46Server
 * @remote_signer_pubkey_hex: (not nullable): remote signer pubkey hex
 * @remote_signer_secret_key_hex: (not nullable): remote signer secret key hex
 * @client_pubkey_hex: (not nullable): client public key in hexadecimal form
 * @ciphertext: (not nullable): ciphertext
 * @created_at: created at
 * @event_id_hex: (nullable): event id hex
 * @now: current Unix time in seconds
 *
 * Handle an incoming relay event view. Returns true if the event was recognized/handled.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_nip46_server_handle_event(SignetNip46Server *s,
                                      const char *remote_signer_pubkey_hex,
                                      const char *remote_signer_secret_key_hex,
                                      const char *client_pubkey_hex,
                                      const char *ciphertext,
                                      int64_t created_at,
                                      const char *event_id_hex,
                                      int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_NIP46_SERVER_H */
