#include "internal.h"
#include <secp256k1_preallocated.h>
#include <stdlib.h>

const char *fips_identity_error_name(fips_identity_status status)
{
    switch (status) {
#define NAME(v) case FIPS_IDENTITY_##v: return #v
    NAME(OK); NAME(InvalidSecretKey); NAME(SignatureVerificationFailed);
    NAME(InvalidNodeAddrLength); NAME(InvalidAddressLength); NAME(InvalidAddressPrefix);
    NAME(Bech32Encode); NAME(Bech32Decode); NAME(InvalidNpubPrefix);
    NAME(InvalidNpubLength); NAME(NonCanonicalNpub); NAME(InvalidNsecPrefix);
    NAME(InvalidNsecLength); NAME(InvalidHex); NAME(NonCanonicalNsec);
    NAME(BufferTooSmall); NAME(AllocationFailed);
#undef NAME
    default: return "Unknown";
    }
}

fips_identity_status fips_identity_init_secp(fips_entropy entropy, fips_secp **out)
{
    fips_secp *secp = malloc(sizeof(*secp));
    if (!secp) return FIPS_IDENTITY_AllocationFailed;
    size_t size = secp256k1_context_preallocated_size(SECP256K1_CONTEXT_NONE);
    secp->storage = malloc(size);
    if (!secp->storage) { free(secp); return FIPS_IDENTITY_AllocationFailed; }
    secp->ctx = secp256k1_context_preallocated_create(secp->storage, SECP256K1_CONTEXT_NONE);
    uint8_t seed[32];
    entropy.fill(entropy.ctx, seed, sizeof(seed));
    int ok = secp256k1_context_randomize(secp->ctx, seed);
    sodium_memzero(seed, sizeof(seed));
    if (!ok) { fips_identity_destroy_secp(secp); return FIPS_IDENTITY_InvalidSecretKey; }
    *out = secp;
    return FIPS_IDENTITY_OK;
}

void fips_identity_destroy_secp(fips_secp *secp)
{
    if (!secp) return;
    size_t size = secp256k1_context_preallocated_size(SECP256K1_CONTEXT_NONE);
    secp256k1_context_preallocated_destroy(secp->ctx);
    sodium_memzero(secp->storage, size);
    free(secp->storage);
    sodium_memzero(secp, sizeof(*secp));
    free(secp);
}
