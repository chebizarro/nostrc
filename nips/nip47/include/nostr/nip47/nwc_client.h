#ifndef NIPS_NIP47_NOSTR_NIP47_NWC_CLIENT_H
#define NIPS_NIP47_NOSTR_NIP47_NWC_CLIENT_H

#include "nwc.h"
#include "nwc_envelope.h"

typedef struct _NostrSimplePool NostrSimplePool;

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*NostrNwcResponseCallback)(const char *event_json,
                                         const char *request_event_id,
                                         const NostrNwcResponseBody *body,
                                         void *user_data);

typedef struct {
  char *wallet_pub_hex;
  NostrNwcEncryption enc;

  /* Response subscription state (kind 23195, #p:<client_pub_hex>). */
  char *client_pub_hex;
  NostrSimplePool *response_pool;
  NostrNwcResponseCallback response_cb;
  void *response_cb_data;
} NostrNwcClientSession;

int nostr_nwc_client_session_init(NostrNwcClientSession *s,
                                  const char *wallet_pub_hex,
                                  const char **client_supported, size_t client_n,
                                  const char **wallet_supported, size_t wallet_n);

void nostr_nwc_client_session_clear(NostrNwcClientSession *s);

int nostr_nwc_client_build_request(const NostrNwcClientSession *s,
                                   const NostrNwcRequestBody *body,
                                   char **out_event_json);

/* Builds an encrypted, signed kind-23194 request event. Requires client_sk_hex
 * to be the client's 32-byte hex secret key. */
int nostr_nwc_client_build_signed_request(const NostrNwcClientSession *s,
                                          const char *client_sk_hex,
                                          const NostrNwcRequestBody *body,
                                          char **out_event_json);

/* Start/stop the NIP-47 response subscription. The subscription filter is:
 * kinds:[23195], #p:[client_pub_hex], since:[time at subscription start].
 * EOSE is intentionally not treated as completion; the subscription remains
 * live so wallet responses arriving after initial catch-up still route to cb. */
int nostr_nwc_client_start_response_subscription(NostrNwcClientSession *s,
                                                 const char **relays,
                                                 size_t n_relays,
                                                 const char *client_pub_hex,
                                                 NostrNwcResponseCallback cb,
                                                 void *user_data);
void nostr_nwc_client_stop_response_subscription(NostrNwcClientSession *s);

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
#endif /* NIPS_NIP47_NOSTR_NIP47_NWC_CLIENT_H */
