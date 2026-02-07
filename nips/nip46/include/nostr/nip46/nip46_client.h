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

/* nostrc-nip46-rpc: Send connect RPC to remote signer.
 * This must be called after parsing bunker:// URI but before other operations.
 * The session must have: remote_pubkey_hex, secret (client key), relays.
 * connect_secret: the secret= value from bunker URI (may be NULL)
 * perms: requested permissions CSV (may be NULL)
 * On success, out_result contains "ack" or the connect secret. Caller must free. */
int nostr_nip46_client_connect_rpc(NostrNip46Session *s,
                                   const char *connect_secret,
                                   const char *perms,
                                   char **out_result);

/* nostrc-nip46-rpc: Send get_public_key RPC to remote signer.
 * Returns the user's actual pubkey (may differ from remote_signer_pubkey).
 * On success, out_user_pubkey_hex contains hex pubkey. Caller must free. */
int nostr_nip46_client_get_public_key_rpc(NostrNip46Session *s, char **out_user_pubkey_hex);

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

/* nostrc-j2yu: Persistent connection API.
 * Start a persistent relay connection for efficient RPC calls.
 * This should be called after nostr_nip46_client_connect() has parsed the bunker URI.
 * Once started, all RPC calls reuse the same relay connections instead of
 * connecting/disconnecting per call.
 * Returns 0 on success, -1 on failure. */
int nostr_nip46_client_start(NostrNip46Session *s);

/* nostrc-j2yu: Stop the persistent relay connection.
 * Safe to call multiple times or if never started. */
void nostr_nip46_client_stop(NostrNip46Session *s);

/* nostrc-j2yu: Check if the persistent client pool is running.
 * Returns 1 if running, 0 if not. */
int nostr_nip46_client_is_running(NostrNip46Session *s);

void nostr_nip46_session_free(NostrNip46Session *s);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_CLIENT_H */
