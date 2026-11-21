#include <fips/noise/ecdh.h>
#include "identity/internal.h"
#include <secp256k1_ecdh.h>

#ifdef FIPS_TEST_HOOKS
static _Thread_local uint64_t dh_ops;
uint64_t fips_noise_dh_ops(void) { return dh_ops; }
#endif

/* The default libsecp256k1 hash includes a parity prefix: not the FIPS DH. */
static int ecdh_hash_xcopy(unsigned char *out, const unsigned char *x32,
                           const unsigned char *y32, void *data)
{
    (void)y32;
    (void)data;
    memcpy(out, x32, 32);
    return 1;
}

fips_identity_status fips_noise_ecdh(const fips_secp *ctx,
    const uint8_t secret_key[32], const uint8_t public_key[33], uint8_t out[32])
{
    secp256k1_pubkey parsed;
    uint8_t x[32] = {0}, hash[32] = {0};
    fips_identity_status status = FIPS_IDENTITY_InvalidSecretKey;
#ifdef FIPS_TEST_HOOKS
    ++dh_ops;
#endif
    if (!secp256k1_ec_pubkey_parse(ctx->ctx, &parsed, public_key, 33) ||
        !secp256k1_ecdh(ctx->ctx, x, &parsed, secret_key, ecdh_hash_xcopy, NULL))
        goto cleanup;
    identity_sha256(x, sizeof(x), hash); /* Also wipes its SHA-256 state. */
    memcpy(out, hash, sizeof(hash));
    status = FIPS_IDENTITY_OK;
cleanup:
    sodium_memzero(x, sizeof(x));
    sodium_memzero(hash, sizeof(hash));
    sodium_memzero(&parsed, sizeof(parsed));
    return status;
}
