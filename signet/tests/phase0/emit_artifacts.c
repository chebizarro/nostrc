/* SPDX-License-Identifier: MIT */
/*
 * Phase 0 spike: emit real WebAuthn artifacts as JSON (hex) so an INDEPENDENT
 * verifier (python-fido2) can CBOR-decode and cryptographically check them.
 */

#include "signet/fido_crypto.h"
#include "signet/fido_cbor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

static const uint8_t SIGNET_AAGUID[16] = {
    0x51,0x67,0x6e,0x74, 0x50,0x61,0x73,0x73, 0x6b,0x65,0x79,0x00, 0x00,0x00,0x00,0x01
};

static void phex(const char *name, const uint8_t *p, size_t n, int last)
{
    printf("  \"%s\": \"", name);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\"%s\n", last ? "" : ",");
}

int main(void)
{
    const char *rp_id = "example.com";

    signet_fido_key *k = signet_fido_key_generate();
    if (!k) return 1;
    uint8_t x[32], y[32];
    if (signet_fido_key_public_xy(k, x, y)) return 1;

    uint8_t *cose = NULL; size_t cose_len = 0;
    if (signet_cose_ec2_p256(x, y, &cose, &cose_len)) return 1;

    uint8_t cred_id[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

    uint8_t reg_flags = SIGNET_FIDO_FLAG_UP | SIGNET_FIDO_FLAG_AT |
                        SIGNET_FIDO_FLAG_BE | SIGNET_FIDO_FLAG_BS;
    uint8_t *authreg = NULL; size_t authreg_len = 0;
    if (signet_fido_auth_data(rp_id, reg_flags, 0, SIGNET_AAGUID,
                              cred_id, sizeof(cred_id), cose, cose_len,
                              &authreg, &authreg_len)) return 1;

    uint8_t *attobj = NULL; size_t attobj_len = 0;
    if (signet_fido_attestation_none(authreg, authreg_len, &attobj, &attobj_len)) return 1;

    /* assertion */
    const char *client_data =
        "{\"type\":\"webauthn.get\",\"challenge\":\"c2lnbmV0LXBoYXNlMA\",\"origin\":\"https://example.com\"}";
    uint8_t cdh[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)client_data, strlen(client_data), cdh);

    uint8_t asr_flags = SIGNET_FIDO_FLAG_UP | SIGNET_FIDO_FLAG_BE | SIGNET_FIDO_FLAG_BS;
    uint8_t *authasr = NULL; size_t authasr_len = 0;
    if (signet_fido_auth_data(rp_id, asr_flags, 0, NULL, NULL, 0, NULL, 0,
                              &authasr, &authasr_len)) return 1;

    size_t signed_len = authasr_len + sizeof(cdh);
    uint8_t *signed_buf = malloc(signed_len);
    memcpy(signed_buf, authasr, authasr_len);
    memcpy(signed_buf + authasr_len, cdh, sizeof(cdh));

    uint8_t *sig = NULL; size_t sig_len = 0;
    if (signet_fido_key_sign(k, signed_buf, signed_len, &sig, &sig_len)) return 1;

    printf("{\n");
    printf("  \"rpId\": \"%s\",\n", rp_id);
    phex("attestationObject", attobj, attobj_len, 0);
    phex("credentialId", cred_id, sizeof(cred_id), 0);
    phex("pubX", x, 32, 0);
    phex("pubY", y, 32, 0);
    phex("authDataAssert", authasr, authasr_len, 0);
    phex("clientDataHashAssert", cdh, sizeof(cdh), 0);
    phex("signature", sig, sig_len, 1);
    printf("}\n");

    free(cose); free(authreg); free(attobj); free(authasr); free(signed_buf); free(sig);
    signet_fido_key_free(k);
    return 0;
}
