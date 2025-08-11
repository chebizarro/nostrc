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

/* Optional private key storage using libsecret when available. */
int nostr_nip55l_store_key(const char *key, const char *identity);
int nostr_nip55l_clear_key(const char *identity);

/* Optional Unix owner metadata (no enforcement). Selector is key_id or npub. */
/* Returns 0 on success, NOT_FOUND if libsecret unavailable or item missing. */
#include <sys/types.h>
int nostr_nip55l_set_owner(const char *selector, uid_t uid, const char *username);
int nostr_nip55l_clear_owner(const char *selector);
/* Outputs: has_owner=1 if present; if present, uid_out and username_out (caller frees username_out). */
int nostr_nip55l_get_owner(const char *selector, int *has_owner, uid_t *uid_out, char **username_out);

#ifdef __cplusplus
}
#endif

#endif
