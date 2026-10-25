/* SPDX-License-Identifier: MIT */
#ifndef VAULT_MARMOT_H
#define VAULT_MARMOT_H
#include <vault-marmot/export.h>
#include <vault/vault.h>
#include <marmot/marmot.h>
#ifdef __cplusplus
extern "C" {
#endif

#define VAULT_MARMOT_EXPORTER_LABEL "porthome/v2/vault"
/* Current canonical epoch only; context is the 32 raw vault_id bytes, not hex.
 * Output is secret key material. Serialize access to the borrowed Marmot instance.
 * No cached epoch, fallback KDF or reuse of MIP-03's NIP-44 transport key. */
VAULT_MARMOT_API MarmotError vault_marmot_seed(Marmot *, const MarmotGroupId *,
                                               const uint8_t vault_id[32], uint8_t seed[32]);

/* Only signed kind-445 ciphertext crosses this seam. publish borrows the event;
 * success means relay OK acceptance, not websocket send. receive transfers an
 * owned event (also on error), or returns VAULT_ERR_AGAIN when none is available.
 * The caller owns subscriptions, EOSE, AUTH/CLOSED/OK, event-id dedup and durable
 * delivery. No relay contact, sleeps, retries or polling occur in this adapter.
 * Commit/proposal/own-message results are processed once and return AGAIN.
 * Commit publication/merge and Welcome delivery remain the group's owner's job.
 */
typedef struct {
    void *ctx;
    int (*publish)(void *, const struct _NostrEvent *group_event);
    int (*receive)(void *, struct _NostrEvent **group_event);
} vault_marmot_transport;
typedef struct vault_marmot_carrier vault_marmot_carrier;

/* Borrow Marmot and transport context; copy group/vault IDs. At least one I/O
 * direction is required. The group must already exist with real MLS state and
 * account-proven sender/recipient leaves (legacy raw/unproven mode is refused).
 * Send requires the grant owner to be our MLS account and recipient a proven
 * current member. All current leaves claiming either account must be proven;
 * the message API identifies an account, not the particular sending leaf.
 * Delivery is GROUP-wide: every member can read the grant, not
 * just recipient. The application must align group membership with vault access.
 * Receive binds the routing group, vault_id, MLS sender, rumor author and owner.
 * Grants from retained epochs are refused: current leaf proofs cannot establish
 * a historical sender's proof. Reissue such grants in the current epoch.
 * Free before Marmot. No persistence of plaintext grants is requested here.
 */
VAULT_MARMOT_API int vault_marmot_carrier_new(Marmot *, const MarmotGroupId *,
                                              const uint8_t vault_id[32],
                                              const vault_marmot_transport *,
                                              vault_marmot_carrier **);
VAULT_MARMOT_API const vault_grant_carrier *
vault_marmot_carrier_interface(vault_marmot_carrier *);
VAULT_MARMOT_API void vault_marmot_carrier_free(vault_marmot_carrier *);

/* Input from an authenticated, recipient-scoped kind-443 lookup on THIS vault's
 * relays. ABSENT means completed catch-up, not a timeout or transport failure.
 * This helper does not perform a lookup, join a group or enable I/O. UNKNOWN
 * returns AGAIN; PRESENT never silently falls back if Marmot is unconfigured.
 * Choosing a carrier does not assert membership or grant authority. */
typedef enum {
    VAULT_MARMOT_KP_UNKNOWN = 0,
    VAULT_MARMOT_KP_ABSENT = 1,
    VAULT_MARMOT_KP_PRESENT = 2
} vault_marmot_key_package_status;
VAULT_MARMOT_API int vault_marmot_prefer(vault_marmot_key_package_status,
                                         const vault_grant_carrier *marmot,
                                         const vault_grant_carrier *nip59,
                                         const vault_grant_carrier **selected);
#ifdef __cplusplus
}
#endif
#endif
