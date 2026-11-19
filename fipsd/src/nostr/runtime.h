#ifndef FIPSD_NOSTR_RUNTIME_H
#define FIPSD_NOSTR_RUNTIME_H
#include "nostr/driver.h"
#include "nostr/failure_state.h"
#include "runtime.h"
#include <nostr/nip11/nip11.h>
typedef struct fipsd_nostr_runtime fipsd_nostr_runtime;
/* Identity is borrowed through join. If NULL, resolved from node at start
 * (after node subsystem startup). All config strings/lists are copied. */
fipsd_nostr_runtime *fipsd_nostr_runtime_create(const fipsd_config *, const fips_identity *,
    const fipsd_node_context *, fipsd_rendezvous_driver);
fipsd_subsystem *fipsd_nostr_runtime_subsystem(fipsd_nostr_runtime *);
void fipsd_nostr_runtime_destroy(fipsd_nostr_runtime *); /* after runtime join */
/* Nonblocking owned-command entrypoints; false means refused/full. */
bool fipsd_nostr_runtime_track_peer(fipsd_nostr_runtime *, const char *pubkey_hex);
bool fipsd_nostr_runtime_refetch(fipsd_nostr_runtime *, const char *pubkey_hex);
bool fipsd_nostr_runtime_send_signal(fipsd_nostr_runtime *, const char *pubkey_hex, const fipsd_nostr_traversal *);
bool fipsd_nostr_runtime_refresh_advert(fipsd_nostr_runtime *);
bool fipsd_nostr_runtime_resend_signal(fipsd_nostr_runtime *, const char *, const fipsd_nostr_traversal *);
bool fipsd_nostr_runtime_session_finished(fipsd_nostr_runtime *, const char *session_id);
bool fipsd_nostr_runtime_peer_result(fipsd_nostr_runtime *, const char *pubkey_hex, bool success, bool mismatch);
/* Pure B6 decision: failed/incomplete fetch, foreign/invalid result, cache full
 * => Skipped; successful empty => Evicted; <= cached => SameAdvert. */
fipsd_nostr_refetch_outcome fipsd_nostr_refetch_decide(bool complete, bool any,
    bool matching_author, bool valid, bool cache_room, bool cached,
    uint64_t cached_created, uint64_t relay_created);
/* NIP-11 refusal is based on affirmative incompatibility: missing required
 * NIP-01/40 (and NIP-42 when authentication is required), payment/PoW, insufficient filter budget.
 * No document is unknown, not compatible: this returns false, and the actor
 * re-probes a relay whose fetch failed on its backoff ladder (never admitting
 * it unverified) while a fetched incompatible document refuses it for the run. */
bool fipsd_nostr_relay_capable(const RelayInformationDocument *, bool signals);
#endif
