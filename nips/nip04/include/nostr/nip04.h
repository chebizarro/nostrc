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
 * Cryptography:
 * - ECDH over secp256k1 to derive the shared secret (X coordinate).
 * - AES key = SHA-256(shared_x) via OpenSSL EVP_Digest.
 * - AES-256-CBC with PKCS#7 padding; 16-byte IV.
 *
 * Content format:
 * - "base64(ciphertext)?iv=base64(iv)"
 *
 * Memory and errors:
 * - Functions return 0 on success and non-zero on error.
 * - On success, output strings are allocated; caller must free with free().
 * - On failure, if @out_error is not NULL, an allocated error message is returned; caller must free.
 */

/**
 * nostr_nip04_encrypt:
 * @plaintext_utf8: (not nullable): NUL-terminated UTF-8 plaintext to encrypt.
 * @receiver_pubkey_hex: (not nullable): Hex-encoded secp256k1 public key (compressed 33-byte or uncompressed 65-byte).
 * @sender_seckey_hex: (not nullable): Hex-encoded 32-byte secp256k1 secret key.
 * @out_content_b64_qiv: (out) (transfer full): On success, set to newly allocated content string "base64(ct)?iv=base64(iv)".
 * @out_error: (out) (optional) (transfer full): On error, set to an allocated error message.
 *
 * Encrypts @plaintext_utf8 for @receiver_pubkey_hex using NIP-04. The IV is randomly generated.
 * For tests, the environment variable NIP04_TEST_IV_B64 may be set to a base64-encoded 16-byte IV.
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
 * @content_b64_qiv: (not nullable): The NIP-04 content string "base64(ct)?iv=base64(iv)".
 * @sender_pubkey_hex: (not nullable): Hex-encoded secp256k1 public key of the sender.
 * @receiver_seckey_hex: (not nullable): Hex-encoded 32-byte secp256k1 secret key of the receiver.
 * @out_plaintext_utf8: (out) (transfer full): On success, newly allocated decrypted UTF-8 string.
 * @out_error: (out) (optional) (transfer full): On error, allocated error message.
 *
 * Decrypts a NIP-04 content string using receiver's secret key and sender's public key.
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
 * nostr_nip04_decrypt_secure:
 * Like nostr_nip04_decrypt but takes the receiver secret key as a secure buffer.
 */
int nostr_nip04_decrypt_secure(
    const char *content_b64_qiv,
    const char *sender_pubkey_hex,
    const nostr_secure_buf *receiver_seckey,
    char **out_plaintext_utf8,
    char **out_error);

/**
 * nostr_nip04_shared_secret_hex:
 * @peer_pubkey_hex: (not nullable): Hex-encoded secp256k1 public key of the peer.
 * @self_seckey_hex: (not nullable): Hex-encoded 32-byte secp256k1 secret key of self.
 * @out_shared_hex: (out) (transfer full): On success, newly allocated 64-char hex of shared X coordinate.
 * @out_error: (out) (optional) (transfer full): On error, allocated error message.
 *
 * Computes the raw ECDH shared secret X coordinate (for diagnostics). Not required for normal usage.
 *
 * Returns: 0 on success; non-zero on failure.
 */
int nostr_nip04_shared_secret_hex(
    const char *peer_pubkey_hex,
    const char *self_seckey_hex,
    char **out_shared_hex,
    char **out_error);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP04_H */
