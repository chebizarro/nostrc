#ifndef NOSTR_NIP46_ENVELOPE_H
#define NOSTR_NIP46_ENVELOPE_H

#include "nostr-event.h"
#include "nostr/nip46/nip46_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build plain (unencrypted) NIP-46 request/response events. These
 * compatibility helpers do not apply transport crypto. Prefer the encrypted
 * builders below for kind-24133 network traffic. */
int nostr_nip46_build_request_event(const char *sender_pubkey_hex,
                                    const char *receiver_pubkey_hex,
                                    const char *request_json,
                                    NostrEvent **out_event);

int nostr_nip46_build_response_event(const char *sender_pubkey_hex,
                                     const char *receiver_pubkey_hex,
                                     const char *response_json,
                                     NostrEvent **out_event);

/* Encrypt content with the session-owned negotiated mode and build a kind
 * 24133 event. The response builder uses exactly the same mode as requests. */
int nostr_nip46_build_encrypted_request_event(
    NostrNip46Session *session,
    const char *sender_pubkey_hex,
    const char *receiver_pubkey_hex,
    const char *request_json,
    NostrEvent **out_event);
int nostr_nip46_build_encrypted_response_event(
    NostrNip46Session *session,
    const char *sender_pubkey_hex,
    const char *receiver_pubkey_hex,
    const char *response_json,
    NostrEvent **out_event);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_ENVELOPE_H */
