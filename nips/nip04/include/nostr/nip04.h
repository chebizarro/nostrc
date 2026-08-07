#ifndef NOSTR_NIP04_H
#define NOSTR_NIP04_H

#include <stddef.h>
#include <secure_buf.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NIP-04: Encrypted Direct Messages
 *
 * The NIP-04 module implements encrypted direct messaging for Nostr.
 *
 * Cryptography and formats:
 * - Generic encryption emits the nostrc AEAD v2 extension:
 *   "v=2:base64(nonce(12) || ciphertext || tag(16))".
 * - AEAD v2 derives AES-256-GCM key material from the raw secp256k1 ECDH X
 *   coordinate with HKDF-SHA256 and info="NIP04".
 * - Decryption also accepts original NIP-04 interoperability payloads:
 *   "base64(ciphertext)?iv=base64(iv)", using raw shared X directly as the
 *   AES-256-CBC key with PKCS#7 padding.
 * - Original CBC has no integrity protection. Emit it only via the explicit
 *   legacy API and only inside a validated authenticated outer envelope.
 *
 * Memory and errors:
 * - Functions return 0 on success and non-zero on error.
 * - On success, output strings are allocated; caller frees them with free().
 * - All public decrypt failures use the allocated string "decrypt failed"
 *   when @out_error is supplied. This intentionally hides format, padding,
 *   KDF, and provider failure details.
 */

/**
 * nostr_nip04_encrypt:
 * @plaintext_utf8: (not nullable): NUL-terminated UTF-8 plaintext to encrypt.
 * @receiver_pubkey_hex: (not nullable): Hex-encoded x-only (32-byte), compressed (33-byte), or uncompressed (65-byte) secp256k1 public key.
 * @sender_seckey_hex: (not nullable): Hex-encoded 32-byte secp256k1 secret key.
 * @out_content_b64_qiv: (out) (transfer full): On success, newly allocated AEAD v2 content string.
 * @out_error: (out) (optional) (transfer full): On error, set to an allocated error message.
 *
 * Encrypts @plaintext_utf8 using the authenticated nostrc AEAD v2
 * extension. This generic API never emits legacy CBC and has no environment
 * variable that can downgrade its output.
 *
 * Returns: 0 on success; non-zero on failure.
 */
int nostr_nip04_encrypt(
    const char *plaintext_utf8,
    const char *receiver_pubkey_hex,
    const char *sender_seckey_hex,
    char **out_content_b64_qiv,
    char **out_error);

/**
 * nostr_nip04_decrypt:
 * @content_b64_qiv: (not nullable): AEAD v2 or original NIP-04 CBC content.
 * @sender_pubkey_hex: (not nullable): Hex-encoded secp256k1 public key of the sender.
 * @receiver_seckey_hex: (not nullable): Hex-encoded 32-byte secp256k1 secret key of the receiver.
 * @out_plaintext_utf8: (out) (transfer full): On success, newly allocated decrypted UTF-8 string.
 * @out_error: (out) (optional) (transfer full): On failure, allocated uniform string "decrypt failed".
 *
 * Decrypts AEAD v2 and, unless disabled at build time, original CBC
 * interoperability payloads. Callers must not distinguish failure causes.
 *
 * Returns: 0 on success; non-zero on failure.
 */
int nostr_nip04_decrypt(
    const char *content_b64_qiv,
    const char *sender_pubkey_hex,
    const char *receiver_seckey_hex,
    char **out_plaintext_utf8,
    char **out_error);

/**
 * nostr_nip04_encrypt_secure:
 * Like nostr_nip04_encrypt but takes the sender secret key as a secure buffer.
 */
int nostr_nip04_encrypt_secure(
    const char *plaintext_utf8,
    const char *receiver_pubkey_hex,
    const nostr_secure_buf *sender_seckey,
    char **out_content_b64_qiv,
    char **out_error);

/**
 * nostr_nip04_encrypt_legacy_secure:
 * Encrypts using the ORIGINAL NIP-04 format (AES-256-CBC with ?iv= output).
 * Required only for explicitly configured legacy peers.
 * Output format: "base64(ciphertext)?iv=base64(iv)".
 * WARNING: this format is unauthenticated and malleable.
 */
int nostr_nip04_encrypt_legacy_secure(
    const char *plaintext_utf8,
    const char *receiver_pubkey_hex,
    const nostr_secure_buf *sender_seckey,
    char **out_content_b64_qiv,
    char **out_error);

/**
 * nostr_nip04_decrypt_secure:
 * Like nostr_nip04_decrypt but takes the receiver secret key as a secure
 * buffer. It has the same uniform "decrypt failed" error contract.
 */
int nostr_nip04_decrypt_secure(
    const char *content_b64_qiv,
    const char *sender_pubkey_hex,
    const nostr_secure_buf *receiver_seckey,
    char **out_plaintext_utf8,
    char **out_error);

/**
 * nostr_nip04_legacy_decrypt_enabled:
 *
 * Reports whether this build of the NIP-04 module accepts original NIP-04
 * CBC interoperability payloads ("base64(ct)?iv=base64(iv)") on decrypt.
 * Builds configured with NIP04_STRICT_AEAD_ONLY compile the legacy decrypt
 * path out entirely; callers that would negotiate a legacy transport must
 * consult this capability first instead of failing mid-conversation.
 *
 * Returns: 1 when legacy CBC decrypt is available; 0 in strict AEAD-only builds.
 */
int nostr_nip04_legacy_decrypt_enabled(void);

/**
 * nostr_nip04_shared_secret_hex: DEPRECATED — do not use in new code.
 * @peer_pubkey_hex: (not nullable): Hex-encoded secp256k1 public key of the peer.
 * @self_seckey_hex: (not nullable): Hex-encoded 32-byte secp256k1 secret key of self.
 * @out_shared_hex: (out) (transfer full): On success, newly allocated 64-char hex of shared X coordinate.
 * @out_error: (out) (optional) (transfer full): On failure, allocated diagnostic error string.
 *
 * Computes the raw ECDH shared secret X coordinate (for diagnostics). Not required for normal usage and insecure to expose.
 * Deprecated because exposing raw shared secrets increases attack surface. Use the AEAD encrypt/decrypt APIs instead.
 *
 * Returns: 0 on success; non-zero on failure.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((deprecated("nostr_nip04_shared_secret_hex is deprecated; use AEAD encrypt/decrypt instead")))
#endif
int nostr_nip04_shared_secret_hex(
    const char *peer_pubkey_hex,
    const char *self_seckey_hex,
    char **out_shared_hex,
    char **out_error);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP04_H */
