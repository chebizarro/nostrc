#ifndef NOSTR_NIP55L_SIGNER_OPS_H
#define NOSTR_NIP55L_SIGNER_OPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nip55l component version (see VERSION_MANIFEST.md). 0.2.0: the
 * org.nostr.Signer.SignEvent D-Bus method returns the complete signed event
 * JSON (nostr_nip55l_sign_event_json) instead of the bare signature. */
#define NOSTR_NIP55L_VERSION_MAJOR 0
#define NOSTR_NIP55L_VERSION_MINOR 2
#define NOSTR_NIP55L_VERSION_PATCH 0
#define NOSTR_NIP55L_VERSION_STRING "0.2.0"

int nostr_nip55l_get_public_key(char **out_npub);
/* Returns only the 128-hex Schnorr signature. In-process helper; the D-Bus
 * SignEvent method no longer returns this shape (see sign_event_json). */
int nostr_nip55l_sign_event(const char *event_json,
                            const char *current_user,
                            const char *app_id,
                            char **out_signature);
/* Sign an event and return the event ID and pubkey alongside the signature.
 * All three outputs use the same internal state, guaranteeing consistency
 * (e.g. when created_at is auto-filled from 0 to now).
 * Any output pointer may be NULL if not needed. Caller frees non-NULL results. */
int nostr_nip55l_sign_event_full(const char *event_json,
                                 const char *current_user,
                                 const char *app_id,
                                 char **out_event_id,
                                 char **out_pubkey_hex,
                                 char **out_signature);
/* Sign an event and return the complete signed event JSON.
 * The returned JSON includes id, pubkey, created_at, kind, tags, content, sig;
 * pubkey is always the signing key's (any caller-supplied pubkey is replaced)
 * and a zero created_at is filled with the current time.
 * This is what org.nostr.Signer.SignEvent returns over D-Bus.
 * Caller frees the result with free(). */
int nostr_nip55l_sign_event_json(const char *event_json,
                                 const char *current_user,
                                 const char *app_id,
                                 char **out_signed_event_json);
/* Directly Schnorr-sign a raw 32-byte hash (given as 64-char hex).
 * Used for NIP-26 delegation signatures and other cases where the caller
 * has already computed the message hash and needs a raw Schnorr signature
 * without the event serialization/hashing that sign_event performs.
 * @hash_hex: 64-character hex string representing the 32-byte hash to sign
 * @current_user: identity selector (npub, key_id, nsec, hex, or NULL)
 * @out_signature: receives newly allocated 128-char hex Schnorr signature
 * Caller frees *out_signature with free(). */
int nostr_nip55l_sign_hash(const char *hash_hex,
                           const char *current_user,
                           char **out_signature);
int nostr_nip55l_nip04_encrypt(const char *plaintext, const char *peer_pub_hex,
                               const char *current_user, char **out_cipher_b64);
int nostr_nip55l_nip04_decrypt(const char *cipher_b64, const char *peer_pub_hex,
                               const char *current_user, char **out_plaintext);
int nostr_nip55l_nip44_encrypt(const char *plaintext, const char *peer_pub_hex,
                               const char *current_user, char **out_cipher_b64);
int nostr_nip55l_nip44_decrypt(const char *cipher_b64, const char *peer_pub_hex,
                               const char *current_user, char **out_plaintext);
/* Binary-safe NIP-44: the plaintext side travels as standard base64.
 *
 * D-Bus strings (like NIP-46 params) must be valid UTF-8, so a plaintext of
 * raw bytes cannot ride the pair above — it is rejected or silently mangled
 * to U+FFFD, and the signer then encrypts corrupted bytes. Protocols whose
 * payload width is the format signal (Concord CORD-06 rekey blobs at 72, 104
 * or 136 bytes) cannot absorb that.
 *
 * The ciphertext is an ordinary NIP-44 v2 payload in both directions; only
 * the plaintext parameter/result is base64. The decode is strict and
 * canonical, so a typo fails the call rather than shortening the plaintext.
 * Caller frees the out parameter with free(). */
int nostr_nip55l_nip44_encrypt_b64(const char *plaintext_b64, const char *peer_pub_hex,
                                   const char *current_user, char **out_cipher_b64);
int nostr_nip55l_nip44_decrypt_b64(const char *cipher_b64, const char *peer_pub_hex,
                                   const char *current_user, char **out_plaintext_b64);
int nostr_nip55l_decrypt_zap_event(const char *event_json,
                                   const char *current_user, char **out_json);
/* GetRelays: the user's explicitly configured relays as a JSON array of
 * normalised ws:// / wss:// URL strings, read from
 * $XDG_CONFIG_HOME/nostr/relays.conf (default ~/.config/nostr/relays.conf).
 * Never touches the network.
 * Returns NOSTR_SIGNER_ERROR_NOT_FOUND when the file is absent or lists no
 * relays, NOSTR_SIGNER_ERROR_INVALID_JSON when it is malformed. */
int nostr_nip55l_get_relays(char **out_relays_json);
/* Parse and normalise a relays.conf document: a JSON array of relay URL
 * strings (no JSON escapes, at most 64 entries, 64 KiB). Scheme and host are
 * lowercased, a bare trailing "/" is dropped, duplicates are removed.
 * NOT_FOUND for an empty array, INVALID_JSON for anything else malformed. */
int nostr_nip55l_relays_normalize_json(const char *doc, size_t len, char **out_relays_json);
/* Same normalisation for an in-memory URL list. INVALID_ARG if any entry is
 * not a relay URL, NOT_FOUND if n == 0. */
int nostr_nip55l_relays_from_list(const char *const *urls, size_t n, char **out_relays_json);

/* Optional private key storage using libsecret when available. */
int nostr_nip55l_store_key(const char *key, const char *identity);
int nostr_nip55l_clear_key(const char *identity);

/* Optional Unix owner metadata (no enforcement). Selector is key_id or npub. */
/* Returns 0 on success, NOT_FOUND if libsecret unavailable or item missing. */
#include <sys/types.h>
#if defined(_WIN32) || defined(__MINGW32__)
/* MinGW does not provide uid_t; define as unsigned int for API compat */
#ifndef _UID_T_DEFINED
typedef unsigned int uid_t;
#define _UID_T_DEFINED
#endif
#endif
int nostr_nip55l_set_owner(const char *selector, uid_t uid, const char *username);
int nostr_nip55l_clear_owner(const char *selector);
/* Outputs: has_owner=1 if present; if present, uid_out and username_out (caller frees username_out). */
int nostr_nip55l_get_owner(const char *selector, int *has_owner, uid_t *uid_out, char **username_out);

#ifdef __cplusplus
}
#endif

#endif
