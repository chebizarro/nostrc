/* SPDX-License-Identifier: MIT */
/*
 * Phase 0 spike: self-hosted WebAuthn register + assert.
 *
 * Builds a real attestation object (makeCredential) and a real assertion
 * (getAssertion) with our encoder + ES256 backend, then verifies the assertion
 * the way a relying party would: extract the public key registered in the
 * attestedCredentialData and check the ECDSA signature over
 * authenticatorData || SHA-256(clientDataJSON). This proves the full crypto +
 * encoding path without an external verifier.
 */

#include "signet/fido_crypto.h"
#include "signet/fido_cbor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

#define OK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

/* Fixed signet spike AAGUID (frozen at implementation per plan). */
static const uint8_t SIGNET_AAGUID[16] = {
    0x51,0x67,0x6e,0x74, 0x50,0x61,0x73,0x73, 0x6b,0x65,0x79,0x00, 0x00,0x00,0x00,0x01
};

int main(void)
{
    const char *rp_id = "example.com";

    /* --- registration (makeCredential) --- */
    signet_fido_key *k = signet_fido_key_generate();
    OK(k, "keygen");

    uint8_t x[32], y[32];
    OK(signet_fido_key_public_xy(k, x, y) == 0, "public_xy");

    uint8_t *cose = NULL; size_t cose_len = 0;
    OK(signet_cose_ec2_p256(x, y, &cose, &cose_len) == 0, "cose_ec2");
    /* COSE_Key must begin with a 5-entry map (0xA5). */
    OK(cose_len > 0 && cose[0] == 0xA5, "cose is 5-map");

    const uint8_t cred_id[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

    uint8_t reg_flags = SIGNET_FIDO_FLAG_UP | SIGNET_FIDO_FLAG_AT |
                        SIGNET_FIDO_FLAG_BE | SIGNET_FIDO_FLAG_BS;
    uint8_t *authreg = NULL; size_t authreg_len = 0;
    OK(signet_fido_auth_data(rp_id, reg_flags, 0, SIGNET_AAGUID,
                             cred_id, sizeof(cred_id), cose, cose_len,
                             &authreg, &authreg_len) == 0, "authData(reg)");

    /* authData layout: 32 rpIdHash + 1 flags + 4 signCount + 16 aaguid + 2 len + credId + cose */
    OK(authreg_len == 32 + 1 + 4 + 16 + 2 + sizeof(cred_id) + cose_len, "authData(reg) length");
    OK(authreg[32] == reg_flags, "reg flags byte (UP|AT|BE|BS)");
    OK(authreg[33]==0 && authreg[34]==0 && authreg[35]==0 && authreg[36]==0, "signCount==0");
    /* the tail must be exactly the COSE key we registered */
    OK(memcmp(authreg + authreg_len - cose_len, cose, cose_len) == 0, "attestedCredentialData embeds COSE key");

    uint8_t *attobj = NULL; size_t attobj_len = 0;
    OK(signet_fido_attestation_none(authreg, authreg_len, &attobj, &attobj_len) == 0, "attObj(none)");
    OK(attobj_len > 0 && attobj[0] == 0xA3, "attObj is 3-map");

    /* --- authentication (getAssertion) --- */
    /* Relying party sends a clientDataJSON; the client hashes it. */
    const char *client_data = "{\"type\":\"webauthn.get\",\"challenge\":\"c2lnbmV0\",\"origin\":\"https://example.com\"}";
    uint8_t cdh[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)client_data, strlen(client_data), cdh);

    uint8_t asr_flags = SIGNET_FIDO_FLAG_UP | SIGNET_FIDO_FLAG_BE | SIGNET_FIDO_FLAG_BS; /* no AT */
    uint8_t *authasr = NULL; size_t authasr_len = 0;
    OK(signet_fido_auth_data(rp_id, asr_flags, 0, NULL, NULL, 0, NULL, 0,
                             &authasr, &authasr_len) == 0, "authData(assert)");
    OK(authasr_len == 32 + 1 + 4, "authData(assert) has no attestedCredentialData");

    /* signed data = authenticatorData || clientDataHash */
    size_t signed_len = authasr_len + sizeof(cdh);
    uint8_t *signed_buf = malloc(signed_len);
    OK(signed_buf, "alloc signed");
    memcpy(signed_buf, authasr, authasr_len);
    memcpy(signed_buf + authasr_len, cdh, sizeof(cdh));

    uint8_t *sig = NULL; size_t sig_len = 0;
    OK(signet_fido_key_sign(k, signed_buf, signed_len, &sig, &sig_len) == 0, "sign assertion");

    /* RP-side: verify with the registered public key. */
    OK(signet_fido_verify_p256(x, y, signed_buf, signed_len, sig, sig_len) == 1,
       "RP verifies assertion signature");

    /* Wrong challenge/clientData must fail. */
    signed_buf[signed_len - 1] ^= 0x01;
    OK(signet_fido_verify_p256(x, y, signed_buf, signed_len, sig, sig_len) == 0,
       "RP rejects wrong clientDataHash");

    free(cose); free(authreg); free(attobj); free(authasr); free(signed_buf); free(sig);
    signet_fido_key_free(k);

    printf("PASS test_fido_cbor (COSE_Key + authData + none-attestation; self-hosted RP register+assert verified)\n");
    return 0;
}
