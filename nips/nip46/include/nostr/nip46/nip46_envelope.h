#ifndef NOSTR_NIP46_ENVELOPE_H
#define NOSTR_NIP46_ENVELOPE_H

#include "nostr-event.h"
#include "nostr/nip46/nip46_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build plain (unencrypted) NIP-46 request/response events per spec.
 * Encryption (NIP-44) will be layered later. */
int nostr_nip46_build_request_event(const char *sender_pubkey_hex,
                                    const char *receiver_pubkey_hex,
                                    const char *request_json,
                                    NostrEvent **out_event);

int nostr_nip46_build_response_event(const char *sender_pubkey_hex,
                                     const char *receiver_pubkey_hex,
                                     const char *response_json,
                                     NostrEvent **out_event);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_ENVELOPE_H */
