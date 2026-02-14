#ifndef NIPS_NIP5F_NOSTR_NIP5F_NIP5F_H
#define NIPS_NIP5F_NOSTR_NIP5F_NIP5F_H

/* See SPEC source: nips/nip5f/SPEC_SOURCE -> ../../docs/proposals/5F.md */

#ifdef __cplusplus
extern "C" {
#endif

/* Error domain for NIP-5F: errors are returned as int codes per SPEC. */

/* Server handlers */
typedef int (*Nip5fGetPubFn)(void *ud, char **out_pub_hex);
typedef int (*Nip5fSignEventFn)(void *ud, const char *event_json, const char *pubkey_hex, char **out_signed_event_json);
typedef int (*Nip5fNip44EncFn)(void *ud, const char *peer_pub_hex, const char *plaintext, char **out_cipher_b64);
typedef int (*Nip5fNip44DecFn)(void *ud, const char *peer_pub_hex, const char *cipher_b64, char **out_plaintext);
typedef int (*Nip5fListKeysFn)(void *ud, char **out_keys_json);

/* Server control */
int nostr_nip5f_server_start(const char *socket_path, void **out_handle);
int nostr_nip5f_server_stop(void *handle);
int nostr_nip5f_server_set_handlers(void *handle,
  Nip5fGetPubFn get_pub, Nip5fSignEventFn sign_event,
  Nip5fNip44EncFn enc44, Nip5fNip44DecFn dec44,
  Nip5fListKeysFn list_keys, void *user_data);

/* Built-in default handlers using libnostr primitives */
int nostr_nip5f_builtin_get_public_key(char **out_pub_hex);
int nostr_nip5f_builtin_sign_event(const char *event_json, const char *pubkey_hex, char **out_signed_event_json);
int nostr_nip5f_builtin_nip44_encrypt(const char *peer_pub_hex, const char *plaintext, char **out_cipher_b64);
int nostr_nip5f_builtin_nip44_decrypt(const char *peer_pub_hex, const char *cipher_b64, char **out_plaintext);
int nostr_nip5f_builtin_list_public_keys(char **out_keys_json);

/* Client helpers */
int nostr_nip5f_client_connect(const char *socket_path, void **out_conn);
int nostr_nip5f_client_close(void *conn);
int nostr_nip5f_client_get_public_key(void *conn, char **out_pub_hex);
int nostr_nip5f_client_sign_event(void *conn, const char *event_json, const char *pubkey_hex, char **out_signed_event_json);
int nostr_nip5f_client_nip44_encrypt(void *conn, const char *peer_pub_hex, const char *plaintext, char **out_cipher_b64);
int nostr_nip5f_client_nip44_decrypt(void *conn, const char *peer_pub_hex, const char *cipher_b64, char **out_plaintext);
int nostr_nip5f_client_list_public_keys(void *conn, char **out_keys_json);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP5F_NOSTR_NIP5F_NIP5F_H */
