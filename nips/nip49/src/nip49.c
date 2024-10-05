#include "nip49.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sodium.h>
#include <bech32.h>
#include <chacha20poly1305.h>
#include <scrypt.h>

static int get_key(const char *password, const uint8_t *salt, int n, uint8_t *key) {
    if (crypto_pwhash_scryptsalsa208sha256_ll((const uint8_t *)password, strlen(password), salt, 16, n, 8, 1, key, 32) != 0) {
        return -1;
    }
    return 0;
}

int nip49_encrypt(const char *secret_key, const char *password, uint8_t logn, KeySecurityByte ksb, char **b32code) {
    if (strlen(secret_key) != 64) {
        return -1;
    }

    uint8_t skb[32];
    sodium_hex2bin(skb, 32, secret_key, 64, NULL, NULL, NULL);

    uint8_t salt[16];
    randombytes_buf(salt, 16);

    int n = pow(2, logn);
    uint8_t key[32];
    if (get_key(password, salt, n, key) != 0) {
        return -1;
    }

    uint8_t concat[91];
    concat[0] = 0x02;
    concat[1] = logn;
    memcpy(concat + 2, salt, 16);
    randombytes_buf(concat + 18, 24);
    concat[42] = ksb;

    if (crypto_aead_chacha20poly1305_ietf_encrypt(concat + 43, NULL, skb, 32, concat + 42, 1, NULL, concat + 18, key) != 0) {
        return -1;
    }

    size_t b32_len = 0;
    *b32code = bech32_encode("ncryptsec", concat, 91, &b32_len);
    if (*b32code == NULL) {
        return -1;
    }

    return 0;
}

int nip49_decrypt(const char *b32code, const char *password, char **secret_key) {
    uint8_t data[91];
    size_t data_len = bech32_decode(data, sizeof(data), b32code);
    if (data_len != 91 || data[0] != 0x02) {
        return -1;
    }

    uint8_t logn = data[1];
    int n = pow(2, logn);
    uint8_t *salt = data + 2;
    uint8_t *nonce = data + 18;
    uint8_t *ad = data + 42;
    uint8_t *encrypted_key = data + 43;

    uint8_t key[32];
    if (get_key(password, salt, n, key) != 0) {
        return -1;
    }

    uint8_t skb[32];
    if (crypto_aead_chacha20poly1305_ietf_decrypt(skb, NULL, NULL, encrypted_key, 48, ad, 1, nonce, key) != 0) {
        return -1;
    }

    *secret_key = malloc(65);
    sodium_bin2hex(*secret_key, 65, skb, 32);
    return 0;
}
