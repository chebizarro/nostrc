#pragma once

#include "nwc.h"
#include "nwc_envelope.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *wallet_pub_hex;
  NostrNwcEncryption enc;
} NostrNwcClientSession;

int nostr_nwc_client_session_init(NostrNwcClientSession *s,
                                  const char *wallet_pub_hex,
                                  const char **client_supported, size_t client_n,
                                  const char **wallet_supported, size_t wallet_n);

void nostr_nwc_client_session_clear(NostrNwcClientSession *s);

int nostr_nwc_client_build_request(const NostrNwcClientSession *s,
                                   const NostrNwcRequestBody *body,
                                   char **out_event_json);

/* Encrypt/decrypt helpers (canonical, no wrappers). Use s->enc to choose NIP-44 v2 or NIP-04.
 * client_sk_hex: 32-byte hex secret of client.
 * wallet_pub_hex: wallet public key (accepts x-only 64, SEC1 33/65 hex; auto-converts to x-only for NIP-44).
 */
int nostr_nwc_client_encrypt(const NostrNwcClientSession *s,
                             const char *client_sk_hex,
                             const char *wallet_pub_hex,
                             const char *plaintext,
                             char **out_ciphertext);

int nostr_nwc_client_decrypt(const NostrNwcClientSession *s,
                             const char *client_sk_hex,
                             const char *wallet_pub_hex,
                             const char *ciphertext,
                             char **out_plaintext);

#ifdef __cplusplus
}
#endif
