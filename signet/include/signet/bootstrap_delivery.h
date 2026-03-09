/* SPDX-License-Identifier: MIT
 *
 * bootstrap_delivery.h - Fleet Commander bootstrap delivery via NIP-17.
 *
 * Flow:
 * 1. Fleet Commander calls signet_bootstrap_send() to gift-wrap the
 *    bootstrap token as a NIP-17 kind 14 DM to the agent's throwaway
 *    bootstrap_pubkey.
 * 2. Agent side calls signet_bootstrap_receive() to subscribe to relay,
 *    decrypt with bootstrap_nsec, extract token, call /bootstrap.
 * 3. Agent sends ACK kind 14 back to Fleet Commander.
 * 4. If token expires before agent receives it, Fleet Commander reissues.
 *
 * Uses libnostr NIP-17: nostr_nip17_wrap_dm() / nostr_nip17_decrypt_dm()
 */

#ifndef SIGNET_BOOTSTRAP_DELIVERY_H
#define SIGNET_BOOTSTRAP_DELIVERY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct SignetRelayPool;
struct SignetStore;

/* Bootstrap delivery message content (JSON inside the DM). */
typedef struct {
  char *token;             /* raw bootstrap token */
  char *agent_id;          /* target agent_id */
  char *bootstrap_url;     /* URL of /bootstrap endpoint */
  char *const *relay_urls; /* relay URLs for bunker connection */
  size_t n_relay_urls;
  int64_t expires_at;      /* token expiry */
} SignetBootstrapMessage;

/* Result of receiving a bootstrap delivery. */
typedef struct {
  char *token;
  char *agent_id;
  char *bootstrap_url;
  char **relay_urls;
  size_t n_relay_urls;
  int64_t expires_at;
  char *sender_pubkey;     /* Fleet Commander pubkey */
} SignetBootstrapReceived;

/* Send a bootstrap token to an agent via NIP-17 gift-wrapped DM.
 * fleet_sk_hex: Fleet Commander's secret key (signs the DM).
 * bootstrap_pubkey_hex: agent's throwaway bootstrap pubkey (recipient).
 * msg: bootstrap message content to deliver.
 * relay_pool: relay pool to publish the gift wrap event.
 * Returns 0 on success, -1 on error. */
int signet_bootstrap_send(const char *fleet_sk_hex,
                            const char *bootstrap_pubkey_hex,
                            const SignetBootstrapMessage *msg,
                            struct SignetRelayPool *relay_pool);

/* Receive and decrypt a bootstrap delivery DM.
 * gift_wrap_json: the kind 1059 event JSON received from relay.
 * bootstrap_sk_hex: agent's throwaway bootstrap secret key.
 * out: populated on success (caller frees with signet_bootstrap_received_clear).
 * Returns 0 on success, -1 on error. */
int signet_bootstrap_receive(const char *gift_wrap_json,
                               const char *bootstrap_sk_hex,
                               SignetBootstrapReceived *out);

/* Send an ACK DM back to the Fleet Commander after successful bootstrap.
 * agent_sk_hex: agent's (newly provisioned) secret key.
 * fleet_pubkey_hex: Fleet Commander's pubkey.
 * agent_id: agent identifier.
 * relay_pool: relay pool to publish.
 * Returns 0 on success, -1 on error. */
int signet_bootstrap_send_ack(const char *agent_sk_hex,
                                const char *fleet_pubkey_hex,
                                const char *agent_id,
                                struct SignetRelayPool *relay_pool);

/* Check if a bootstrap token should be reissued (expired but agent hasn't
 * bootstrapped yet). Returns true if reissue is needed. */
bool signet_bootstrap_needs_reissue(struct SignetStore *store,
                                      const char *agent_id,
                                      int64_t now);

/* Free a received bootstrap message. Safe on NULL. */
void signet_bootstrap_received_clear(SignetBootstrapReceived *recv);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_BOOTSTRAP_DELIVERY_H */
