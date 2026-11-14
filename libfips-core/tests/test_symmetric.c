#include <fips/noise/symmetric.h>
#include <fips/identity/local.h>
#include "../src/noise/hkdf.h"
#include "fakes/host.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

/* Pinned by python3 -I tests/tools/noise_symmetric_vectors.py: hashlib/hmac,
 * plus the affine secp256k1 reference in ecdh_vectors.py for the seeded key. */
static const char responder_static_hex[] =
    "02cd759a0e0fc62db2e12056a21f7bef027921df768ce3eb35db923b415fe83432";
static const char ik_h0_hex[] = "15bea16f93969bcae868e128bcbdc110b1d88a5306b367632171a9aaaad3f533";
static const char xk_h0_hex[] = "2640f52eebcd9e882958951c794250eedb28002c05d7dc2ea0f195406042caf1";
static const char padded_h0_hex[] = "4e6f6973655f4e4e5f32353531395f436861436861506f6c795f534841323536";
static const char short_h0_hex[] = "4e6f6973655f4e4e000000000000000000000000000000000000000000000000";
static const char long_h0_hex[] = "dc4e9dfb6ebc921cd142891c534c6300315f66ff790aff1622470447bf0e69a9";
static const char ik_h1_hex[] = "6e8f93a78e7d1c3e817b474a58d5992421236e0359d603fda70227dcaa7b79e3";
static const char ik_h2_hex[] = "97883e8c47d85495aa9de0fe3bd080eeff70b74bb95297dc51b31f3754b6d5c3";
static const char ik_ck1_hex[] = "0c36136761fc2736e2faf0a7cb4549fd6176e53079261ffbefc6320173524fcb";
static const char ik_k1_hex[] = "f5aa6c8f0e053bddfad7fd82717920d887e30c1b2c70ef11485ffd42bc9578f8";
static const char ik_h3_hex[] = "def78f798cc495bfda963c79e54ce79131f0fb7e8a823f07a2cc37083905846b";
static const char ik_ck2_hex[] = "d33cf19ff485fba533cde3c66430ce53fade10ec36321f91a61b5548f859f0e9";
static const char ik_k2_hex[] = "1404d32b5404f8afc518afa566a1e69b2c212a3864b9a0f856566054c98bcd60";
static const char ik_split1_hex[] = "c72d2b0302a778454d26f5b3f9d163c7bf6a841e3b02fbaecdf9f2e6a2c3fe02";
static const char ik_split2_hex[] = "d06ed94c09251793b4990189e89d57b8a171b5cc30998ddd773f09b2f7e11a12";
static const char xk_ck1_hex[] = "43fe4f01aeafad9199380d4df402f21ff8f592075934c0d84da680bfc60fb402";
static const char xk_k1_hex[] = "41af47e8cf6453995c3119f07df25cafc85752157c8d86fe06e0b40574852583";
static const char xk_split1_hex[] = "3b871326d888f2af56175f2d49e74b010703f9be99afb3955867c864184ace29";
static const char xk_split2_hex[] = "6f04962e4b638c216cf451a0d37e5840b78896fbb967a41141b47382a28bf258";
/* tests/vectors/noise.json cases.patterns[].steps[0] ("constructed", initiator
 * == responder): new_initiator/new_responder mix the <- s pre-message, the
 * responder's static key (seeded_keypair(1)) with its prefix forced to 0x02,
 * into SHA-256(name). */
static const char ik_constructed_hex[] = "c4f4d7e468c992772c7227ff713db3f384364eb3a7cf1996e39568e0dd51c89d";
static const char xk_constructed_hex[] = "d26750f4eafd9d9d10b10f07ec287e2ffdd33f0df19eeefe3d27168b7e5fb9b0";

static const uint8_t ik_name[] = FIPS_NOISE_PROTOCOL_NAME_IK;
static const uint8_t xk_name[] = FIPS_NOISE_PROTOCOL_NAME_XK;
static const uint8_t prologue[] = "fips-symmetric/prologue";

static int unhex(const char *hex, uint8_t *out, size_t len)
{
    CHECK(strlen(hex) == 2 * len);
    CHECK(sodium_hex2bin(out, len, hex, 2 * len, NULL, NULL, NULL) == 0);
    return 0;
}

static int is_hex(const uint8_t *buf, size_t len, const char *hex)
{
    uint8_t expected[128];
    CHECK(len <= sizeof(expected));
    CHECK(unhex(hex, expected, len) == 0);
    CHECK(memcmp(buf, expected, len) == 0);
    return 0;
}

static int filled(const uint8_t *buf, size_t len, uint8_t byte)
{
    for (size_t i = 0; i < len; ++i) if (buf[i] != byte) return 0;
    return 1;
}

static int empty_cipher(const fips_symmetric_state *s)
{
    CHECK(!s->cipher.has_key && s->cipher.nonce == 0);
    CHECK(filled(s->cipher.key.bytes, sizeof(s->cipher.key.bytes), 0));
    return 0;
}

static int same(const fips_symmetric_state *a, const fips_symmetric_state *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static int hmac_case(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                     const char *mac_hex)
{
    uint8_t mac[32];
    noise_hmac_sha256(key, key_len, data, len, mac);
    CHECK(is_hex(mac, sizeof(mac), mac_hex) == 0);
    return 0;
}

/* RFC 4231 sections 4.2-4.5. */
static int rfc4231(void)
{
    uint8_t key[25], data[50];
    memset(key, 0x0b, 20);
    CHECK(hmac_case(key, 20, (const uint8_t *)"Hi There", 8,
                    "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") == 0);
    CHECK(hmac_case((const uint8_t *)"Jefe", 4,
                    (const uint8_t *)"what do ya want for nothing?", 28,
                    "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);
    memset(key, 0xaa, 20);
    memset(data, 0xdd, sizeof(data));
    CHECK(hmac_case(key, 20, data, sizeof(data),
                    "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe") == 0);
    for (unsigned i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(i + 1);
    memset(data, 0xcd, sizeof(data));
    CHECK(hmac_case(key, sizeof(key), data, sizeof(data),
                    "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b") == 0);
    return 0;
}

/* RFC 5869 appendix A.1 and A.2; A.2's 80-byte salt takes HMAC's hashed-key
 * path and its 82-byte output needs a third block. */
static int rfc5869(void)
{
    uint8_t ikm[80], salt[80], info[80], prk[32], okm[82], ck[32], out[64], direct[64];
    memset(ikm, 0x0b, 22);
    for (unsigned i = 0; i < 13; ++i) salt[i] = (uint8_t)i;
    for (unsigned i = 0; i < 10; ++i) info[i] = (uint8_t)(0xf0 + i);
    noise_hkdf_extract(salt, 13, ikm, 22, prk);
    CHECK(is_hex(prk, 32, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5") == 0);
    noise_hkdf_expand(prk, info, 10, okm, 42);
    CHECK(is_hex(okm, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                          "34007208d5b887185865") == 0);
    for (unsigned i = 0; i < 80; ++i) {
        ikm[i] = (uint8_t)i;
        salt[i] = (uint8_t)(0x60 + i);
        info[i] = (uint8_t)(0xb0 + i);
    }
    noise_hkdf_extract(salt, 80, ikm, 80, prk);
    CHECK(is_hex(prk, 32, "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244") == 0);
    noise_hkdf_expand(prk, info, 80, okm, 82);
    CHECK(is_hex(okm, 82, "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
                          "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
                          "cc30c58179ec3e87c14c01d5c1f3434f1d87") == 0);
    /* The Noise two-output form is extract then a 64-byte expand with empty info. */
    memset(ck, 0x5a, sizeof(ck));
    noise_hkdf2(ck, ikm, 80, out);
    noise_hkdf_extract(ck, sizeof(ck), ikm, 80, prk);
    noise_hkdf_expand(prk, NULL, 0, direct, sizeof(direct));
    CHECK(memcmp(out, direct, sizeof(out)) == 0);
    return 0;
}

static int protocol_names(void)
{
    static const uint8_t padded[] = "Noise_NN_25519_ChaChaPoly_SHA256";
    static const uint8_t short_name[] = "Noise_NN";
    static const uint8_t long_name[] =
        "Noise_IKpsk2_secp256k1_ChaChaPoly_SHA256_fips-vectors-long-name!!";
    fips_symmetric_state s;
    uint8_t digest[32];
    CHECK(sizeof(ik_name) - 1 == FIPS_NOISE_PROTOCOL_NAME_SIZE);
    CHECK(sizeof(xk_name) - 1 == FIPS_NOISE_PROTOCOL_NAME_SIZE);
    CHECK(sizeof(padded) - 1 == 32 && sizeof(short_name) - 1 == 8 && sizeof(long_name) - 1 == 65);
    memset(&s, 0xa5, sizeof(s));
    fips_symmetric_init(&s, ik_name, sizeof(ik_name) - 1);
    CHECK(is_hex(s.h, 32, ik_h0_hex) == 0);
    CHECK(memcmp(s.ck, s.h, 32) == 0);
    CHECK(empty_cipher(&s) == 0);
    crypto_hash_sha256(digest, ik_name, sizeof(ik_name) - 1);
    CHECK(memcmp(s.h, digest, 32) == 0);
    fips_symmetric_init(&s, xk_name, sizeof(xk_name) - 1);
    CHECK(is_hex(s.h, 32, xk_h0_hex) == 0);
    CHECK(memcmp(s.ck, s.h, 32) == 0);
    CHECK(empty_cipher(&s) == 0);
    /* 32 bytes is used verbatim, shorter is zero padded, longer is hashed. */
    fips_symmetric_init(&s, padded, 32);
    CHECK(memcmp(s.h, padded, 32) == 0);
    CHECK(is_hex(s.h, 32, padded_h0_hex) == 0);
    fips_symmetric_init(&s, short_name, 8);
    CHECK(memcmp(s.h, short_name, 8) == 0 && filled(s.h + 8, 24, 0));
    CHECK(is_hex(s.h, 32, short_h0_hex) == 0);
    fips_symmetric_init(&s, long_name, 65);
    CHECK(is_hex(s.h, 32, long_h0_hex) == 0);
    crypto_hash_sha256(digest, long_name, 65);
    CHECK(memcmp(s.h, digest, 32) == 0);
    CHECK(memcmp(s.ck, s.h, 32) == 0);
    /* An empty name over a keyed state leaves zeros and an empty cipher. */
    memset(&s, 0xa5, sizeof(s));
    fips_symmetric_init(&s, NULL, 0);
    CHECK(filled(s.h, 32, 0) && filled(s.ck, 32, 0));
    CHECK(empty_cipher(&s) == 0);
    fips_symmetric_destroy(&s);
    return 0;
}

/* Both noise.json patterns: SHA-256(name) alone is not the recorded value;
 * the constructors mix the normalized responder static key first. */
static int constructed(void)
{
    fips_fixed_entropy fixed = {0x5a};
    fips_entropy entropy = fips_fixed_entropy_adapter(&fixed);
    /* tools/fips-vectors seeded_keypair(1): xor the keypair domain, then SplitMix64. */
    fips_seeded_entropy seeded = {UINT64_C(1) ^ UINT64_C(0x6b65797061697273)};
    fips_entropy keyed = fips_seeded_entropy_adapter(&seeded);
    fips_secp *ctx = NULL;
    fips_identity *responder = NULL;
    fips_symmetric_state ik, xk;
    uint8_t secret[32], pub[33], expected[32];
    keyed.fill(keyed.ctx, secret, sizeof(secret));
    CHECK(fips_identity_init_secp(entropy, &ctx) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_from_secret_bytes(ctx, entropy, secret, &responder) == FIPS_IDENTITY_OK);
    fips_identity_pubkey_full(responder, pub);
    fips_identity_destroy(responder);
    fips_identity_destroy_secp(ctx);
    sodium_memzero(secret, sizeof(secret));
    CHECK(is_hex(pub, sizeof(pub), responder_static_hex) == 0);
    pub[0] = 0x02; /* normalize_for_premessage: even parity regardless of the key. */
    CHECK(unhex(ik_constructed_hex, expected, sizeof(expected)) == 0);
    fips_symmetric_init(&ik, ik_name, sizeof(ik_name) - 1);
    CHECK(memcmp(ik.h, expected, sizeof(expected)) != 0); /* Not SHA-256(name) alone. */
    fips_symmetric_mix_hash(&ik, pub, sizeof(pub));
    CHECK(memcmp(ik.h, expected, sizeof(expected)) == 0);
    CHECK(is_hex(ik.ck, 32, ik_h0_hex) == 0); /* MixHash leaves ck alone. */
    CHECK(empty_cipher(&ik) == 0);
    fips_symmetric_init(&xk, xk_name, sizeof(xk_name) - 1);
    fips_symmetric_mix_hash(&xk, pub, sizeof(pub));
    CHECK(is_hex(xk.h, 32, xk_constructed_hex) == 0);
    CHECK(is_hex(xk.ck, 32, xk_h0_hex) == 0);
    fips_symmetric_destroy(&ik);
    fips_symmetric_destroy(&xk);
    return 0;
}

static int pinned_sequence(void)
{
    fips_symmetric_state s, before;
    fips_aead_key k1, k2;
    uint8_t ikm[32], pubkey[33];
    for (unsigned i = 0; i < sizeof(ikm); ++i) ikm[i] = (uint8_t)i;
    memset(pubkey, 0xa5, sizeof(pubkey));
    pubkey[0] = 0x02;
    fips_symmetric_init(&s, ik_name, sizeof(ik_name) - 1);
    fips_symmetric_mix_hash(&s, prologue, sizeof(prologue) - 1);
    CHECK(is_hex(s.h, 32, ik_h1_hex) == 0);
    fips_symmetric_mix_hash(&s, pubkey, sizeof(pubkey));
    CHECK(is_hex(s.h, 32, ik_h2_hex) == 0);
    CHECK(is_hex(s.ck, 32, ik_h0_hex) == 0);
    CHECK(fips_symmetric_mix_key(&s, ikm, sizeof(ikm)) == FIPS_SYMMETRIC_OK);
    CHECK(is_hex(s.ck, 32, ik_ck1_hex) == 0);
    CHECK(is_hex(s.cipher.key.bytes, 32, ik_k1_hex) == 0);
    CHECK(s.cipher.has_key && s.cipher.nonce == 0);
    CHECK(is_hex(s.h, 32, ik_h2_hex) == 0); /* MixKey leaves h alone. */
    fips_symmetric_mix_hash(&s, NULL, 0);
    CHECK(is_hex(s.h, 32, ik_h3_hex) == 0);
    s.cipher.nonce = 5; /* A new key restarts the counter. */
    memset(ikm, 0xff, sizeof(ikm));
    CHECK(fips_symmetric_mix_key(&s, ikm, sizeof(ikm)) == FIPS_SYMMETRIC_OK);
    CHECK(is_hex(s.ck, 32, ik_ck2_hex) == 0);
    CHECK(is_hex(s.cipher.key.bytes, 32, ik_k2_hex) == 0);
    CHECK(s.cipher.has_key && s.cipher.nonce == 0);
    CHECK(is_hex(s.h, 32, ik_h3_hex) == 0);
    fips_symmetric_copy(&before, &s);
    CHECK(fips_symmetric_split(&s, &k1, &k2) == FIPS_SYMMETRIC_OK);
    CHECK(same(&s, &before));
    CHECK(is_hex(k1.bytes, 32, ik_split1_hex) == 0);
    CHECK(is_hex(k2.bytes, 32, ik_split2_hex) == 0);
    fips_aead_key_destroy(&k1);
    fips_aead_key_destroy(&k2);

    for (unsigned i = 0; i < sizeof(ikm); ++i) ikm[i] = (uint8_t)i;
    fips_symmetric_init(&s, xk_name, sizeof(xk_name) - 1);
    CHECK(fips_symmetric_mix_key(&s, ikm, sizeof(ikm)) == FIPS_SYMMETRIC_OK);
    CHECK(is_hex(s.ck, 32, xk_ck1_hex) == 0);
    CHECK(is_hex(s.cipher.key.bytes, 32, xk_k1_hex) == 0);
    CHECK(is_hex(s.h, 32, xk_h0_hex) == 0);
    CHECK(fips_symmetric_split(&s, &k1, &k2) == FIPS_SYMMETRIC_OK);
    CHECK(is_hex(k1.bytes, 32, xk_split1_hex) == 0);
    CHECK(is_hex(k2.bytes, 32, xk_split2_hex) == 0);
    fips_aead_key_destroy(&k1);
    fips_aead_key_destroy(&k2);
    fips_symmetric_destroy(&s);
    fips_symmetric_destroy(&before);
    return 0;
}

/* Two sides that agree on the transcript so far, keyed. */
static int keyed_pair(fips_symmetric_state *a, fips_symmetric_state *b)
{
    uint8_t ikm[32];
    memset(ikm, 0x42, sizeof(ikm));
    fips_symmetric_init(a, ik_name, sizeof(ik_name) - 1);
    fips_symmetric_mix_hash(a, prologue, sizeof(prologue) - 1);
    CHECK(fips_symmetric_mix_key(a, ikm, sizeof(ikm)) == FIPS_SYMMETRIC_OK);
    fips_symmetric_copy(b, a);
    CHECK(same(a, b) && a->cipher.has_key);
    return 0;
}

static int hashed(const uint8_t h[32], const uint8_t *data, size_t len, uint8_t out[32])
{
    crypto_hash_sha256_state st;
    CHECK(crypto_hash_sha256_init(&st) == 0);
    CHECK(crypto_hash_sha256_update(&st, h, 32) == 0);
    CHECK(crypto_hash_sha256_update(&st, data, len) == 0);
    CHECK(crypto_hash_sha256_final(&st, out) == 0);
    return 0;
}

static int roundtrip(void)
{
    fips_symmetric_state a, b, before;
    uint8_t plain[40], wire[80], wire2[56], out[80], expect[32];
    size_t got, n;
    for (unsigned i = 0; i < sizeof(plain); ++i) plain[i] = (uint8_t)(i * 7);

    /* Without a key both directions pass the bytes through and hash them. */
    fips_symmetric_init(&a, ik_name, sizeof(ik_name) - 1);
    fips_symmetric_copy(&b, &a);
    fips_symmetric_copy(&before, &a);
    got = SIZE_MAX;
    CHECK(fips_symmetric_encrypt_and_hash(&a, plain, sizeof(plain), wire, sizeof(wire), &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == sizeof(plain) && memcmp(wire, plain, sizeof(plain)) == 0);
    CHECK(fips_symmetric_decrypt_and_hash(&b, wire, got, out, sizeof(out), &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == sizeof(plain) && memcmp(out, plain, sizeof(plain)) == 0);
    CHECK(same(&a, &b) && empty_cipher(&a) == 0);
    CHECK(hashed(before.h, plain, sizeof(plain), expect) == 0);
    CHECK(memcmp(a.h, expect, 32) == 0 && memcmp(a.ck, before.ck, 32) == 0);
    fips_symmetric_copy(&before, &a);
    got = SIZE_MAX;
    CHECK(fips_symmetric_encrypt_and_hash(&a, plain, sizeof(plain), wire, sizeof(plain) - 1, &got) == FIPS_SYMMETRIC_ERR_RANGE);
    CHECK(fips_symmetric_decrypt_and_hash(&a, plain, sizeof(plain), wire, sizeof(plain) - 1, &got) == FIPS_SYMMETRIC_ERR_RANGE);
    CHECK(same(&a, &before) && got == SIZE_MAX);

    /* Keyed: counter 0, empty associated data, then h = SHA-256(h || ciphertext). */
    CHECK(keyed_pair(&a, &b) == 0);
    fips_symmetric_copy(&before, &a);
    got = SIZE_MAX;
    CHECK(fips_symmetric_encrypt_and_hash(&a, plain, sizeof(plain), wire, sizeof(wire), &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == sizeof(plain) + FIPS_NOISE_TAG_SIZE && a.cipher.nonce == 1);
    CHECK(memcmp(wire, plain, sizeof(plain)) != 0);
    CHECK(hashed(before.h, wire, got, expect) == 0);
    CHECK(memcmp(a.h, expect, 32) == 0);
    CHECK(memcmp(a.ck, before.ck, 32) == 0 && memcmp(a.cipher.key.bytes, before.cipher.key.bytes, 32) == 0);
    CHECK(fips_aead_open(&b.cipher.key, 0, NULL, 0, wire, got, out, sizeof(out), &n) == FIPS_AEAD_OK);
    CHECK(n == sizeof(plain) && memcmp(out, plain, sizeof(plain)) == 0);
    memset(out, 0xa5, sizeof(out));
    n = SIZE_MAX;
    CHECK(fips_symmetric_decrypt_and_hash(&b, wire, got, out, sizeof(out), &n) == FIPS_SYMMETRIC_OK);
    CHECK(n == sizeof(plain) && memcmp(out, plain, sizeof(plain)) == 0);
    CHECK(filled(out + n, sizeof(out) - n, 0xa5));
    CHECK(same(&a, &b));
    /* A second message takes counter 1 and differs for the same plaintext. */
    CHECK(fips_symmetric_encrypt_and_hash(&a, plain, sizeof(plain), wire2, sizeof(wire2), &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == sizeof(wire2) && a.cipher.nonce == 2 && memcmp(wire2, wire, sizeof(wire2)) != 0);
    CHECK(fips_symmetric_decrypt_and_hash(&b, wire2, got, out, sizeof(plain), &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == sizeof(plain) && memcmp(out, plain, sizeof(plain)) == 0 && same(&a, &b));
    /* An empty payload is a lone tag. */
    CHECK(fips_symmetric_encrypt_and_hash(&a, NULL, 0, wire, FIPS_NOISE_TAG_SIZE, &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == FIPS_NOISE_TAG_SIZE);
    CHECK(fips_symmetric_decrypt_and_hash(&b, wire, got, out, 0, &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == 0 && same(&a, &b));
    /* Exact in-place calls in both directions. */
    memcpy(wire, plain, sizeof(plain));
    CHECK(fips_symmetric_encrypt_and_hash(&a, wire, sizeof(plain), wire, sizeof(wire), &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == sizeof(plain) + FIPS_NOISE_TAG_SIZE && memcmp(wire, plain, sizeof(plain)) != 0);
    CHECK(fips_symmetric_decrypt_and_hash(&b, wire, got, wire, sizeof(wire), &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == sizeof(plain) && memcmp(wire, plain, sizeof(plain)) == 0 && same(&a, &b));
    /* Range errors change nothing and consume no counter. */
    fips_symmetric_copy(&before, &a);
    got = SIZE_MAX;
    CHECK(fips_symmetric_encrypt_and_hash(&a, plain, sizeof(plain), wire, sizeof(plain) + FIPS_NOISE_TAG_SIZE - 1, &got) == FIPS_SYMMETRIC_ERR_RANGE);
    CHECK(fips_symmetric_encrypt_and_hash(&a, plain, FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE + 1, wire, SIZE_MAX, &got) == FIPS_SYMMETRIC_ERR_RANGE);
    CHECK(fips_symmetric_decrypt_and_hash(&a, wire, FIPS_NOISE_TAG_SIZE - 1, out, sizeof(out), &got) == FIPS_SYMMETRIC_ERR_RANGE);
    CHECK(fips_symmetric_decrypt_and_hash(&a, wire, FIPS_NOISE_MAX_MESSAGE_SIZE + 1, out, SIZE_MAX, &got) == FIPS_SYMMETRIC_ERR_RANGE);
    CHECK(fips_symmetric_decrypt_and_hash(&a, wire, sizeof(plain) + FIPS_NOISE_TAG_SIZE, out, sizeof(plain) - 1, &got) == FIPS_SYMMETRIC_ERR_RANGE);
    CHECK(same(&a, &before) && got == SIZE_MAX);
    /* Nonce overflow is refused before any work. */
    a.cipher.nonce = UINT64_MAX;
    fips_symmetric_copy(&before, &a);
    CHECK(fips_symmetric_encrypt_and_hash(&a, plain, sizeof(plain), wire, sizeof(wire), &got) == FIPS_SYMMETRIC_ERR_NONCE_OVERFLOW);
    CHECK(fips_symmetric_decrypt_and_hash(&a, wire, sizeof(wire), out, sizeof(out), &got) == FIPS_SYMMETRIC_ERR_NONCE_OVERFLOW);
    CHECK(same(&a, &before) && got == SIZE_MAX);
    fips_symmetric_destroy(&a);
    fips_symmetric_destroy(&b);
    fips_symmetric_destroy(&before);
    return 0;
}

/* A failed read: h untouched, counter consumed (upstream advances before open),
 * plaintext region zeroed. The read runs on a fork; the original still reads
 * the genuine message. */
static int tamper(void)
{
    fips_symmetric_state a, b, fork, other;
    uint8_t plain[24], wire[40], bad[40], out[40], ikm[32];
    size_t got;
    for (unsigned i = 0; i < sizeof(plain); ++i) plain[i] = (uint8_t)(0x80 + i);
    CHECK(keyed_pair(&a, &b) == 0);
    CHECK(fips_symmetric_encrypt_and_hash(&a, plain, sizeof(plain), wire, sizeof(wire), &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == sizeof(wire));
    for (unsigned which = 0; which < 4; ++which) {
        fips_symmetric_state *reader = &fork;
        size_t len = sizeof(wire);
        fips_symmetric_copy(&fork, &b);
        memcpy(bad, wire, sizeof(wire));
        if (which == 0) bad[0] ^= 1;
        if (which == 1) bad[sizeof(wire) - 1] ^= 1;
        if (which == 2) len -= 1; /* Truncated, still at least a tag. */
        if (which == 3) { /* Wrong key. */
            memset(ikm, 0x43, sizeof(ikm));
            fips_symmetric_init(&other, ik_name, sizeof(ik_name) - 1);
            CHECK(fips_symmetric_mix_key(&other, ikm, sizeof(ikm)) == FIPS_SYMMETRIC_OK);
            reader = &other;
        }
        memset(out, 0xa5, sizeof(out));
        got = SIZE_MAX;
        CHECK(fips_symmetric_decrypt_and_hash(reader, bad, len, out, sizeof(out), &got) == FIPS_SYMMETRIC_ERR_AUTH);
        CHECK(got == SIZE_MAX);
        CHECK(filled(out, len - FIPS_NOISE_TAG_SIZE, 0));
        CHECK(filled(out + len - FIPS_NOISE_TAG_SIZE, sizeof(out) - (len - FIPS_NOISE_TAG_SIZE), 0xa5));
        if (which == 3) {
            CHECK(other.cipher.nonce == 1);
            fips_symmetric_destroy(&other);
            continue;
        }
        CHECK(memcmp(fork.h, b.h, 32) == 0 && memcmp(fork.ck, b.ck, 32) == 0);
        CHECK(memcmp(fork.cipher.key.bytes, b.cipher.key.bytes, 32) == 0);
        CHECK(fork.cipher.nonce == b.cipher.nonce + 1 && fork.cipher.has_key);
        fips_symmetric_destroy(&fork);
    }
    CHECK(fips_symmetric_decrypt_and_hash(&b, wire, sizeof(wire), out, sizeof(out), &got) == FIPS_SYMMETRIC_OK);
    CHECK(got == sizeof(plain) && memcmp(out, plain, sizeof(plain)) == 0 && same(&a, &b));
    /* The same message again fails: its counter has been used. */
    fips_symmetric_copy(&fork, &b);
    CHECK(fips_symmetric_decrypt_and_hash(&fork, wire, sizeof(wire), out, sizeof(out), &got) == FIPS_SYMMETRIC_ERR_AUTH);
    CHECK(fork.cipher.nonce == 2 && memcmp(fork.h, b.h, 32) == 0);
    fips_symmetric_destroy(&a);
    fips_symmetric_destroy(&b);
    fips_symmetric_destroy(&fork);
    return 0;
}

static int split_determinism(void)
{
    fips_symmetric_state a, b, before;
    fips_aead_key k1, k2, k3, k4;
    uint8_t ikm[32];
    CHECK(keyed_pair(&a, &b) == 0);
    fips_symmetric_copy(&before, &a);
    CHECK(fips_symmetric_split(&a, &k1, &k2) == FIPS_SYMMETRIC_OK);
    CHECK(same(&a, &before));
    CHECK(fips_symmetric_split(&b, &k3, &k4) == FIPS_SYMMETRIC_OK);
    CHECK(memcmp(k1.bytes, k3.bytes, 32) == 0 && memcmp(k2.bytes, k4.bytes, 32) == 0);
    CHECK(memcmp(k1.bytes, k2.bytes, 32) != 0);
    CHECK(memcmp(k1.bytes, a.cipher.key.bytes, 32) != 0 && memcmp(k2.bytes, a.cipher.key.bytes, 32) != 0);
    CHECK(memcmp(k1.bytes, a.ck, 32) != 0 && memcmp(k2.bytes, a.ck, 32) != 0);
    /* Aliased outputs are refused before anything is derived or written. */
    memset(k3.bytes, 0xa5, sizeof(k3.bytes));
    CHECK(fips_symmetric_split(&a, &k3, &k3) == FIPS_SYMMETRIC_ERR_RANGE);
    CHECK(filled(k3.bytes, sizeof(k3.bytes), 0xa5) && same(&a, &before));
    /* Split reads ck only: MixHash and the counter do not move it, MixKey does. */
    fips_symmetric_mix_hash(&a, prologue, sizeof(prologue) - 1);
    a.cipher.nonce = 7;
    CHECK(fips_symmetric_split(&a, &k3, &k4) == FIPS_SYMMETRIC_OK);
    CHECK(memcmp(k1.bytes, k3.bytes, 32) == 0 && memcmp(k2.bytes, k4.bytes, 32) == 0);
    memset(ikm, 0x44, sizeof(ikm));
    CHECK(fips_symmetric_mix_key(&a, ikm, sizeof(ikm)) == FIPS_SYMMETRIC_OK);
    CHECK(fips_symmetric_split(&a, &k3, &k4) == FIPS_SYMMETRIC_OK);
    CHECK(memcmp(k1.bytes, k3.bytes, 32) != 0 && memcmp(k2.bytes, k4.bytes, 32) != 0);
    fips_aead_key_destroy(&k1);
    fips_aead_key_destroy(&k2);
    fips_aead_key_destroy(&k3);
    fips_aead_key_destroy(&k4);
    fips_symmetric_destroy(&a);
    fips_symmetric_destroy(&b);
    fips_symmetric_destroy(&before);
    return 0;
}

static int copy_and_destroy(void)
{
    fips_symmetric_state a, b;
    uint8_t wire[32];
    size_t got;
    CHECK(keyed_pair(&a, &b) == 0);
    CHECK(fips_symmetric_encrypt_and_hash(&a, NULL, 0, wire, sizeof(wire), &got) == FIPS_SYMMETRIC_OK);
    CHECK(a.cipher.nonce == 1);
    fips_symmetric_copy(&b, &a);
    CHECK(same(&a, &b));
    fips_symmetric_copy(&a, &a); /* Self-copy is a no-op. */
    CHECK(same(&a, &b));
    /* Copies are independent. */
    b.h[0] ^= 1;
    b.cipher.nonce += 1;
    CHECK(!same(&a, &b) && a.cipher.nonce == 1 && (a.h[0] ^ 1) == b.h[0]);
    CHECK(!filled((const uint8_t *)&a, sizeof(a), 0));
    fips_symmetric_destroy(&a);
    CHECK(filled((const uint8_t *)&a, sizeof(a), 0));
    CHECK(!a.cipher.has_key && a.cipher.nonce == 0);
    fips_symmetric_destroy(&a); /* Idempotent. */
    CHECK(filled((const uint8_t *)&a, sizeof(a), 0));
    fips_symmetric_destroy(&b);
    CHECK(filled((const uint8_t *)&b, sizeof(b), 0));
    return 0;
}

int main(void)
{
    CHECK(sodium_init() >= 0);
    CHECK(rfc4231() == 0);
    CHECK(rfc5869() == 0);
    CHECK(protocol_names() == 0);
    CHECK(constructed() == 0);
    CHECK(pinned_sequence() == 0);
    CHECK(roundtrip() == 0);
    CHECK(tamper() == 0);
    CHECK(split_determinism() == 0);
    CHECK(copy_and_destroy() == 0);
    puts("SymmetricState: RFC 4231/5869, protocol names, noise.json constructed h (IK/XK), "
         "pinned MixKey/MixHash/Split, roundtrip, tamper/fork, split, copy/destroy passed");
    return 0;
}
