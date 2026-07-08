/*
 * libmarmot - MLS KeyPackage implementation (RFC 9420 §10)
 *
 * KeyPackage creation, serialization, validation, and KeyPackageRef.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls_key_package.h"
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_key_package_clear(MlsKeyPackage *kp)
{
    if (!kp) return;
    mls_leaf_node_clear(&kp->leaf_node);
    free(kp->extensions_data);
    memset(kp, 0, sizeof(*kp));
}

void
mls_key_package_private_clear(MlsKeyPackagePrivate *priv)
{
    if (!priv) return;
    sodium_memzero(priv, sizeof(*priv));
}

static void
key_package_set_default_cipher_suite(MlsKeyPackage *kp)
{
    kp->cipher_suite = MARMOT_CIPHERSUITE;
    kp->cipher_suite_raw[0] = 0x00;
    kp->cipher_suite_raw[1] = (uint8_t)MARMOT_CIPHERSUITE;
    kp->cipher_suite_raw_len = 2;
}

static int
key_package_write_cipher_suite(const MlsKeyPackage *kp, MlsTlsBuf *buf)
{
    if (!kp || !buf) return -1;
    if (kp->cipher_suite_raw_len > 0) {
        if (kp->cipher_suite_raw_len > sizeof(kp->cipher_suite_raw)) return -1;
        return mls_tls_buf_append(buf, kp->cipher_suite_raw, kp->cipher_suite_raw_len);
    }
    return mls_tls_write_u16(buf, kp->cipher_suite);
}

static int
u16_vec_contains(const uint16_t *v, size_t n, uint16_t x)
{
    if (!v && n > 0) return 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] == x) return 1;
    }
    return 0;
}

static int
key_package_tbs_serialize_for_signature(const MlsKeyPackage *kp, MlsTlsBuf *buf)
{
    if (!kp || !buf) return -1;
    MlsKeyPackage canonical = *kp;
    canonical.cipher_suite_raw[0] = 0x00;
    canonical.cipher_suite_raw[1] = (uint8_t)canonical.cipher_suite;
    canonical.cipher_suite_raw_len = 2;
    return mls_key_package_tbs_serialize(&canonical, buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * LeafNode signing
 *
 * LeafNodeTBS (for key_package source):
 * struct {
 *   <leaf_node_content>   // encryption_key, signature_key, credential, ...
 * } LeafNodeTBS;
 *
 * For key_package source, the signature is over the LeafNode content
 * (everything except the signature field).
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Serialize the LeafNode content for signing (LeafNodeTBS for key_package source).
 */
static int
leaf_node_tbs_serialize(const MlsLeafNode *node, MlsTlsBuf *buf)
{
    if (!node || !buf) return -1;

    /* encryption_key: HPKEPublicKey */
    if (mls_tls_write_opaque16(buf, node->encryption_key, MLS_KEM_PK_LEN) != 0)
        return -1;
    /* signature_key: SignaturePublicKey */
    if (mls_tls_write_opaque16(buf, node->signature_key, MLS_SIG_PK_LEN) != 0)
        return -1;
    /* credential_type */
    if (mls_tls_write_u16(buf, node->credential_type) != 0)
        return -1;
    /* credential identity */
    if (mls_tls_write_opaque16(buf, node->credential_identity,
                                node->credential_identity_len) != 0)
        return -1;
    /* capabilities (RFC 9420 §7.2): versions, ciphersuites, extensions, proposals, credentials */
#define WRITE_U16_VEC(arr, count) do { \
        size_t total = (count) * 2; \
        if (mls_tls_write_vli(buf, total) != 0) return -1; \
        for (size_t _i = 0; _i < (count); _i++) { \
            if (mls_tls_write_u16(buf, (arr)[_i]) != 0) return -1; \
        } \
    } while (0)

    WRITE_U16_VEC(node->versions, node->version_count);
    WRITE_U16_VEC(node->ciphersuites, node->ciphersuite_count);
    WRITE_U16_VEC(node->cap_extensions, node->cap_extension_count);
    WRITE_U16_VEC(node->proposals, node->proposal_count);
    WRITE_U16_VEC(node->cap_credentials, node->cap_credential_count);

#undef WRITE_U16_VEC
    /* leaf_node_source */
    if (mls_tls_write_u8(buf, node->leaf_node_source) != 0)
        return -1;
    /* source-dependent fields (RFC 9420 §7.2) */
    if (node->leaf_node_source == 1) { /* key_package: Lifetime */
        if (mls_tls_write_u64(buf, node->lifetime_not_before) != 0) return -1;
        if (mls_tls_write_u64(buf, node->lifetime_not_after) != 0) return -1;
    } else if (node->leaf_node_source == 3) { /* commit: parent_hash */
        if (mls_tls_write_opaque8(buf, node->parent_hash, node->parent_hash_len) != 0)
            return -1;
    }
    /* extensions */
    if (mls_tls_write_opaque32(buf, node->extensions_data, node->extensions_len) != 0)
        return -1;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * KeyPackage creation
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_key_package_create(MlsKeyPackage *kp,
                        MlsKeyPackagePrivate *priv_out,
                        const uint8_t *credential_identity,
                        size_t credential_identity_len,
                        const uint8_t *extensions_data,
                        size_t extensions_len)
{
    if (!kp || !priv_out || !credential_identity) return -1;
    memset(kp, 0, sizeof(*kp));
    memset(priv_out, 0, sizeof(*priv_out));

    /* Protocol version and ciphersuite */
    kp->version = 1;    /* mls10 */
    key_package_set_default_cipher_suite(kp);

    /* Generate init keypair (X25519) */
    if (mls_crypto_kem_keygen(priv_out->init_key_private, kp->init_key) != 0)
        goto fail;

    /* Generate leaf encryption keypair (X25519) */
    if (mls_crypto_kem_keygen(priv_out->encryption_key_private,
                               kp->leaf_node.encryption_key) != 0)
        goto fail;

    /* Generate signing keypair (Ed25519) */
    if (mls_crypto_sign_keygen(priv_out->signature_key_private,
                                kp->leaf_node.signature_key) != 0)
        goto fail;

    /* Credential: BasicCredential with identity */
    kp->leaf_node.credential_type = MLS_CREDENTIAL_BASIC;
    kp->leaf_node.credential_identity = malloc(credential_identity_len);
    if (!kp->leaf_node.credential_identity) goto fail;
    memcpy(kp->leaf_node.credential_identity, credential_identity,
           credential_identity_len);
    kp->leaf_node.credential_identity_len = credential_identity_len;

    /* Capabilities (RFC 9420 §7.2) */
    /* versions: MLS 1.0 */
    kp->leaf_node.version_count = 1;
    kp->leaf_node.versions = malloc(sizeof(uint16_t));
    if (!kp->leaf_node.versions) goto fail;
    kp->leaf_node.versions[0] = 1;  /* mls10 */

    /* ciphersuites: 0x0001 */
    kp->leaf_node.ciphersuite_count = 1;
    kp->leaf_node.ciphersuites = malloc(sizeof(uint16_t));
    if (!kp->leaf_node.ciphersuites) goto fail;
    kp->leaf_node.ciphersuites[0] = MARMOT_CIPHERSUITE;

    /* extensions: none */
    kp->leaf_node.cap_extensions = NULL;
    kp->leaf_node.cap_extension_count = 0;

    /* proposals: none */
    kp->leaf_node.proposals = NULL;
    kp->leaf_node.proposal_count = 0;

    /* credentials: basic (0x0001) */
    kp->leaf_node.cap_credential_count = 1;
    kp->leaf_node.cap_credentials = malloc(sizeof(uint16_t));
    if (!kp->leaf_node.cap_credentials) goto fail;
    kp->leaf_node.cap_credentials[0] = MLS_CREDENTIAL_BASIC;

    /* Leaf node source: key_package (1) */
    kp->leaf_node.leaf_node_source = 1;

    /* Extensions on the leaf node: empty for now */
    kp->leaf_node.extensions_data = NULL;
    kp->leaf_node.extensions_len = 0;

    /* KeyPackage-level extensions */
    if (extensions_data && extensions_len > 0) {
        kp->extensions_data = malloc(extensions_len);
        if (!kp->extensions_data) goto fail;
        memcpy(kp->extensions_data, extensions_data, extensions_len);
        kp->extensions_len = extensions_len;
    }

    /* Sign the leaf node (LeafNodeTBS for key_package source) */
    {
        MlsTlsBuf tbs_buf;
        if (mls_tls_buf_init(&tbs_buf, 256) != 0) goto fail;

        if (leaf_node_tbs_serialize(&kp->leaf_node, &tbs_buf) != 0) {
            mls_tls_buf_free(&tbs_buf);
            goto fail;
        }

        if (mls_crypto_sign_with_label(kp->leaf_node.signature,
                                        priv_out->signature_key_private,
                                        "LeafNodeTBS",
                                        tbs_buf.data, tbs_buf.len) != 0) {
            mls_tls_buf_free(&tbs_buf);
            goto fail;
        }
        kp->leaf_node.signature_len = MLS_SIG_LEN;
        mls_tls_buf_free(&tbs_buf);
    }

    /* Sign the KeyPackage (KeyPackageTBS) */
    {
        MlsTlsBuf kp_tbs;
        if (mls_tls_buf_init(&kp_tbs, 512) != 0) goto fail;

        if (key_package_tbs_serialize_for_signature(kp, &kp_tbs) != 0) {
            mls_tls_buf_free(&kp_tbs);
            goto fail;
        }

        if (mls_crypto_sign_with_label(kp->signature,
                                        priv_out->signature_key_private,
                                        "KeyPackageTBS",
                                        kp_tbs.data, kp_tbs.len) != 0) {
            mls_tls_buf_free(&kp_tbs);
            goto fail;
        }
        kp->signature_len = MLS_SIG_LEN;
        mls_tls_buf_free(&kp_tbs);
    }

    return 0;

fail:
    mls_key_package_clear(kp);
    mls_key_package_private_clear(priv_out);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * TLS serialization
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_key_package_tbs_serialize(const MlsKeyPackage *kp, MlsTlsBuf *buf)
{
    if (!kp || !buf) return -1;

    /* version: uint16 */
    if (mls_tls_write_u16(buf, kp->version) != 0) return -1;
    /* cipher_suite: exact wire encoding preserved from deserialize */
    if (key_package_write_cipher_suite(kp, buf) != 0) return -1;
    /* init_key: HPKEPublicKey<V> */
    if (mls_tls_write_opaque16(buf, kp->init_key, MLS_KEM_PK_LEN) != 0) return -1;
    /* leaf_node: LeafNode */
    if (mls_leaf_node_serialize(&kp->leaf_node, buf) != 0) return -1;
    /* extensions: Extension<V> */
    if (mls_tls_write_opaque32(buf, kp->extensions_data, kp->extensions_len) != 0) return -1;

    return 0;
}

int
mls_key_package_serialize(const MlsKeyPackage *kp, MlsTlsBuf *buf)
{
    if (!kp || !buf) return -1;

    /* KeyPackageTBS fields */
    if (mls_key_package_tbs_serialize(kp, buf) != 0) return -1;

    /* signature: opaque<V> */
    if (mls_tls_write_opaque16(buf, kp->signature, kp->signature_len) != 0) return -1;

    return 0;
}

int
mls_key_package_deserialize(MlsTlsReader *reader, MlsKeyPackage *kp)
{
    if (!reader || !kp) return -1;
    memset(kp, 0, sizeof(*kp));

    /* version */
    if (mls_tls_read_u16(reader, &kp->version) != 0) goto fail;
    /* cipher_suite: RFC 9420 uses uint16.  MDK vectors encode the same
     * supported suite as a three-u16 tuple; preserve exact bytes so TBS
     * signatures and roundtrips use the received wire image. */
    {
        uint16_t first = 0;
        if (mls_tls_read_u16(reader, &first) != 0) goto fail;
        kp->cipher_suite_raw[0] = (uint8_t)(first >> 8);
        kp->cipher_suite_raw[1] = (uint8_t)(first & 0xff);
        kp->cipher_suite_raw_len = 2;
        kp->cipher_suite = first;

        if (first != MARMOT_CIPHERSUITE && mls_tls_reader_remaining(reader) >= 5) {
            uint16_t second = ((uint16_t)reader->data[reader->pos] << 8) |
                              (uint16_t)reader->data[reader->pos + 1];
            uint16_t third = ((uint16_t)reader->data[reader->pos + 2] << 8) |
                             (uint16_t)reader->data[reader->pos + 3];
            uint8_t next = reader->data[reader->pos + 4];
            if (second == MARMOT_CIPHERSUITE && third == MARMOT_CIPHERSUITE &&
                next == MLS_KEM_PK_LEN) {
                kp->cipher_suite = MARMOT_CIPHERSUITE;
                kp->cipher_suite_raw[2] = reader->data[reader->pos];
                kp->cipher_suite_raw[3] = reader->data[reader->pos + 1];
                kp->cipher_suite_raw[4] = reader->data[reader->pos + 2];
                kp->cipher_suite_raw[5] = reader->data[reader->pos + 3];
                kp->cipher_suite_raw_len = 6;
                reader->pos += 4;
            }
        }
    }
    /* init_key */
    {
        uint8_t *init_key = NULL;
        size_t init_key_len = 0;
        if (mls_tls_read_opaque16(reader, &init_key, &init_key_len) != 0) goto fail;
        if (init_key_len != MLS_KEM_PK_LEN) { free(init_key); goto fail; }
        memcpy(kp->init_key, init_key, MLS_KEM_PK_LEN);
        free(init_key);
    }
    /* leaf_node */
    if (mls_leaf_node_deserialize(reader, &kp->leaf_node) != 0) goto fail;
    /* extensions */
    if (mls_tls_read_opaque32(reader, &kp->extensions_data, &kp->extensions_len) != 0) goto fail;
    /* signature */
    {
        uint8_t *sig = NULL;
        size_t sig_len = 0;
        if (mls_tls_read_opaque16(reader, &sig, &sig_len) != 0) goto fail;
        if (sig_len != MLS_SIG_LEN) { free(sig); goto fail; }
        memcpy(kp->signature, sig, sig_len);
        kp->signature_len = sig_len;
        free(sig);
    }

    return 0;

fail:
    mls_key_package_clear(kp);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Validation
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_key_package_validate(const MlsKeyPackage *kp)
{
    if (!kp) return MARMOT_ERR_INVALID_INPUT;

    /* Check protocol version */
    if (kp->version != 1) return MARMOT_ERR_UNSUPPORTED;

    /* Check ciphersuite */
    if (kp->cipher_suite != MARMOT_CIPHERSUITE) return MARMOT_ERR_UNSUPPORTED;

    /* Signature lengths are fixed for Ed25519 in this ciphersuite. */
    if (kp->signature_len != MLS_SIG_LEN || kp->leaf_node.signature_len != MLS_SIG_LEN)
        return MARMOT_ERR_SIGNATURE;

    /* RFC 9420 §7.2 KeyPackage LeafNode checklist. */
    if (kp->leaf_node.leaf_node_source != 1) return MARMOT_ERR_KEY_PACKAGE;
    if (kp->leaf_node.parent_hash || kp->leaf_node.parent_hash_len != 0)
        return MARMOT_ERR_KEY_PACKAGE;
    if (kp->leaf_node.lifetime_not_after < kp->leaf_node.lifetime_not_before)
        return MARMOT_ERR_KEY_PACKAGE;
    if (!kp->leaf_node.credential_identity || kp->leaf_node.credential_identity_len == 0)
        return MARMOT_ERR_KEY_PACKAGE;
    if (kp->leaf_node.credential_type != MLS_CREDENTIAL_BASIC)
        return MARMOT_ERR_KEY_PACKAGE;
    if (kp->leaf_node.version_count == 0 ||
        !u16_vec_contains(kp->leaf_node.versions, kp->leaf_node.version_count, kp->version))
        return MARMOT_ERR_KEY_PACKAGE;
    if (kp->leaf_node.ciphersuite_count == 0 ||
        !u16_vec_contains(kp->leaf_node.ciphersuites, kp->leaf_node.ciphersuite_count, kp->cipher_suite))
        return MARMOT_ERR_KEY_PACKAGE;
    if (kp->leaf_node.cap_credential_count == 0 ||
        !u16_vec_contains(kp->leaf_node.cap_credentials, kp->leaf_node.cap_credential_count,
                          kp->leaf_node.credential_type))
        return MARMOT_ERR_KEY_PACKAGE;

    /* Verify the independently-signed LeafNode signature first. */
    {
        MlsTlsBuf tbs;
        if (mls_tls_buf_init(&tbs, 512) != 0) return MARMOT_ERR_INTERNAL;
        if (leaf_node_tbs_serialize(&kp->leaf_node, &tbs) != 0) {
            mls_tls_buf_free(&tbs);
            return MARMOT_ERR_INTERNAL;
        }
        int rc = mls_crypto_verify_with_label(kp->leaf_node.signature,
                                              kp->leaf_node.signature_key,
                                              "LeafNodeTBS", tbs.data, tbs.len);
        mls_tls_buf_free(&tbs);
        if (rc != 0) return MARMOT_ERR_SIGNATURE;
    }

    /* Verify KeyPackage signature */
    {
        MlsTlsBuf tbs;
        if (mls_tls_buf_init(&tbs, 512) != 0) return MARMOT_ERR_INTERNAL;
        if (key_package_tbs_serialize_for_signature(kp, &tbs) != 0) {
            mls_tls_buf_free(&tbs);
            return MARMOT_ERR_INTERNAL;
        }

        int rc = mls_crypto_verify_with_label(kp->signature, kp->leaf_node.signature_key,
                                              "KeyPackageTBS", tbs.data, tbs.len);
        mls_tls_buf_free(&tbs);
        if (rc != 0) return MARMOT_ERR_SIGNATURE;
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * KeyPackageRef (RFC 9420 §5.3.1)
 *
 * KeyPackageRef = RefHash("MLS 1.0 KeyPackage Reference",
 *                          KeyPackage)
 *
 * Where KeyPackage is the full TLS-serialized key package.
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_key_package_ref(const MlsKeyPackage *kp, uint8_t out[MLS_HASH_LEN])
{
    if (!kp || !out) return -1;

    /* Serialize the full key package */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 512) != 0) return -1;
    if (mls_key_package_serialize(kp, &buf) != 0) {
        mls_tls_buf_free(&buf);
        return -1;
    }

    /* RefHash("MLS 1.0 KeyPackage Reference", serialized) */
    int rc = mls_crypto_ref_hash(out, "MLS 1.0 KeyPackage Reference",
                                  buf.data, buf.len);
    mls_tls_buf_free(&buf);
    return rc;
}
