#ifndef NOSTR_NIP46_CLIENT_H
#define NOSTR_NIP46_CLIENT_H

#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_uri.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Client session (no GLib) */
NostrNip46Session *nostr_nip46_client_new(void);

/* bunker_uri may be bunker:// or nostrconnect:// per spec. requested_perms_csv optional. */
int nostr_nip46_client_connect(NostrNip46Session *s,
                               const char *bunker_uri,
                               const char *requested_perms_csv);

int nostr_nip46_client_get_public_key(NostrNip46Session *s, char **out_user_pubkey_hex);

/* Set the remote signer's pubkey (received after connect handshake) */
int nostr_nip46_client_set_signer_pubkey(NostrNip46Session *s, const char *signer_pubkey_hex);

/* nostrc-1wfi: Set the client's secret key directly (for ECDH encryption).
 * This is the client's secp256k1 private key, NOT the URI's secret param.
 * The secret_hex must be a 64-character hex string (32 bytes). */
int nostr_nip46_client_set_secret(NostrNip46Session *s, const char *secret_hex);

int nostr_nip46_client_sign_event(NostrNip46Session *s, const char *event_json, char **out_signed_event_json);
int nostr_nip46_client_ping(NostrNip46Session *s);

int nostr_nip46_client_nip04_encrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext);
int nostr_nip46_client_nip04_decrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext);

int nostr_nip46_client_nip44_encrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext);
int nostr_nip46_client_nip44_decrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext);

void nostr_nip46_session_free(NostrNip46Session *s);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_CLIENT_H */
