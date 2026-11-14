#include "internal.h"
#include <fips/identity/auth.h>
#include <fips/identity/node_addr.h>
#include <fips/proto/codec.h>

fips_auth_challenge fips_auth_challenge_from_bytes(const uint8_t bytes[32])
{
    fips_auth_challenge challenge; memcpy(challenge.bytes, bytes, 32); return challenge;
}
fips_auth_challenge fips_auth_challenge_generate(fips_entropy entropy)
{
    fips_auth_challenge challenge; entropy.fill(entropy.ctx, challenge.bytes, 32); return challenge;
}
static fips_identity_status auth_digest(const fips_auth_challenge *challenge, uint64_t timestamp, uint8_t out[32])
{
    uint8_t bytes[52], be[8];
    for (size_t i = 0; i < 8; ++i) be[i] = (uint8_t)(timestamp >> (56-8*i));
    fips_writer writer; fips_writer_init(&writer, bytes, sizeof(bytes));
    /* Fixed exact capacities. Keep failures checked if the transcript changes. */
    if (fips_writer_write_bytes(&writer, (const uint8_t *)"fips-auth-v1", 12, NULL) != FIPS_OK ||
        fips_writer_write_bytes(&writer, challenge->bytes, 32, NULL) != FIPS_OK ||
        fips_writer_write_bytes(&writer, be, 8, NULL) != FIPS_OK)
        return FIPS_IDENTITY_BufferTooSmall;
    identity_sha256(bytes, sizeof(bytes), out);
    return FIPS_IDENTITY_OK;
}
fips_identity_status fips_identity_sign_challenge(const fips_identity *id, const fips_auth_challenge *challenge, uint64_t timestamp, fips_auth_response *out)
{
    uint8_t digest[32];
    fips_identity_status status = auth_digest(challenge, timestamp, digest);
    if (status != FIPS_IDENTITY_OK) return status;
    fips_auth_response response;
    status = identity_sign_digest(id, digest, response.signature);
    if (status != FIPS_IDENTITY_OK) return status;
    response.pubkey = id->pubkey; response.timestamp = timestamp;
    *out = response;
    return FIPS_IDENTITY_OK;
}
fips_identity_status fips_auth_challenge_verify(const fips_secp *secp, const fips_auth_challenge *challenge, const fips_auth_response *response, fips_node_addr *out)
{
    uint8_t digest[32];
    fips_identity_status status = auth_digest(challenge, response->timestamp, digest);
    if (status != FIPS_IDENTITY_OK) return status;
    secp256k1_xonly_pubkey pubkey;
    if (!secp256k1_xonly_pubkey_parse(secp->ctx, &pubkey, response->pubkey.bytes) ||
        !secp256k1_schnorrsig_verify(secp->ctx, response->signature, digest, 32, &pubkey))
        return FIPS_IDENTITY_SignatureVerificationFailed;
    *out = fips_node_addr_from_pubkey(&response->pubkey);
    return FIPS_IDENTITY_OK;
}
