/* SPDX-License-Identifier: MIT
 *
 * key_store_private.h - key_store.c internals shared with nip46_server.c.
 *
 * Not installed. NIP-46 client pairing for a CACHE-ONLY key store (no
 * persistent SignetStore). It mirrors the persistent pairing model
 * (signet_store_consume_connect_secret_and_bind /
 * signet_store_lookup_client_binding) for the lifetime of one process:
 *
 * - provision/adopt keep only the SHA-256 of the agent's pending one-time
 *   connect_secret on its hot-cache entry (never the secret itself);
 * - pairing consumes that secret and records client_pubkey -> agent_id with
 *   the secret's hash in ONE critical section under the key-store mutex;
 * - a binding is pinned to the exact hot-cache entry it was made against, so
 *   revoke, evict, rotate-key, restore or re-adopt/re-provision invalidate it
 *   (no resurrection of prior authority), and those operations drop the
 *   agent's bindings at once;
 * - rotate-key leaves no pending secret (like the store row);
 *   signet_key_store_reissue_connect_secret mints one, as with a store;
 * - nothing survives the process: after a restart a cache-only signer holds
 *   no agents, secrets or bindings, and every client must be provisioned and
 *   paired again.
 */

#ifndef SIGNET_KEY_STORE_PRIVATE_H
#define SIGNET_KEY_STORE_PRIVATE_H

#include "signet/key_store.h"

/* Cache-only pairing: consumes the pending one-time @connect_secret of the
 * agent that holds it and binds @client_pubkey_hex (64 hex, any case) to
 * that agent, replacing any earlier binding of the same client key.
 * Returns 0 and sets *out_agent_id (g_free) on success, 1 when no agent in
 * this process holds that pending secret, -1 on error or when @ks has a
 * persistent store (use signet_store_consume_connect_secret_and_bind). */
int signet_key_store_ephemeral_pair_client(SignetKeyStore *ks,
                                           const char *connect_secret,
                                           const char *client_pubkey_hex,
                                           char **out_agent_id);

/* Cache-only binding lookup. Returns 0 and sets *out_agent_id (g_free) and,
 * when non-NULL, *out_bound_secret_hash (SHA-256 hex of the pairing secret,
 * g_free) while the agent still holds the identity the client paired with;
 * 1 when the client is not paired in this process or its agent was revoked,
 * rotated or replaced since (the stale binding is dropped); -1 on error or
 * when @ks has a persistent store. */
int signet_key_store_ephemeral_lookup_client(SignetKeyStore *ks,
                                             const char *client_pubkey_hex,
                                             char **out_agent_id,
                                             char **out_bound_secret_hash);

/* Number of cache-only client bindings currently held (test seam). */
unsigned int signet_key_store_ephemeral_binding_count(SignetKeyStore *ks);

#endif /* SIGNET_KEY_STORE_PRIVATE_H */
