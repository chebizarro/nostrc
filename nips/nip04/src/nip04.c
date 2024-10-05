#include "nostr/nip04.h"
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Utility function to decode hex
static int hex2bin(const char *hex, unsigned char *bin, size_t bin_len) {
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0 || bin_len < hex_len / 2) {
        return -1;
    }
    for (size_t i = 0; i < hex_len; i += 2) {
        sscanf(hex + i, "%2hhx", &bin[i / 2]);
    }
    return 0;
}

// Compute the shared secret using ECDH
char* compute_shared_secret(const char *pub, const char *sk) {
    unsigned char priv_key_bytes[32];
    unsigned char pub_key_bytes[33];
    unsigned char secret[32];
    char *hex_secret = NULL;

    if (hex2bin(sk, priv_key_bytes, sizeof(priv_key_bytes)) < 0 ||
        hex2bin(pub, pub_key_bytes + 1, sizeof(pub_key_bytes) - 1) < 0) {
        return NULL;
    }
    pub_key_bytes[0] = 0x02; // Compressed key prefix

    EC_KEY *priv_key = EC_KEY_new_by_curve_name(NID_secp256k1);
    BIGNUM *priv_bn = BN_bin2bn(priv_key_bytes, sizeof(priv_key_bytes), NULL);
    EC_KEY_set_private_key(priv_key, priv_bn);

    EC_GROUP *group = EC_GROUP_new_by_curve_name(NID_secp256k1);
    EC_POINT *pub_point = EC_POINT_new(group);
    EC_POINT_oct2point(group, pub_point, pub_key_bytes, sizeof(pub_key_bytes), NULL);

    ECDH_compute_key(secret, sizeof(secret), pub_point, priv_key, NULL);

    hex_secret = malloc(65);
    for (size_t i = 0; i < sizeof(secret); i++) {
        sprintf(hex_secret + (i * 2), "%02x", secret[i]);
    }
    hex_secret[64] = '\0';

    BN_free(priv_bn);
    EC_POINT_free(pub_point);
    EC_GROUP_free(group);
    EC_KEY_free(priv_key);

    return hex_secret;
}

// Encrypt the message using AES-256-CBC
char* encrypt_message(const char *message, const char *key) {
    unsigned char iv[AES_BLOCK_SIZE];
    unsigned char *ciphertext;
    char *output;
    int len, ciphertext_len;
    int padding_len = AES_BLOCK_SIZE - (strlen(message) % AES_BLOCK_SIZE);

    ciphertext = malloc(strlen(message) + padding_len);
    if (!ciphertext) return NULL;

    RAND_bytes(iv, sizeof(iv));

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, (unsigned char *)key, iv);
    EVP_EncryptUpdate(ctx, ciphertext, &len, (unsigned char *)message, strlen(message));
    ciphertext_len = len;
    EVP_EncryptFinal_ex(ctx, ciphertext + len, &len);
    ciphertext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    output = malloc(ciphertext_len * 2 + sizeof(iv) * 2 + 10);
    if (!output) {
        free(ciphertext);
        return NULL;
    }

    char *iv_hex = output;
    for (size_t i = 0; i < sizeof(iv); i++) {
        sprintf(iv_hex + (i * 2), "%02x", iv[i]);
    }
    iv_hex[sizeof(iv) * 2] = '\0';

    char *cipher_hex = iv_hex + sizeof(iv) * 2 + 4; // Space for "?iv="

    for (int i = 0; i < ciphertext_len; i++) {
        sprintf(cipher_hex + (i * 2), "%02x", ciphertext[i]);
    }
    cipher_hex[ciphertext_len * 2] = '\0';

    sprintf(output, "%s?iv=%s", cipher_hex, iv_hex);

    free(ciphertext);
    return output;
}

// Decrypt the content using AES-256-CBC
char* decrypt_message(const char *content, const char *key) {
    unsigned char iv[AES_BLOCK_SIZE];
    unsigned char *ciphertext;
    char *plaintext;
    int len, plaintext_len;
    char *cipher_hex = strdup(content);
    char *iv_hex = strstr(cipher_hex, "?iv=");
    if (!iv_hex) {
        free(cipher_hex);
        return NULL;
    }
    *iv_hex = '\0';
    iv_hex += 4;

    size_t cipher_hex_len = strlen(cipher_hex);
    size_t iv_hex_len = strlen(iv_hex);

    if (hex2bin(cipher_hex, ciphertext = malloc(cipher_hex_len / 2), cipher_hex_len / 2) < 0 ||
        hex2bin(iv_hex, iv, sizeof(iv)) < 0) {
        free(cipher_hex);
        return NULL;
    }
    free(cipher_hex);

    plaintext = malloc(cipher_hex_len / 2 + 1);
    if (!plaintext) {
        free(ciphertext);
        return NULL;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, (unsigned char *)key, iv);
    EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, cipher_hex_len / 2);
    plaintext_len = len;
    EVP_DecryptFinal_ex(ctx, plaintext + len, &len);
    plaintext_len += len;
    plaintext[plaintext_len] = '\0';
    EVP_CIPHER_CTX_free(ctx);

    free(ciphertext);
    return plaintext;
}
