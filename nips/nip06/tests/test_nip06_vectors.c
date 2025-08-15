#include "nip06.h"
#include <nostr/crypto/bip32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex2bin(const char *hex, unsigned char *out, size_t out_len) {
    size_t n = strlen(hex);
    if (n % 2 != 0 || n/2 > out_len) return 0;
    for (size_t i = 0; i < n/2; ++i) {
        char c1 = hex[2*i], c2 = hex[2*i+1];
        int v1 = (c1 >= '0' && c1 <= '9') ? c1 - '0' : (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10 : (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10 : -1;
        int v2 = (c2 >= '0' && c2 <= '9') ? c2 - '0' : (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10 : (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10 : -1;
        if (v1 < 0 || v2 < 0) return 0;
        out[i] = (unsigned char)((v1 << 4) | v2);
    }
    return 1;
}

/* Minimal Base58 (Bitcoin alphabet) decoder for test-only usage */
static int b58_decode(const char *b58, unsigned char *out, size_t *out_len) {
    static const char *ALPH = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    size_t n = strlen(b58);
    /* approximate max bytes: log(58^n)/log(256) ~ n*log(58)/log(256) < n */
    unsigned int tmp_cap = (unsigned int)(n * 733 / 1000 + 1); /* ~ log(58)/log(256) */
    unsigned char *tmp = (unsigned char *)calloc(tmp_cap + 1, 1);
    if (!tmp) return 0;
    unsigned int length = 0;
    for (size_t i = 0; i < n; ++i) {
        const char *p = strchr(ALPH, b58[i]);
        if (!p) { free(tmp); return 0; }
        unsigned int carry = (unsigned int)(p - ALPH);
        unsigned int j = 0;
        for (j = 0; j < length; ++j) {
            unsigned int x = (unsigned int)tmp[j] * 58u + carry;
            tmp[j] = (unsigned char)(x & 0xFF);
            carry = x >> 8;
        }
        while (carry > 0) {
            tmp[length++] = (unsigned char)(carry & 0xFF);
            carry >>= 8;
            if (length > tmp_cap) { free(tmp); return 0; }
        }
    }
    /* leading zeros represented by '1' */
    size_t zeros = 0; while (zeros < n && b58[zeros] == '1') zeros++;
    size_t total = zeros + length;
    if (*out_len < total) { free(tmp); return 0; }
    memset(out, 0, zeros);
    for (size_t i = 0; i < length; ++i) out[total - 1 - i] = tmp[i];
    *out_len = total;
    free(tmp);
    return 1;
}

/* Extract 32-byte private key from Base58Check xprv (expects 78-byte payload + 4-byte checksum) */
static int xprv_extract_priv32(const char *xprv, unsigned char out32[32]) {
    unsigned char buf[128]; size_t blen = sizeof(buf);
    if (!b58_decode(xprv, buf, &blen)) return 0;
    if (blen != 82) return 0; /* 78 payload + 4 checksum */
    /* last 33 bytes of payload are: 0x00 + 32-byte key */
    const unsigned char *payload = buf; /* version at [0..3] */
    const unsigned char *key33 = payload + 82 - 4 /*chk*/ - 33;
    if (key33[0] != 0x00) return 0;
    memcpy(out32, key33 + 1, 32);
    return 1;
}

static int test_bip32_vector1(void) {
    /* Test Vector 1 from Bitcoin wiki */
    const char *seed_hex = "000102030405060708090a0b0c0d0e0f";
    unsigned char seed[64];
    memset(seed, 0, sizeof(seed));
    if (!hex2bin(seed_hex, seed, sizeof(seed))) { fprintf(stderr, "seed hex2bin failed\n"); return 1; }

    struct { const char *path_label; uint32_t path[5]; size_t path_len; const char *want_priv_hex; } cases[] = {
        { "m",            { },                    0, "e8f32e723decf4051aefac8e2c93c9c5b214313817cdb01a1494b917c8436b35" },
        { "m/0'",         { 0x80000000u | 0 },    1, "edb2e14f9ee77d26dd93b4ecede8d16ed408ce149b6cd80b0715a2d911a0afea" },
        { "m/0'/1",       { 0x80000000u | 0, 1 }, 2, "3c6cb8d0f6a264c91ea8b5030fadaa8e538b020f0a387421a12de9319dc93368" },
        { "m/0'/1/2'",    { 0x80000000u | 0, 1, 0x80000000u | 2 }, 3, "cbce0d719ecf7431d88e6a89fa1483e02e35092af60c042b1df2ff59fa424dca" },
        { "m/0'/1/2'/2",  { 0x80000000u | 0, 1, 0x80000000u | 2, 2 }, 4, "0f479245fb19a38a1954c5c7c0ebab2f9bdfd96a17563ef28a6a4b1a2a764ef4" },
        { "m/0'/1/2'/2/1000000000", { 0x80000000u | 0, 1, 0x80000000u | 2, 2, 1000000000u }, 5, "471b76e389e528d6de6d816857e012c5455051cad6660850e58372a6c3e6e7c8" },
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        unsigned char out32[32];
        if (!nostr_bip32_priv_from_master_seed(seed, 16, cases[i].path, cases[i].path_len, out32)) {
            fprintf(stderr, "bip32 derive failed at %s\n", cases[i].path_label);
            return 1;
        }
        char hex[65];
        static const char *hexchars = "0123456789abcdef";
        for (int j = 0; j < 32; ++j) { hex[2*j] = hexchars[(out32[j]>>4)&0xF]; hex[2*j+1] = hexchars[out32[j]&0xF]; }
        hex[64] = '\0';
        if (strcmp(hex, cases[i].want_priv_hex) != 0) {
            fprintf(stderr, "bip32 vector mismatch at %s\n got:  %s\n want: %s\n", cases[i].path_label, hex, cases[i].want_priv_hex);
            return 1;
        }
    }
    return 0;
}

static int test_bip32_vector2(void) {
    /* Test Vector 2 from Bitcoin wiki */
    const char *seed_hex = "fffcf9f6f3f0edeae7e4e1dedbd8d5d2cfccc9c6c3c0bdbab7b4b1aeaba8a5a29f9c999693908d8a8784817e7b7875726f6c696663605d5a5754514e4b484542";
    unsigned char seed[128];
    memset(seed, 0, sizeof(seed));
    if (!hex2bin(seed_hex, seed, sizeof(seed))) { fprintf(stderr, "seed2 hex2bin failed\n"); return 1; }

    struct { const char *path_label; uint32_t path[5]; size_t path_len; const char *want_priv_hex; } cases[] = {
        { "m",            { },                    0, "4b03d6fc340455b363f51020ad3ecca4f0850280cf436c70c727923f6db46c3e" },
        { "m/0",          { 0 },                  1, "abe74a98f6c7eabee0428f53798f0ab8aa1bd37873999041703c742f15ac7e1e" },
        { "m/0/2147483647'", { 0, 0x80000000u | 2147483647u }, 2, "877c779ad9687164e9c2f4f0f4ff0340814392330693ce95a58fe18fd52e6e93" },
        { "m/0/2147483647'/1", { 0, 0x80000000u | 2147483647u, 1 }, 3, "704addf544a06e5ee4bea37098463c23613da32020d604506da8c0518e1da4b7" },
        { "m/0/2147483647'/1/2147483646'", { 0, 0x80000000u | 2147483647u, 1, 0x80000000u | 2147483646u }, 4, "f1c7c871a54a804afe328b4c83a1c33b8e5ff48f5087273f04efa83b247d6a2d" },
        { "m/0/2147483647'/1/2147483646'/2", { 0, 0x80000000u | 2147483647u, 1, 0x80000000u | 2147483646u, 2 }, 5, "bb7d39bdb83ecf58f2fd82b6d918341cbef428661ef01ab97c28a4842125ac23" },
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        unsigned char out32[32];
        if (!nostr_bip32_priv_from_master_seed(seed, 64, cases[i].path, cases[i].path_len, out32)) {
            fprintf(stderr, "bip32 derive failed at %s (vec2)\n", cases[i].path_label);
            return 1;
        }
        char hex[65];
        static const char *hexchars = "0123456789abcdef";
        for (int j = 0; j < 32; ++j) { hex[2*j] = hexchars[(out32[j]>>4)&0xF]; hex[2*j+1] = hexchars[out32[j]&0xF]; }
        hex[64] = '\0';
        if (strcmp(hex, cases[i].want_priv_hex) != 0) {
            fprintf(stderr, "bip32 vector2 mismatch at %s\n got:  %s\n want: %s\n", cases[i].path_label, hex, cases[i].want_priv_hex);
            return 1;
        }
    }
    return 0;
}

static int test_bip32_vector3(void) {
    /* Test Vector 3 (leading zeros) from BIP-32 mediawiki */
    const char *seed_hex = "4b381541583be4423346c643850da4b320e46a87ae3d2a4e6da11eba819cd4acba45d239319ac14f863b8d5ab5a0d0c64d2e8a1e7d1457df2e5a3c51c73235be";
    unsigned char seed[128];
    memset(seed, 0, sizeof(seed));
    if (!hex2bin(seed_hex, seed, sizeof(seed))) { fprintf(stderr, "seed3 hex2bin failed\n"); return 1; }

    struct { const char *path_label; uint32_t path[2]; size_t path_len; const char *xprv; } cases[] = {
        { "m",            { }, 0, "xprv9s21ZrQH143K25QhxbucbDDuQ4naNntJRi4KUfWT7xo4EKsHt2QJDu7KXp1A3u7Bi1j8ph3EGsZ9Xvz9dGuVrtHHs7pXeTzjuxBrCmmhgC6" },
        { "m/0'",         { 0x80000000u | 0 }, 1, "xprv9uPDJpEQgRQfDcW7BkF7eTya6RPxXeJCqCJGHuCJ4GiRVLzkTXBAJMu2qaMWPrS7AANYqdq6vcBcBUdJCVVFceUvJFjaPdGZ2y9WACViL4L" },
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        unsigned char out32[32];
        if (!nostr_bip32_priv_from_master_seed(seed, 64, cases[i].path, cases[i].path_len, out32)) {
            fprintf(stderr, "bip32 derive failed at %s (vec3)\n", cases[i].path_label);
            return 1;
        }
        unsigned char want32[32];
        if (!xprv_extract_priv32(cases[i].xprv, want32)) {
            fprintf(stderr, "xprv decode failed at %s (vec3)\n", cases[i].path_label);
            return 1;
        }
        if (memcmp(out32, want32, 32) != 0) {
            fprintf(stderr, "bip32 vector3 mismatch at %s\n", cases[i].path_label);
            return 1;
        }
    }
    return 0;
}

int run_bip32_vectors(void) {
    /* BIP-32 vectors */
    int rc = 0;
    rc |= test_bip32_vector1();
    rc |= test_bip32_vector2();
    rc |= test_bip32_vector3();
    return rc;
}

/* no main() here; linked into test_nip06_roundtrip main */
