#ifndef NOSTR_NIP55L_SIGNER_OPS_H
#define NOSTR_NIP55L_SIGNER_OPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nip55l component version (see VERSION_MANIFEST.md). 0.2.0: the
 * org.nostr.Signer.SignEvent D-Bus method returns the complete signed event
 * JSON (nostr_nip55l_sign_event_json) instead of the bare signature.
 * 0.3.0 (additive): org.nostr.Signer.NIP44DeriveConversationKey, an
 * approval-gated export of the NIP-44 v2 conversation key for one peer
 * (nostr_nip55l_nip44_conversation_key; used by nostr-seal, nostrc-da9c).
 * 0.4.0 (ACL semantics): grants are keyed on a bus-derived caller principal
 * plus the npub the identity selector resolves to (never on the caller's
 * app_id string), GetPublicKey / GetRelays / NIP-04 / NIP-44 / DecryptZapEvent
 * go through the same approval flow as SignEvent, only the installed approval
 * UI may call ApproveRequest, and *ForApp / GetApprovalInfo methods were
 * added (nostrc-y02q, nostrc-phk4, nostrc-1e31, nostrc-eie5).
 * 0.5.0 (additive): org.nostr.Signer.EnableTypedApprovalErrors lets a bus
 * connection opt in to Error.ApprovalTimedOut / Error.NoApprovalAgent /
 * Error.IdentityChanged in place of Error.ApprovalDenied for those cases
 * (nostrc-qp24.16). 0.5.1: refuse to approve a parked D-Bus request whose
 * caller disconnected, even before its NameOwnerChanged cleanup runs. */
#define NOSTR_NIP55L_VERSION_MAJOR 0
#define NOSTR_NIP55L_VERSION_MINOR 5
#define NOSTR_NIP55L_VERSION_PATCH 1
#define NOSTR_NIP55L_VERSION_STRING "0.5.1"

int nostr_nip55l_get_public_key(char **out_npub);
/* npub of the key that `current_user` selects, i.e. the key sign/encrypt/
 * decrypt would use for the same selector (empty = the active identity).
 * This is the canonical identity the daemon's ACL is keyed on: the selector
 * itself may be empty, a key_id, an npub or even a secret key. NOT_FOUND /
 * INVALID_KEY when no key matches. Caller frees *out_npub with free(). */
int nostr_nip55l_resolve_npub(const char *current_user, char **out_npub);
/* Normalise an identity selector supplied by another process (the D-Bus
 * daemon's callers, nostrc-a4w5). The in-process resolver also accepts a
 * raw secret (64-hex or nsec) as a selector; a caller over the bus must not
 * be able to do that - a hex *pubkey* passed by mistake would be used as a
 * private key. This never interprets its input as key material:
 *   NULL / ""   -> "" (the active identity)
 *   nsec1...    -> NOSTR_SIGNER_ERROR_INVALID_ARG
 *   64-hex      -> an x-only public key, i.e. the same as its npub
 *   npub1...    -> must name a known identity: the active one (-> "") or a
 *                  stored one whose key has exactly this npub; else
 *                  NOSTR_SIGNER_ERROR_NOT_FOUND
 *   otherwise   -> a key_id / label, resolved as before
 * On success *out_selector is the selector to pass to the other calls and
 * *out_npub the npub it resolves to; free both with free(). */
int nostr_nip55l_normalize_selector(const char *selector, char **out_selector, char **out_npub);
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
/* NIP-44 v2 conversation key between the selected identity and peer_pub_hex
 * (64-hex x-only), returned as 64 lowercase hex. Same derivation as
 * nostr_nip44_convkey; the secret key stays in the signer. INVALID_KEY for a
 * malformed or off-curve peer. Caller frees *out_convkey_hex with free(). */
int nostr_nip55l_nip44_conversation_key(const char *peer_pub_hex,
                                        const char *current_user,
                                        char **out_convkey_hex);
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

/* One-shot keyring migration (nostrc-bml6). Moves private keys stored under
 * the legacy libsecret schemas (org.gnostr.Signer/key, org.gnostr.Key) to the
 * unified org.gnostr.Signer/identity schema and deletes the originals.
 * Idempotent; guarded by a per-keyring marker item once a pass completes.
 * On macOS (Keychain builds, nostrc-de9h) the same pass imports the keys
 * GNostr stored itself before nostrc-e5nz (generic passwords, service
 * "org.gnostr.Client", account = npub, data = nsec) into the daemon's
 * "Gnostr Identity Key" items, guarded by a Keychain marker item.
 * The daemon runs it once at startup. Returns 0 when nothing is left to
 * retry (including "no Secret Service support compiled in"),
 * NOSTR_SIGNER_ERROR_BACKEND when the Secret Service was unreachable or some
 * item must be retried on a later start. *out is always filled. */
typedef struct {
  int already_done;       /* marker present: nothing was searched */
  unsigned found;         /* legacy items seen */
  unsigned migrated;      /* re-stored under the unified schema, original deleted */
  unsigned skipped;       /* permanently unmigratable (hardware ref / not a key); left in place */
  unsigned failed;        /* transient failure; retried on the next start */
  int marker_written;     /* this pass wrote the completion marker */
} nostr_nip55l_keyring_migration;
int nostr_nip55l_migrate_legacy_keys(nostr_nip55l_keyring_migration *out);

/* Optional private key storage using libsecret when available. */
int nostr_nip55l_store_key(const char *key, const char *identity);
int nostr_nip55l_clear_key(const char *identity);

/* List all stored identity npubs.
 * Caller frees each string with free() and the array with free().
 * Returns 0 on success; *out_count is the number of entries. */
int nostr_nip55l_list_identities(char ***out_npubs, int *out_count);

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
