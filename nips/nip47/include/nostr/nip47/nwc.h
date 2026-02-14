#ifndef NIPS_NIP47_NOSTR_NIP47_NWC_H
#define NIPS_NIP47_NOSTR_NIP47_NWC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOSTR_EVENT_KIND_NWC_INFO        13194
#define NOSTR_EVENT_KIND_NWC_REQUEST     23194
#define NOSTR_EVENT_KIND_NWC_RESPONSE    23195
#define NOSTR_EVENT_KIND_NWC_NOTIFY_44   23197
#define NOSTR_EVENT_KIND_NWC_NOTIFY_04   23196

typedef enum { NOSTR_NWC_ENC_NIP44_V2, NOSTR_NWC_ENC_NIP04 } NostrNwcEncryption;

typedef enum {
  NOSTR_NWC_PAY_INVOICE,
  NOSTR_NWC_MULTI_PAY_INVOICE,
  NOSTR_NWC_PAY_KEYSEND,
  NOSTR_NWC_MULTI_PAY_KEYSEND,
  NOSTR_NWC_MAKE_INVOICE,
  NOSTR_NWC_LOOKUP_INVOICE,
  NOSTR_NWC_LIST_TRANSACTIONS,
  NOSTR_NWC_GET_BALANCE,
  NOSTR_NWC_GET_INFO
} NostrNwcMethod;

typedef enum { NOSTR_NWC_NOTIFY_PAYMENT_RECEIVED, NOSTR_NWC_NOTIFY_PAYMENT_SENT } NostrNwcNotificationType;

typedef struct {
  char   *wallet_pubkey_hex;  /* base path pubkey (hex) */
  char  **relays;             /* null-terminated list */
  char   *secret_hex;         /* 32-byte hex (client SK) */
  char   *lud16;              /* optional */
} NostrNwcConnection;

int nostr_nwc_uri_parse(const char *uri, NostrNwcConnection *out);
int nostr_nwc_uri_build(const NostrNwcConnection *in, char **out_uri);
void nostr_nwc_connection_clear(NostrNwcConnection *c);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP47_NOSTR_NIP47_NWC_H */
