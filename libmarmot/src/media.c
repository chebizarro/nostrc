/*
 * libmarmot - MIP-04: Encrypted Media
 *
 * Implements file encryption/decryption using ChaCha20-Poly1305 with
 * keys derived from MLS exporter secrets.
 *
 * Encryption: exporter_secret → HKDF-SHA256("marmot-media-key") → key
 * Nonce: random 12 bytes
 * AEAD: ChaCha20-Poly1305(key, nonce, plaintext, aad=mime_type)
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"

#include <sodium.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <string.h>
#include <stdlib.h>

/* ── Key derivation ────────────────────────────────────────────────────── */

static int
derive_media_key(const uint8_t exporter_secret[32],
                 uint8_t out_key[32])
{
    /*
     * HKDF-Expand-Label(exporter_secret, "marmot-media-key", "", 32)
     * We use the same HKDF-Expand from our MLS crypto layer:
     *   PRK = exporter_secret
     *   info = "marmot-media-key"
     *   L = 32
     */
    /* Simple HKDF-Expand for one block (output <= hash size) */
    uint8_t info_block[1 + 16 + 1]; /* counter + label + 0x01 */
    unsigned int hmac_len = 32;

    /* HMAC-SHA256(PRK, label || 0x01) */
    const char *label = "marmot-media-key";
    size_t label_len = strlen(label);

    uint8_t input[64];
    size_t input_len = 0;
    memcpy(input + input_len, label, label_len);
    input_len += label_len;
    input[input_len++] = 0x01; /* counter byte */

    (void)info_block;

    uint8_t *result = HMAC(EVP_sha256(), exporter_secret, 32,
                            input, input_len, out_key, &hmac_len);
    return result ? 0 : -1;
}

/* ── AEAD encryption/decryption ────────────────────────────────────────── */

MarmotError
marmot_encrypt_media(Marmot *m,
                      const MarmotGroupId *mls_group_id,
                      const uint8_t *file_data, size_t file_len,
                      const char *mime_type,
                      const char *filename,
                      MarmotEncryptedMedia *result)
{
    if (!m || !mls_group_id || !file_data || !result)
        return MARMOT_ERR_INVALID_ARG;

    memset(result, 0, sizeof(*result));

    /* Look up the group to get the current epoch */
    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                                         mls_group_id, &group);
    if (err != MARMOT_OK) return err;
    if (!group) return MARMOT_ERR_GROUP_NOT_FOUND;

    uint64_t epoch = group->epoch;
    marmot_group_free(group);

    /* Get exporter secret for current epoch */
    uint8_t exporter_secret[32];
    err = m->storage->get_exporter_secret(m->storage->ctx,
                                            mls_group_id, epoch,
                                            exporter_secret);
    if (err != MARMOT_OK) return err;

    /* Derive media encryption key */
    uint8_t media_key[32];
    if (derive_media_key(exporter_secret, media_key) != 0) {
        sodium_memzero(exporter_secret, 32);
        return MARMOT_ERR_CRYPTO;
    }
    sodium_memzero(exporter_secret, 32);

    /* Generate random nonce */
    uint8_t nonce[12];
    randombytes_buf(nonce, sizeof(nonce));

    /* Allocate output: ciphertext + 16-byte Poly1305 tag */
    size_t ct_len = file_len + crypto_aead_chacha20poly1305_ietf_ABYTES;
    uint8_t *ciphertext = malloc(ct_len);
    if (!ciphertext) {
        sodium_memzero(media_key, 32);
        return MARMOT_ERR_MEMORY;
    }

    /* Encrypt with ChaCha20-Poly1305 */
    const uint8_t *aad = (const uint8_t *)(mime_type ? mime_type : "");
    size_t aad_len = mime_type ? strlen(mime_type) : 0;

    unsigned long long actual_ct_len;
    int rc = crypto_aead_chacha20poly1305_ietf_encrypt(
        ciphertext, &actual_ct_len,
        file_data, file_len,
        aad, aad_len,
        NULL, /* nsec (unused) */
        nonce,
        media_key
    );
    sodium_memzero(media_key, 32);

    if (rc != 0) {
        free(ciphertext);
        return MARMOT_ERR_CRYPTO;
    }

    /* Compute SHA-256 hash of the plaintext for integrity/dedup */
    uint8_t file_hash[32];
    SHA256(file_data, file_len, file_hash);

    /* Build result */
    result->encrypted_data = ciphertext;
    result->encrypted_len = (size_t)actual_ct_len;
    memcpy(result->nonce, nonce, 12);
    memcpy(result->file_hash, file_hash, 32);
    result->original_size = file_len;

    /* Build imeta info */
    if (mime_type) {
        result->imeta.mime_type = strdup(mime_type);
        if (!result->imeta.mime_type) {
            free(ciphertext);
            return MARMOT_ERR_MEMORY;
        }
    }
    if (filename) {
        result->imeta.filename = strdup(filename);
        if (!result->imeta.filename) {
            free(result->imeta.mime_type);
            free(ciphertext);
            return MARMOT_ERR_MEMORY;
        }
    }
    result->imeta.original_size = file_len;
    memcpy(result->imeta.file_hash, file_hash, 32);
    memcpy(result->imeta.nonce, nonce, 12);
    result->imeta.epoch = epoch;

    return MARMOT_OK;
}

MarmotError
marmot_decrypt_media(Marmot *m,
                      const MarmotGroupId *mls_group_id,
                      const uint8_t *encrypted_data, size_t enc_len,
                      const MarmotImetaInfo *imeta,
                      uint8_t **out_data, size_t *out_len)
{
    if (!m || !mls_group_id || !encrypted_data || !imeta || !out_data || !out_len)
        return MARMOT_ERR_INVALID_ARG;

    *out_data = NULL;
    *out_len = 0;

    /* Get exporter secret for the epoch specified in imeta */
    uint8_t exporter_secret[32];
    MarmotError err = m->storage->get_exporter_secret(m->storage->ctx,
                                                        mls_group_id, imeta->epoch,
                                                        exporter_secret);
    if (err != MARMOT_OK) return err;

    /* Derive media key */
    uint8_t media_key[32];
    if (derive_media_key(exporter_secret, media_key) != 0) {
        sodium_memzero(exporter_secret, 32);
        return MARMOT_ERR_CRYPTO;
    }
    sodium_memzero(exporter_secret, 32);

    /* Validate ciphertext length */
    if (enc_len < crypto_aead_chacha20poly1305_ietf_ABYTES) {
        sodium_memzero(media_key, 32);
        return MARMOT_ERR_INVALID_INPUT;
    }

    size_t pt_len = enc_len - crypto_aead_chacha20poly1305_ietf_ABYTES;
    uint8_t *plaintext = malloc(pt_len);
    if (!plaintext) {
        sodium_memzero(media_key, 32);
        return MARMOT_ERR_MEMORY;
    }

    /* Decrypt with ChaCha20-Poly1305 */
    const uint8_t *aad = (const uint8_t *)(imeta->mime_type ? imeta->mime_type : "");
    size_t aad_len = imeta->mime_type ? strlen(imeta->mime_type) : 0;

    unsigned long long actual_pt_len;
    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext, &actual_pt_len,
        NULL, /* nsec */
        encrypted_data, enc_len,
        aad, aad_len,
        imeta->nonce,
        media_key
    );
    sodium_memzero(media_key, 32);

    if (rc != 0) {
        free(plaintext);
        return MARMOT_ERR_MEDIA_DECRYPT;
    }

    /* Verify file hash if available */
    uint8_t zero_hash[32] = {0};
    if (memcmp(imeta->file_hash, zero_hash, 32) != 0) {
        uint8_t actual_hash[32];
        SHA256(plaintext, (size_t)actual_pt_len, actual_hash);
        if (memcmp(actual_hash, imeta->file_hash, 32) != 0) {
            free(plaintext);
            return MARMOT_ERR_MEDIA_HASH_MISMATCH;
        }
    }

    *out_data = plaintext;
    *out_len = (size_t)actual_pt_len;
    return MARMOT_OK;
}

/* ── Cleanup ───────────────────────────────────────────────────────────── */

void
marmot_encrypted_media_clear(MarmotEncryptedMedia *result)
{
    if (!result) return;
    free(result->encrypted_data);
    free(result->imeta.mime_type);
    free(result->imeta.filename);
    free(result->imeta.url);
    memset(result, 0, sizeof(*result));
}
