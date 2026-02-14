#ifndef NIPS_NIP47_NOSTR_NIP47_NWC_ENVELOPE_H
#define NIPS_NIP47_NOSTR_NIP47_NWC_ENVELOPE_H

#include "nwc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { char *method; char *params_json; } NostrNwcRequestBody;

typedef struct { char *result_type; char *result_json; char *error_code; char *error_message; } NostrNwcResponseBody;

int nostr_nwc_request_build(const char *wallet_pub_hex, NostrNwcEncryption enc,
                            const NostrNwcRequestBody *body,
                            char **out_event_json);

int nostr_nwc_response_build(const char *client_pub_hex, const char *req_event_id,
                             NostrNwcEncryption enc, const NostrNwcResponseBody *body,
                             char **out_event_json);

/* Parse request event JSON. Allocates strings in out structures; caller must free fields. */
int nostr_nwc_request_parse(const char *event_json,
                            char **out_wallet_pub_hex,
                            NostrNwcEncryption *out_enc,
                            NostrNwcRequestBody *out_body);

/* Parse response event JSON. Allocates strings in out structures; caller must free fields. */
int nostr_nwc_response_parse(const char *event_json,
                             char **out_client_pub_hex,
                             char **out_req_event_id,
                             NostrNwcEncryption *out_enc,
                             NostrNwcResponseBody *out_body);

/* Encryption negotiation: prefer nip44-v2 if available, else nip04. Returns 0 on success. */
int nostr_nwc_select_encryption(const char **client_supported, size_t client_n,
                                const char **wallet_supported, size_t wallet_n,
                                NostrNwcEncryption *out_enc);

/* Clear helpers: free all allocated fields and zero the struct. */
void nostr_nwc_request_body_clear(NostrNwcRequestBody *b);
void nostr_nwc_response_body_clear(NostrNwcResponseBody *b);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP47_NOSTR_NIP47_NWC_ENVELOPE_H */
