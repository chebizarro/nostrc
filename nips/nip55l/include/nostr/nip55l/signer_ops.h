#ifndef NOSTR_NIP55L_SIGNER_OPS_H
#define NOSTR_NIP55L_SIGNER_OPS_H

#ifdef __cplusplus
extern "C" {
#endif

int nostr_nip55l_get_public_key(char **out_npub);
int nostr_nip55l_sign_event(const char *event_json,
                            const char *current_user,
                            const char *app_id,
                            char **out_signature);
int nostr_nip55l_nip04_encrypt(const char *plaintext, const char *peer_pub_hex,
                               const char *current_user, char **out_cipher_b64);
int nostr_nip55l_nip04_decrypt(const char *cipher_b64, const char *peer_pub_hex,
                               const char *current_user, char **out_plaintext);
int nostr_nip55l_nip44_encrypt(const char *plaintext, const char *peer_pub_hex,
                               const char *current_user, char **out_cipher_b64);
int nostr_nip55l_nip44_decrypt(const char *cipher_b64, const char *peer_pub_hex,
                               const char *current_user, char **out_plaintext);
int nostr_nip55l_decrypt_zap_event(const char *event_json,
                                   const char *current_user, char **out_json);
int nostr_nip55l_get_relays(char **out_relays_json);

/* Optional key storage using libsecret when available. */
int nostr_nip55l_store_secret(const char *secret, const char *account);
int nostr_nip55l_clear_secret(const char *account);

#ifdef __cplusplus
}
#endif

#endif
