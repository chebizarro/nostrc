/* SPDX-License-Identifier: MIT — NIP-94 / kind-15 single-file projection (A2). */
#include "internal.h"
#include <vault/tree.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <limits.h>

#define NIP94_KEY_LEN 32u
#define NIP94_NONCE_LEN 12u

static int file_type_valid(const char *s)
{
    size_t n = 0;
    for (; s[n]; ++n) {
        unsigned char c = (unsigned char)s[n];
        if (c <= ' ' || c >= 0x7f || c == '"' || c == '\\')
            return 0;
    }
    return n > 0 && n <= 255;
}

static int classify_upload(blossom_error_t e)
{
    switch (e) {
    case BLOSSOM_OK:
        return VAULT_OK;
    case BLOSSOM_ERR_NOMEM:
        return VAULT_ERR_OOM;
    case BLOSSOM_ERR_INVALID_ARG:
        return VAULT_ERR_ARG;
    case BLOSSOM_ERR_HASH_MISMATCH:
        return PORTHOME_ERR_HASH_MISMATCH;
    case BLOSSOM_ERR_AUTH:
        return VAULT_ERR_AUTH;
    case BLOSSOM_ERR_TOO_LARGE:
        return PORTHOME_ERR_TOO_LARGE;
    default:
        return VAULT_ERR_IO;
    }
}

/* AES-256-GCM over the whole file: ct || tag, as WebCrypto/Quartz produce. */
static int aes_gcm_seal(const uint8_t key[NIP94_KEY_LEN], const uint8_t nonce[NIP94_NONCE_LEN],
                        const uint8_t *pt, size_t pt_len, uint8_t *out)
{
    static const uint8_t empty = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int rc = PORTHOME_ERR_CRYPTO, outl = 0, finl = 0;
    if (!ctx)
        return VAULT_ERR_OOM;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)NIP94_NONCE_LEN, NULL) == 1 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
        EVP_EncryptUpdate(ctx, out, &outl, pt_len ? pt : &empty, (int)pt_len) == 1 &&
        outl == (int)pt_len && EVP_EncryptFinal_ex(ctx, out + outl, &finl) == 1 && finl == 0 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)VAULT_NIP94_TAG_LEN, out + pt_len) == 1)
        rc = VAULT_OK;
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

static void hex_lower(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xf];
    }
    out[2 * len] = 0;
}

static int ring_has_epoch(const porthome_ring_t *ring, uint64_t epoch)
{
    for (size_t i = 0; epoch && i < ring->count; ++i)
        if (ring->entries[i].epoch == epoch)
            return 1;
    return 0;
}

static int hex_equal_ci(const char *a, const char *b, size_t len)
{
    if (!a || !b || strlen(a) != len || strlen(b) != len)
        return 0;
    for (size_t i = 0; i < len; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'F')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'F')
            y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return 1;
}

/* Reassemble the file through the authenticated chunk reader. Chunk lengths
 * must sum exactly to the entry's size_plain; nothing is guessed from padding. */
static int read_plain(const porthome_node_entry *file, const porthome_ring_t *ring,
                      const vault_blob_io *io, uint8_t **out, size_t *out_len)
{
    size_t total = (size_t)file->size_plain, used = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < file->chunks_len; ++i) {
        if (!file->chunks || sum > UINT64_MAX - file->chunks[i].size_plain)
            return VAULT_ERR_ARG;
        sum += file->chunks[i].size_plain;
    }
    if (sum != file->size_plain)
        return VAULT_ERR_ARG;
    uint8_t *buf = malloc(total ? total : 1);
    if (!buf)
        return VAULT_ERR_OOM;
    int rc = VAULT_OK;
    for (size_t i = 0; i < file->chunks_len && !rc; ++i) {
        uint8_t *plain = NULL;
        size_t plain_len = 0;
        rc = vault_chunk_read(&file->chunks[i], ring, io, &plain, &plain_len);
        if (!rc && (plain_len != file->chunks[i].size_plain || plain_len > total - used))
            rc = VAULT_ERR_ARG;
        if (!rc && plain_len)
            memcpy(buf + used, plain, plain_len);
        used += rc ? 0 : plain_len;
        if (plain) {
            OPENSSL_cleanse(plain, plain_len);
            free(plain);
        }
    }
    if (!rc && used != total)
        rc = VAULT_ERR_ARG;
    if (rc) {
        OPENSSL_cleanse(buf, total);
        free(buf);
        return rc;
    }
    *out = buf;
    *out_len = total;
    return VAULT_OK;
}

int vault_file_export_nip94(const porthome_node_entry *file, const porthome_ring_t *ring,
                            const vault_blob_io *io, blossom_client_t *client,
                            const char *file_type, vault_nip94_tags **out)
{
    if (out)
        *out = NULL;
    if (!file || !io || !io->read || !client || !out || file->kind != PORTHOME_KIND_FILE ||
        (file_type && !file_type_valid(file_type)))
        return VAULT_ERR_ARG;
    int rc = vault_ring_valid(ring);
    if (rc)
        return rc;
    /* Memory bound: plaintext + ciphertext resident together. Refuse before I/O. */
    if (file->size_plain > VAULT_NIP94_MAX_EXPORT_BYTES ||
        file->size_plain > (uint64_t)INT_MAX - VAULT_NIP94_TAG_LEN)
        return PORTHOME_ERR_TOO_LARGE;
    /* Gate every chunk's exact epoch before any I/O; no key retries later. */
    for (size_t i = 0; i < file->chunks_len; ++i)
        if (!file->chunks || !ring_has_epoch(ring, file->chunks[i].chunk_key_id))
            return VAULT_ERR_EPOCH;

    uint8_t *plain = NULL, *cipher = NULL, key[NIP94_KEY_LEN], nonce[NIP94_NONCE_LEN];
    uint8_t plain_hash[32], cipher_hash[32];
    size_t plain_len = 0, cipher_len = 0;
    vault_nip94_tags *tags = NULL;
    blossom_blob_descriptor_t *desc = NULL;
    rc = read_plain(file, ring, io, &plain, &plain_len);
    if (rc)
        goto done;
    rc = porthome_sha256(plain, plain_len, plain_hash);
    if (rc)
        goto done;
    /* Fresh export key and nonce from the CSPRNG; never ring-derived. */
    if (RAND_bytes(key, sizeof key) != 1 || RAND_bytes(nonce, sizeof nonce) != 1) {
        rc = PORTHOME_ERR_CRYPTO;
        goto done;
    }
    cipher_len = plain_len + VAULT_NIP94_TAG_LEN;
    cipher = malloc(cipher_len);
    if (!cipher) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    rc = aes_gcm_seal(key, nonce, plain, plain_len, cipher);
    if (rc)
        goto done;
    OPENSSL_cleanse(plain, plain_len);
    free(plain);
    plain = NULL;
    rc = porthome_sha256(cipher, cipher_len, cipher_hash);
    if (rc)
        goto done;
    tags = calloc(1, sizeof *tags);
    if (!tags) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    porthome_hex64(cipher_hash, tags->x);
    porthome_hex64(plain_hash, tags->ox);
    hex_lower(key, sizeof key, tags->decryption_key);
    hex_lower(nonce, sizeof nonce, tags->decryption_nonce);
    tags->encryption_algorithm = VAULT_NIP94_ALGORITHM;
    tags->size = cipher_len;
    if (file_type) {
        tags->file_type = strdup(file_type);
        if (!tags->file_type) {
            rc = VAULT_ERR_OOM;
            goto done;
        }
    }
    /* Existing signed upload path; the expected hash pins the exact wire bytes. */
    rc = classify_upload(blossom_client_put(client, cipher, cipher_len, tags->x, &desc));
    if (rc)
        goto done;
    if (!desc || !hex_equal_ci(desc->sha256, tags->x, 64) || desc->size != cipher_len) {
        rc = PORTHOME_ERR_HASH_MISMATCH;
        goto done;
    }
    if (desc->url) {
        tags->url = strdup(desc->url);
        if (!tags->url) {
            rc = VAULT_ERR_OOM;
            goto done;
        }
    }
    *out = tags;
    tags = NULL;
done:
    if (plain) {
        OPENSSL_cleanse(plain, plain_len);
        free(plain);
    }
    free(cipher);
    OPENSSL_cleanse(key, sizeof key);
    OPENSSL_cleanse(nonce, sizeof nonce);
    blossom_blob_descriptor_free(desc);
    vault_nip94_tags_free(tags);
    return rc;
}

void vault_nip94_tags_free(vault_nip94_tags *t)
{
    if (!t)
        return;
    free(t->file_type);
    free(t->url);
    OPENSSL_cleanse(t, sizeof *t);
    free(t);
}
