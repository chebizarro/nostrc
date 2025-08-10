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

#ifdef __cplusplus
}
#endif
