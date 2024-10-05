#include "crypto_utils.h"
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <string.h>
#include <stdio.h>

// Helper function to convert BIGNUM to hex string
char *bn_to_hex(const BIGNUM *bn) {
    char *hex = BN_bn2hex(bn);
    if (!hex) {
        return NULL;
    }

    char *result = (char *)malloc(strlen(hex) + 1);
    if (!result) {
        OPENSSL_free(hex);
        return NULL;
    }

    strcpy(result, hex);
    OPENSSL_free(hex);
    return result;
}

// Generate a private key
char *generate_private_key() {
    int curve = NID_secp256k1;
    EC_KEY *ec_key = EC_KEY_new_by_curve_name(curve);
    if (!ec_key) {
        return NULL;
    }

    if (!EC_KEY_generate_key(ec_key)) {
        EC_KEY_free(ec_key);
        return NULL;
    }

    const BIGNUM *priv_key_bn = EC_KEY_get0_private_key(ec_key);
    char *priv_key_hex = bn_to_hex(priv_key_bn);

    EC_KEY_free(ec_key);
    return priv_key_hex;
}

// Get the public key from a private key
char *get_public_key(const char *sk) {
    BIGNUM *priv_key_bn = NULL;
    EC_KEY *ec_key = NULL;
    char *pub_key_hex = NULL;

    if (BN_hex2bn(&priv_key_bn, sk) == 0) {
        goto cleanup;
    }

    ec_key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!ec_key) {
        goto cleanup;
    }

    if (EC_KEY_set_private_key(ec_key, priv_key_bn) == 0) {
        goto cleanup;
    }

    const EC_GROUP *group = EC_KEY_get0_group(ec_key);
    EC_POINT *pub_key_point = EC_POINT_new(group);
    if (!pub_key_point) {
        goto cleanup;
    }

    if (EC_POINT_mul(group, pub_key_point, priv_key_bn, NULL, NULL, NULL) == 0) {
        EC_POINT_free(pub_key_point);
        goto cleanup;
    }

    if (EC_KEY_set_public_key(ec_key, pub_key_point) == 0) {
        EC_POINT_free(pub_key_point);
        goto cleanup;
    }

    EC_POINT_free(pub_key_point);

    const EC_POINT *pub_key = EC_KEY_get0_public_key(ec_key);
    BN_CTX *ctx = BN_CTX_new();
    pub_key_hex = EC_POINT_point2hex(group, pub_key, POINT_CONVERSION_COMPRESSED, ctx);
    BN_CTX_free(ctx);

cleanup:
    BN_free(priv_key_bn);
    EC_KEY_free(ec_key);
    return pub_key_hex;
}

// Validate if a public key is a valid 32-byte hex string
bool is_valid_public_key_hex(const char *pk) {
    if (!pk) {
        return false;
    }

    size_t len = strlen(pk);
    if (len != 64) { // 32 bytes in hex representation
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (!isxdigit(pk[i])) {
            return false;
        }
    }

    return true;
}

// Validate if a public key is valid
bool is_valid_public_key(const char *pk) {
    if (!is_valid_public_key_hex(pk)) {
        return false;
    }

    BIGNUM *pub_key_bn = NULL;
    EC_KEY *ec_key = NULL;
    bool result = false;

    if (BN_hex2bn(&pub_key_bn, pk) == 0) {
        goto cleanup;
    }

    ec_key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!ec_key) {
        goto cleanup;
    }

    const EC_GROUP *group = EC_KEY_get0_group(ec_key);
    EC_POINT *pub_key_point = EC_POINT_new(group);
    if (!pub_key_point) {
        goto cleanup;
    }

    if (EC_POINT_hex2point(group, pk, pub_key_point, NULL) == NULL) {
        EC_POINT_free(pub_key_point);
        goto cleanup;
    }

    if (EC_KEY_set_public_key(ec_key, pub_key_point) == 0) {
        EC_POINT_free(pub_key_point);
        goto cleanup;
    }

    result = EC_KEY_check_key(ec_key);

    EC_POINT_free(pub_key_point);

cleanup:
    BN_free(pub_key_bn);
    EC_KEY_free(ec_key);
    return result;
}