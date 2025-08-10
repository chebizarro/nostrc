#pragma once

#include "nwc.h"
#include "nwc_envelope.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *client_pub_hex;
  NostrNwcEncryption enc;
} NostrNwcWalletSession;

int nostr_nwc_wallet_session_init(NostrNwcWalletSession *s,
                                  const char *client_pub_hex,
                                  const char **wallet_supported, size_t wallet_n,
                                  const char **client_supported, size_t client_n);

void nostr_nwc_wallet_session_clear(NostrNwcWalletSession *s);

int nostr_nwc_wallet_build_response(const NostrNwcWalletSession *s,
                                    const char *req_event_id,
                                    const NostrNwcResponseBody *body,
                                    char **out_event_json);

/* Encrypt/decrypt helpers (canonical). Use s->enc to choose NIP-44 v2 or NIP-04.
 * wallet_sk_hex: 32-byte hex secret of wallet.
 * client_pub_hex: client pubkey (accepts x-only 64, SEC1 33/65 hex; auto-converts to x-only for NIP-44).
 */
int nostr_nwc_wallet_encrypt(const NostrNwcWalletSession *s,
                             const char *wallet_sk_hex,
                             const char *client_pub_hex,
                             const char *plaintext,
                             char **out_ciphertext);

int nostr_nwc_wallet_decrypt(const NostrNwcWalletSession *s,
                             const char *wallet_sk_hex,
                             const char *client_pub_hex,
                             const char *ciphertext,
                             char **out_plaintext);

#ifdef __cplusplus
}
#endif
