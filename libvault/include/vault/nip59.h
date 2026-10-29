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
/* Experimental NIP-4E, opt-in because NIPs PR #1647 is unmerged.
 * Implements kind 10044's unique ["n", <encryption pubkey>] announcement from
 * revision b22711d050df662bfba5f813a35d801daf067dc8 (4e.md). This is the
 * user-level encryption key, NOT the per-client kind-4454/4455 exchange.
 * query issues the supplied author/kind/limit filter on ONE configured relay,
 * synchronously calls emit for each borrowed event, and sets *eose=1 ONLY on
 * that subscription's EOSE. Stop on emit error. Do not emit after EOSE.
 * Unreachable/disconnected/refusing relays (CLOSED), unhandled AUTH, cancellation
 * or deadline expiry return an error, never an empty successful result. Query
 * owns connections, NIP-11, AUTH/re-REQ and CLOSE cleanup; do not poll or sleep.
 * All callbacks, relay strings and context are borrowed until disabled/free;
 * emit and its context are valid only during query. No concurrent carrier use.
 */
struct NostrFilter;
typedef int (*vault_nip4e_emit_fn)(void *, const struct _NostrEvent *);
typedef struct {
    void *ctx;
    const char *const *relays;
    size_t relays_len;
    int (*query)(void *, const char *relay, const struct NostrFilter *,
                 vault_nip4e_emit_fn emit, void *emit_ctx, int *eose);
} vault_nip4e_resolver;
/* NULL disables resolution (the default). Otherwise every relay must reach
 * EOSE before any grant is sent. Query errors propagate; missing EOSE returns
 * VAULT_ERR_INCOMPLETE. Never silently downgrade an incomplete lookup.
 * Libvault verifies author, id and signature, selects the latest replaceable
 * event (lowest id breaks timestamp ties), then validates its unique n key.
 * Invalid announcements return VAULT_ERR_AUTH; no older-key fallback.
 * Only an EOSE-complete empty result uses the identity key. Both NIP-44 layers
 * use the selected key. The signed wrapper's standard ["p", <selected key>]
 * records/routes that encryption recipient per NIP-59; it does not expose the
 * identity-to-device mapping. The kind-30804 rumor body is unchanged.
 * Invalid setter arguments leave the previous configuration unchanged. */
VAULT_API int vault_nip59_carrier_set_nip4e(vault_nip59_carrier *, const vault_nip4e_resolver *);
/* Copies a local encryption secret key; NULL clears it. Receive tries it before
 * the identity key, independently of outbound resolution. Both failures have
 * the same VAULT_ERR_AUTH result, with no key-match status or diagnostic.
 * Keys are wiped on replacement/free. This does not manage/distribute keys or
 * subscribe to either inbox: receive transport must supply wraps for both p
 * keys. No forward secrecy against compromise of the selected delivery key. */
VAULT_API int vault_nip59_carrier_set_device_key(vault_nip59_carrier *, const uint8_t device_sk[32]);
VAULT_API const vault_grant_carrier *vault_nip59_carrier_interface(vault_nip59_carrier *);
VAULT_API void vault_nip59_carrier_free(vault_nip59_carrier *);
#ifdef __cplusplus
}
#endif
#endif
