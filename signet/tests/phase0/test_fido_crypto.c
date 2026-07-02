/* SPDX-License-Identifier: MIT */
/* Phase 0 spike: prove the OpenSSL ES256/P-256 key backend. */

#include "signet/fido_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

int main(void)
{
    /* keygen + public coordinates */
    signet_fido_key *k = signet_fido_key_generate();
    OK(k, "keygen");

    uint8_t x[32], y[32];
    OK(signet_fido_key_public_xy(k, x, y) == 0, "public_xy");

    /* export + re-import the private key (software backend is exportable) */
    uint8_t *der = NULL; size_t der_len = 0;
    OK(signet_fido_key_export_private(k, &der, &der_len) == 0 && der_len > 0, "export_private");

    signet_fido_key *k2 = signet_fido_key_import_private(der, der_len);
    OK(k2, "import_private");

    uint8_t x2[32], y2[32];
    OK(signet_fido_key_public_xy(k2, x2, y2) == 0, "public_xy(reimport)");
    OK(memcmp(x, x2, 32) == 0 && memcmp(y, y2, 32) == 0, "reimported public key matches");

    /* sign with original, verify against exported public coordinates */
    const uint8_t msg[] = "signet phase0 es256 signing input";
    uint8_t *sig = NULL; size_t sig_len = 0;
    OK(signet_fido_key_sign(k, msg, sizeof(msg) - 1, &sig, &sig_len) == 0 && sig_len > 0, "sign");
    OK(signet_fido_verify_p256(x, y, msg, sizeof(msg) - 1, sig, sig_len) == 1, "verify valid");

    /* signature made by the re-imported key also verifies (same key material) */
    uint8_t *sig2 = NULL; size_t sig2_len = 0;
    OK(signet_fido_key_sign(k2, msg, sizeof(msg) - 1, &sig2, &sig2_len) == 0, "sign(reimport)");
    OK(signet_fido_verify_p256(x, y, msg, sizeof(msg) - 1, sig2, sig2_len) == 1, "verify reimport sig");

    /* tampered message must fail */
    uint8_t bad[sizeof(msg) - 1];
    memcpy(bad, msg, sizeof(bad));
    bad[0] ^= 0x01;
    OK(signet_fido_verify_p256(x, y, bad, sizeof(bad), sig, sig_len) == 0, "verify rejects tamper");

    free(der); free(sig); free(sig2);
    signet_fido_key_free(k);
    signet_fido_key_free(k2);

    printf("PASS test_fido_crypto (ES256/P-256 keygen, export/import, sign, verify, tamper-reject)\n");
    return 0;
}
