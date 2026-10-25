/* SPDX-License-Identifier: MIT */
#ifndef VAULT_NIP59_H
#define VAULT_NIP59_H
#include <vault/vault.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Transport sees only signed kind-1059 wraps, never a rumor or kind-13 seal.
 * publish borrows its event until return; success means relay OK acceptance,
 * not merely websocket send. receive returns an owned event (also on error),
 * or VAULT_ERR_AGAIN when no event is available. Callbacks own subscriptions,
 * AUTH/OK/CLOSED/EOSE, persistence and event-id dedup; no I/O retries here.
 * Callbacks/context are borrowed for the lifetime of the adapter. */
typedef struct {
    void *ctx;
    int (*publish)(void *, const struct _NostrEvent *gift_wrap);
    int (*receive)(void *, struct _NostrEvent **gift_wrap);
} vault_nip59_transport;
typedef struct vault_nip59_carrier vault_nip59_carrier;

/* Copies an identity secret key; free securely disposes the adapter's copy.
 * At least one transport callback is required; only configured directions may
 * be used. Send requires the grant owner/rumor author to be this identity.
 * Receive authenticates the signed seal author (NOT the outer ephemeral key),
 * requires it to equal rumor author/owner, and checks the unique recipient p tag.
 * Nested NIP-44 envelopes must each fit its 65535-byte plaintext bound; grants
 * near the raw codec limit therefore cannot be carried by this fallback.
 * The fallback has no forward secrecy against later identity-key compromise.
 * Setup/use is explicit; this adapter does not select a carrier or enable I/O.
 */
VAULT_API int vault_nip59_carrier_new(const uint8_t identity_sk[32], const vault_nip59_transport *,
                                      vault_nip59_carrier **);
VAULT_API const vault_grant_carrier *vault_nip59_carrier_interface(vault_nip59_carrier *);
VAULT_API void vault_nip59_carrier_free(vault_nip59_carrier *);
#ifdef __cplusplus
}
#endif
#endif
