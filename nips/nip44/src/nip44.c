#include "nip44.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include "nip04.h"

static void chacha20_xor(uint8_t *key, uint8_t *nonce, uint8_t *message, size_t message_len, uint8_t *output) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len;
    EVP_EncryptInit_ex(ctx, EVP_chacha20(), NULL, key, nonce);
    EVP_EncryptUpdate(ctx, output, &len, message, message_len);
    EVP_EncryptFinal_ex(ctx, output + len, &len);
    EVP_CIPHER_CTX_free(ctx);
}

static void sha256_hmac(uint8_t *key, uint8_t *ciphertext, size_t ciphertext_len, uint8_t *aad, uint8_t *hmac_output) {
    unsigned int len;
    HMAC_CTX *hctx = HMAC_CTX_new();
    HMAC_Init_ex(hctx, key, 32, EVP_sha256(), NULL);
    HMAC_Update(hctx, aad, 32);
    HMAC_Update(hctx, ciphertext, ciphertext_len);
    HMAC_Final(hctx, hmac_output, &len);
    HMAC_CTX_free(hctx);
}

static void hkdf_expand(uint8_t *conversation_key, uint8_t *salt, uint8_t *enc, uint8_t *nonce, uint8_t *auth) {
    unsigned int len;
    uint8_t prk[32];
    uint8_t info[32] = "nip44-v2";
    uint8_t okm[76];

    HMAC_CTX *hctx = HMAC_CTX_new();
    HMAC_Init_ex(hctx, salt, 32, EVP_sha256(), NULL);
    HMAC_Update(hctx, conversation_key, 32);
    HMAC_Final(hctx, prk, &len);
    HMAC_CTX_reset(hctx);

    HMAC_Init_ex(hctx, prk, 32, EVP_sha256(), NULL);
    HMAC_Update(hctx, info, 32);
    HMAC_Final(hctx, okm, &len);
    memcpy(enc, okm, 32);
    memcpy(nonce, okm + 32, 12);
    memcpy(auth, okm + 44, 32);
    HMAC_CTX_free(hctx);
}

void nip44_generate_conversation_key(const char *pub, const char *sk, uint8_t *key) {
    uint8_t shared[32];
    nip04_compute_shared_secret(pub, sk, shared);
    hkdf_expand(shared, (uint8_t *)"nip44-v2", key, key, key);
}

char *nip44_encrypt(const char *plaintext, const uint8_t *conversation_key, const nip44_encrypt_options_t *options) {
    uint8_t salt[32];
    if (options && options->salt_len == 32) {
        memcpy(salt, options->salt, 32);
    } else {
        RAND_bytes(salt, 32);
    }

    uint8_t enc[32], nonce[12], auth[32];
    hkdf_expand((uint8_t *)conversation_key, salt, enc, nonce, auth);

    size_t padded_len = (size_t)ceil((double)(strlen(plaintext) + 2) / 32) * 32;
    uint8_t *padded = (uint8_t *)malloc(padded_len);
    uint16_t plaintext_len = strlen(plaintext);
    padded[0] = (uint8_t)(plaintext_len >> 8);
    padded[1] = (uint8_t)(plaintext_len & 0xFF);
    memcpy(padded + 2, plaintext, plaintext_len);
    memset(padded + 2 + plaintext_len, 0, padded_len - 2 - plaintext_len);

    uint8_t *ciphertext = (uint8_t *)malloc(padded_len);
    chacha20_xor(enc, nonce, padded, padded_len, ciphertext);

    uint8_t hmac[32];
    sha256_hmac(auth, ciphertext, padded_len, salt, hmac);

    size_t output_len = 1 + 32 + padded_len + 32;
    char *output = (char *)malloc(output_len * 2 + 1);
    sprintf(output, "%02x", NIP44_VERSION);
    for (int i = 0; i < 32; i++) {
        sprintf(output + 2 + i * 2, "%02x", salt[i]);
    }
    for (size_t i = 0; i < padded_len; i++) {
        sprintf(output + 2 + 64 + i * 2, "%02x", ciphertext[i]);
    }
    for (int i = 0; i < 32; i++) {
        sprintf(output + 2 + 64 + padded_len * 2 + i * 2, "%02x", hmac[i]);
    }

    free(padded);
    free(ciphertext);

    return output;
}

char *nip44_decrypt(const char *ciphertext, const uint8_t *conversation_key) {
    size_t ciphertext_len = strlen(ciphertext);
    if (ciphertext_len < 264) {
        return NULL; // invalid payload length
    }

    uint8_t version;
    sscanf(ciphertext, "%2hhx", &version);
    if (version != NIP44_VERSION) {
        return NULL; // unknown version
    }

    uint8_t salt[32];
    for (int i = 0; i < 32; i++) {
        sscanf(ciphertext + 2 + i * 2, "%2hhx", &salt[i]);
    }

    size_t encrypted_len = (ciphertext_len - 66) / 2;
    uint8_t *encrypted = (uint8_t *)malloc(encrypted_len);
    for (size_t i = 0; i < encrypted_len; i++) {
        sscanf(ciphertext + 66 + i * 2, "%2hhx", &encrypted[i]);
    }

    uint8_t received_hmac[32];
    for (int i = 0; i < 32; i++) {
        sscanf(ciphertext + 66 + encrypted_len * 2 + i * 2, "%2hhx", &received_hmac[i]);
    }

    uint8_t enc[32], nonce[12], auth[32];
    hkdf_expand((uint8_t *)conversation_key, salt, enc, nonce, auth);

    uint8_t calculated_hmac[32];
    sha256_hmac(auth, encrypted, encrypted_len, salt, calculated_hmac);

    if (memcmp(received_hmac, calculated_hmac, 32) != 0) {
        free(encrypted);
        return NULL; // invalid hmac
    }

    uint8_t *padded = (uint8_t *)malloc(encrypted_len);
    chacha20_xor(enc, nonce, encrypted, encrypted_len, padded);

    uint16_t plaintext_len = (padded[0] << 8) | padded[1];
    if (plaintext_len < MIN_PLAINTEXT_SIZE || plaintext_len > MAX_PLAINTEXT_SIZE) {
        free(encrypted);
        free(padded);
        return NULL; // invalid padding
    }

    char *plaintext = (char *)malloc(plaintext_len + 1);
    memcpy(plaintext, padded + 2, plaintext_len);
    plaintext[plaintext_len] = '\0';

    free(encrypted);
    free(padded);

    return plaintext;
}
