#include <fips/noise/ecdh.h>
#include <fips/identity/local.h>
#include "fakes/host.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

/* Independent affine-Python oracle: python3 -I tests/tools/ecdh_vectors.py.
 * Pairs: seeded(0, 0123456789abcdef), seeded(0123456789abcdef, ffffffffffffffff),
 * seeded(ffffffffffffffff, 0), and fixed scalars (1, 2). */
static const struct { const char *x; const char *hash; } vectors[] = {
    {"87a885da2e9b55e676418bcec8ce14f4c3cb0f4e412f5d27075cb06f9cc71321",
     "9ac03def6b11f20d220bc5176c430dc1ef8bd0580cabcc3efaa2a052a2cf54dd"},
    {"626e83e9cbcca1cca2e814383f85d9051a52bae2a6582b9e40ea6c3298593965",
     "de8b8deddc56236b042684a146d08050622805583984075cbbd9461dcfab7d7d"},
    {"b750fe91050744fb1cd2b800995a6d46b536a80d8f90ebeab625a6a2d4675aed",
     "1430ba928b12a915a91a6af58a213a87be1ef4bcf2a3033609a8c7c3ee13fc3a"},
    {"c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5",
     "0135da2f8acf7b9e3090939432e47684eb888ea38c2173054d4eedffdf152ca5"}
};

static int unhex(const char *hex, uint8_t out[32])
{
    CHECK(sodium_hex2bin(out, 32, hex, 64, NULL, NULL, NULL) == 0);
    return 0;
}

static int dh(const fips_secp *ctx, const uint8_t secret[32],
              const uint8_t public_key[33], uint8_t out[32],
              fips_identity_status expected)
{
    uint64_t before = fips_noise_dh_ops();
    CHECK(fips_noise_ecdh(ctx, secret, public_key, out) == expected);
    CHECK(fips_noise_dh_ops() == before + 1);
    return 0;
}

static int vector(const fips_secp *ctx, fips_entropy entropy,
                  const uint8_t a[32], const uint8_t b[32], size_t i)
{
    fips_identity *alice = NULL, *bob = NULL;
    uint8_t A[33], B[33], negated[33], out[32], reverse[32], x[32], expected[32];
    CHECK(fips_identity_from_secret_bytes(ctx, entropy, a, &alice) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_from_secret_bytes(ctx, entropy, b, &bob) == FIPS_IDENTITY_OK);
    fips_identity_pubkey_full(alice, A);
    fips_identity_pubkey_full(bob, B);
    CHECK(unhex(vectors[i].x, x) == 0);
    CHECK(unhex(vectors[i].hash, expected) == 0);
    /* Pin both x and SHA-256(x), not SHA-256(compressed shared point). */
    CHECK(crypto_hash_sha256(out, x, sizeof(x)) == 0);
    CHECK(memcmp(out, expected, 32) == 0);
    CHECK(dh(ctx, a, B, out, FIPS_IDENTITY_OK) == 0);
    CHECK(memcmp(out, expected, 32) == 0);
    CHECK(dh(ctx, b, A, reverse, FIPS_IDENTITY_OK) == 0);
    CHECK(memcmp(out, reverse, 32) == 0);
    /* Multiplication by -B negates the shared y but must not affect DH. */
    memcpy(negated, B, 33);
    negated[0] ^= 1;
    CHECK(dh(ctx, a, negated, reverse, FIPS_IDENTITY_OK) == 0);
    CHECK(memcmp(out, reverse, 32) == 0);
    memcpy(negated, A, 33);
    negated[0] ^= 1;
    CHECK(dh(ctx, b, negated, reverse, FIPS_IDENTITY_OK) == 0);
    CHECK(memcmp(out, reverse, 32) == 0);
    fips_identity_destroy(alice);
    fips_identity_destroy(bob);
    sodium_memzero(out, sizeof(out));
    sodium_memzero(reverse, sizeof(reverse));
    sodium_memzero(x, sizeof(x));
    sodium_memzero(expected, sizeof(expected));
    return 0;
}

static int rejected(const fips_secp *ctx, const uint8_t secret[32],
                    const uint8_t public_key[33])
{
    uint8_t out[32], before[32];
    memset(out, 0xa5, sizeof(out));
    memcpy(before, out, sizeof(out));
    CHECK(dh(ctx, secret, public_key, out, FIPS_IDENTITY_InvalidSecretKey) == 0);
    CHECK(memcmp(out, before, sizeof(out)) == 0);
    return 0;
}

static int invalid_keys(const fips_secp *ctx, fips_entropy entropy)
{
    uint8_t secret[32] = {0}, pub[33], bad[33], order[32];
    secret[31] = 1;
    fips_identity *id = NULL;
    CHECK(fips_identity_from_secret_bytes(ctx, entropy, secret, &id) == FIPS_IDENTITY_OK);
    fips_identity_pubkey_full(id, pub);
    fips_identity_destroy(id);
    memset(bad, 0xff, sizeof(bad)); bad[0] = 2; /* x >= field modulus */
    CHECK(rejected(ctx, secret, bad) == 0);
    memset(bad, 0, sizeof(bad)); bad[0] = 2; /* off-curve x = 0 */
    CHECK(rejected(ctx, secret, bad) == 0);
    memcpy(bad, pub, sizeof(bad)); bad[0] = 4; /* not compressed */
    CHECK(rejected(ctx, secret, bad) == 0);
    memset(bad, 0, sizeof(bad)); /* infinity/invalid encoding */
    CHECK(rejected(ctx, secret, bad) == 0);
    memset(secret, 0, sizeof(secret));
    CHECK(rejected(ctx, secret, pub) == 0);
    CHECK(unhex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141", order) == 0);
    CHECK(rejected(ctx, order, pub) == 0);
    memset(secret, 0xff, sizeof(secret));
    CHECK(rejected(ctx, secret, pub) == 0);
    sodium_memzero(secret, sizeof(secret));
    sodium_memzero(order, sizeof(order));
    return 0;
}

int main(void)
{
    static const uint64_t seeds[] = {0, UINT64_C(0x0123456789abcdef), UINT64_MAX};
    fips_fixed_entropy fixed = {0x5a};
    fips_entropy entropy = fips_fixed_entropy_adapter(&fixed);
    fips_secp *ctx = NULL;
    uint8_t secrets[3][32], one[32] = {0}, two[32] = {0};
    CHECK(fips_identity_init_secp(entropy, &ctx) == FIPS_IDENTITY_OK);
    CHECK(fips_noise_dh_ops() == 0);
    for (size_t i = 0; i < 3; ++i) {
        fips_seeded_entropy seeded = {seeds[i] ^ UINT64_C(0x6b65797061697273)};
        fips_entropy e = fips_seeded_entropy_adapter(&seeded);
        e.fill(e.ctx, secrets[i], 32);
        sodium_memzero(&seeded, sizeof(seeded));
    }
    for (size_t i = 0; i < 3; ++i)
        CHECK(vector(ctx, entropy, secrets[i], secrets[(i+1)%3], i) == 0);
    one[31] = 1; two[31] = 2;
    CHECK(vector(ctx, entropy, one, two, 3) == 0);
    CHECK(invalid_keys(ctx, entropy) == 0);
    CHECK(fips_noise_dh_ops() == 23); /* Four calls/vector plus seven refusals. */
    fips_identity_destroy_secp(ctx);
    sodium_memzero(secrets, sizeof(secrets));
    sodium_memzero(one, sizeof(one));
    sodium_memzero(two, sizeof(two));
    puts("ECDH: four independent vectors, symmetry, parity, invalid keys, DH counter passed");
    return 0;
}
