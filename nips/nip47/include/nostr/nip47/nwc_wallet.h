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

#ifdef __cplusplus
}
#endif
