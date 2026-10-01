/*
 * libmarmot - MLS Welcome Message implementation (RFC 9420 §12.4.3.1)
 *
 * Welcome processing: decrypt joiner_secret, decrypt GroupInfo,
 * initialize group state for the new member.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls_welcome.h"
#include <stdlib.h>
#include <string.h>
#include <sodium.h>
#include <stdio.h>

#define MLS_EXTENSION_RATCHET_TREE 0x0002

static int
build_encrypt_context(const char *label,
                      const uint8_t *context, size_t context_len,
                      uint8_t **out, size_t *out_len)
{
    if (!label || !out || !out_len || (context_len > 0 && !context)) return -1;
    const char prefix[] = "MLS 1.0 ";
    size_t prefix_len = strlen(prefix);
    size_t label_body_len = strlen(label);
    size_t full_len = prefix_len + label_body_len;
    uint8_t *full_label = malloc(full_len);
    if (!full_label) return -1;
    memcpy(full_label, prefix, prefix_len);
    memcpy(full_label + prefix_len, label, label_body_len);
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, full_len + context_len + 16) != 0) {
        free(full_label);
        return -1;
    }
    if (mls_tls_write_opaque16(&buf, full_label, full_len) != 0 ||
        mls_tls_write_opaque32(&buf, context, context_len) != 0) {
        free(full_label);
        mls_tls_buf_free(&buf);
        return -1;
    }
    free(full_label);
    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

static int
hpke_decrypt_with_label(uint8_t *pt, size_t *pt_len,
                        const uint8_t enc[MLS_KEM_ENC_LEN],
                        const uint8_t sk[MLS_KEM_SK_LEN],
                        const uint8_t pk[MLS_KEM_PK_LEN],
                        const char *label,
                        const uint8_t *context, size_t context_len,
                        const uint8_t *ct, size_t ct_len)
{
    uint8_t *info = NULL;
    size_t info_len = 0;
    if (build_encrypt_context(label, context, context_len, &info, &info_len) != 0)
        return -1;
    int rc = mls_crypto_hpke_open_base(pt, pt_len, enc, sk, pk, info, info_len,
                                       NULL, 0, ct, ct_len);
    free(info);
    return rc;
}

typedef struct {
    uint8_t *psk_id;
    size_t psk_id_len;
    uint8_t *psk_nonce;
    size_t psk_nonce_len;
} MlsWelcomePskId;

static void
welcome_psk_ids_free(MlsWelcomePskId *ids, size_t count)
{
    if (!ids) return;
    for (size_t i = 0; i < count; i++) {
        free(ids[i].psk_id);
        free(ids[i].psk_nonce);
    }
    free(ids);
}

/* Free a heap copy of secret material. */
static void
free_secret(uint8_t *p, size_t len)
{
    if (!p) return;
    sodium_memzero(p, len);
    free(p);
}

/*
 * GroupSecrets (RFC 9420 §12.4.3.1):
 *   opaque joiner_secret<V>;
 *   optional<PathSecret> path_secret;   PathSecret = { opaque path_secret<V>; }
 *   PreSharedKeyID psks<V>;
 * *has_path_secret reports whether path_secret was present; when it is, its
 * value (exactly Hash.length bytes) is written to path_secret.
 */
static int
parse_group_secrets(const uint8_t *data, size_t len,
                    uint8_t joiner_secret[MLS_HASH_LEN],
                    uint8_t path_secret[MLS_HASH_LEN],
                    bool *has_path_secret,
                    MlsWelcomePskId **psk_ids_out,
                    size_t *psk_id_count_out)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    *has_path_secret = false;
    uint8_t *js = NULL;
    size_t js_len = 0;
    if (mls_tls_read_opaque16(&r, &js, &js_len) != 0) return -1;
    if (js_len != MLS_HASH_LEN) { free_secret(js, js_len); return -1; }
    memcpy(joiner_secret, js, MLS_HASH_LEN);
    free_secret(js, js_len);

    if (psk_ids_out) *psk_ids_out = NULL;
    if (psk_id_count_out) *psk_id_count_out = 0;

    /* optional<T> presence is exactly 0 or 1 (RFC 9420 §2.1.1). */
    uint8_t has_path = 0;
    if (mls_tls_read_u8(&r, &has_path) != 0 || has_path > 1) return -1;
    if (has_path) {
        uint8_t *path = NULL;
        size_t path_len = 0;
        if (mls_tls_read_opaque16(&r, &path, &path_len) != 0) return -1;
        if (path_len != MLS_HASH_LEN) { free_secret(path, path_len); return -1; }
        memcpy(path_secret, path, MLS_HASH_LEN);
        free_secret(path, path_len);
        *has_path_secret = true;
    }
    uint8_t *psks = NULL;
    size_t psks_len = 0;
    if (mls_tls_read_opaque32(&r, &psks, &psks_len) != 0) return -1;
    if (psks_len > 0) {
        MlsTlsReader pr;
        mls_tls_reader_init(&pr, psks, psks_len);
        while (!mls_tls_reader_done(&pr)) {
            uint8_t psk_type = 0;
            if (mls_tls_read_u8(&pr, &psk_type) != 0 || psk_type != 1) {
                free(psks);
                welcome_psk_ids_free(psk_ids_out ? *psk_ids_out : NULL,
                                     psk_id_count_out ? *psk_id_count_out : 0);
                return -1;
            }
            MlsWelcomePskId *items = realloc(psk_ids_out ? *psk_ids_out : NULL,
                                             ((psk_id_count_out ? *psk_id_count_out : 0) + 1) * sizeof(MlsWelcomePskId));
            if (!items) {
                free(psks);
                welcome_psk_ids_free(psk_ids_out ? *psk_ids_out : NULL,
                                     psk_id_count_out ? *psk_id_count_out : 0);
                return -1;
            }
            *psk_ids_out = items;
            MlsWelcomePskId *slot = &items[*psk_id_count_out];
            memset(slot, 0, sizeof(*slot));
            if (mls_tls_read_opaque16(&pr, &slot->psk_id, &slot->psk_id_len) != 0 ||
                mls_tls_read_opaque16(&pr, &slot->psk_nonce, &slot->psk_nonce_len) != 0) {
                free(psks);
                welcome_psk_ids_free(*psk_ids_out, *psk_id_count_out + 1);
                *psk_ids_out = NULL;
                *psk_id_count_out = 0;
                return -1;
            }
            (*psk_id_count_out)++;
        }
    }
    free(psks);
    if (!mls_tls_reader_done(&r)) {
        welcome_psk_ids_free(psk_ids_out ? *psk_ids_out : NULL,
                             psk_id_count_out ? *psk_id_count_out : 0);
        if (psk_ids_out) *psk_ids_out = NULL;
        if (psk_id_count_out) *psk_id_count_out = 0;
        return -1;
    }
    return 0;
}

static int
mls_group_info_tbs_serialize_local(const MlsGroupInfo *gi, MlsTlsBuf *buf)
{
    if (!gi || !buf) return -1;
    if (mls_tls_write_u16(buf, 1) != 0) return -1;
    if (mls_tls_write_u16(buf, MARMOT_CIPHERSUITE) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->group_id, gi->group_id_len) != 0) return -1;
    if (mls_tls_write_u64(buf, gi->epoch) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->tree_hash, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->confirmed_transcript_hash, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_write_opaque32(buf, gi->extensions_data, gi->extensions_len) != 0) return -1;
    if (mls_tls_write_opaque32(buf, gi->group_info_extensions_data,
                                gi->group_info_extensions_len) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->confirmation_tag, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_write_u32(buf, gi->signer_leaf) != 0) return -1;
    return 0;
}

static int
find_ratchet_tree_extension(const uint8_t *exts, size_t exts_len,
                            const uint8_t **tree_data, size_t *tree_len)
{
    if (!exts || exts_len == 0 || !tree_data || !tree_len) return -1;
    MlsTlsReader r;
    mls_tls_reader_init(&r, exts, exts_len);
    while (!mls_tls_reader_done(&r)) {
        uint16_t ext_type = 0;
        uint8_t *data = NULL;
        size_t len = 0;
        if (mls_tls_read_u16(&r, &ext_type) != 0) return -1;
        if (mls_tls_read_opaque32(&r, &data, &len) != 0) return -1;
        if (ext_type == MLS_EXTENSION_RATCHET_TREE) {
            *tree_data = data;
            *tree_len = len;
            return 0;
        }
        free(data);
    }
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_encrypted_group_secrets_clear(MlsEncryptedGroupSecrets *egs)
{
    if (!egs) return;
    free(egs->encrypted_joiner_secret);
    sodium_memzero(egs, sizeof(*egs));
}

void
mls_welcome_clear(MlsWelcome *w)
{
    if (!w) return;
    if (w->secrets) {
        for (size_t i = 0; i < w->secret_count; i++)
            mls_encrypted_group_secrets_clear(&w->secrets[i]);
        free(w->secrets);
    }
    free(w->encrypted_group_info);
    memset(w, 0, sizeof(*w));
}

/* ══════════════════════════════════════════════════════════════════════════
 * TLS serialization
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_welcome_serialize(const MlsWelcome *w, MlsTlsBuf *buf)
{
    if (!w || !buf) return -1;

    if (w->has_mls_message_wrapper) {
        if (mls_tls_write_u16(buf, 1) != 0) return -1;
        if (mls_tls_write_u16(buf, MLS_WIRE_FORMAT_WELCOME) != 0) return -1;
    }

    /* cipher_suite */
    if (mls_tls_write_u16(buf, w->cipher_suite) != 0) return -1;

    /* secrets: EncryptedGroupSecrets<V> */
    MlsTlsBuf secrets_buf;
    if (mls_tls_buf_init(&secrets_buf, 256) != 0) return -1;

    for (size_t i = 0; i < w->secret_count; i++) {
        const MlsEncryptedGroupSecrets *egs = &w->secrets[i];

        /* key_package_ref: KeyPackageRef opaque<V> */
        if (mls_tls_write_opaque16(&secrets_buf, egs->key_package_ref, MLS_HASH_LEN) != 0)
            goto fail_secrets;
        /* HPKECiphertext: kem_output || ciphertext */
        if (mls_tls_write_opaque16(&secrets_buf, egs->kem_output, MLS_KEM_ENC_LEN) != 0)
            goto fail_secrets;
        if (mls_tls_write_opaque16(&secrets_buf, egs->encrypted_joiner_secret,
                                    egs->encrypted_joiner_secret_len) != 0)
            goto fail_secrets;
    }

    if (mls_tls_write_opaque32(buf, secrets_buf.data, secrets_buf.len) != 0) {
        mls_tls_buf_free(&secrets_buf);
        return -1;
    }
    mls_tls_buf_free(&secrets_buf);

    /* encrypted_group_info */
    if (mls_tls_write_opaque32(buf, w->encrypted_group_info,
                                w->encrypted_group_info_len) != 0)
        return -1;

    return 0;

fail_secrets:
    mls_tls_buf_free(&secrets_buf);
    return -1;
}

int
mls_welcome_deserialize(MlsTlsReader *reader, MlsWelcome *w)
{
    if (!reader || !w) return -1;
    memset(w, 0, sizeof(*w));

    size_t start_pos = reader->pos;
    uint16_t first = 0;
    if (mls_tls_read_u16(reader, &first) != 0) goto fail;
    if (first == 1 && mls_tls_reader_remaining(reader) >= 2) {
        uint16_t wire_format = 0;
        if (mls_tls_read_u16(reader, &wire_format) != 0) goto fail;
        if (wire_format == MLS_WIRE_FORMAT_WELCOME) {
            w->has_mls_message_wrapper = true;
            if (mls_tls_read_u16(reader, &w->cipher_suite) != 0) goto fail;
        } else {
            reader->pos = start_pos;
            if (mls_tls_read_u16(reader, &w->cipher_suite) != 0) goto fail;
        }
    } else {
        reader->pos = start_pos;
        if (mls_tls_read_u16(reader, &w->cipher_suite) != 0) goto fail;
    }

    uint8_t *secrets_data = NULL;
    size_t secrets_len = 0;
    if (mls_tls_read_opaque32(reader, &secrets_data, &secrets_len) != 0) goto fail;

    if (secrets_len > 0) {
        MlsTlsReader secrets_reader;
        mls_tls_reader_init(&secrets_reader, secrets_data, secrets_len);

        while (!mls_tls_reader_done(&secrets_reader)) {
            MlsEncryptedGroupSecrets *new_secrets = realloc(
                w->secrets, (w->secret_count + 1) * sizeof(MlsEncryptedGroupSecrets));
            if (!new_secrets) { free(secrets_data); goto fail; }
            w->secrets = new_secrets;

            MlsEncryptedGroupSecrets *egs = &w->secrets[w->secret_count];
            memset(egs, 0, sizeof(*egs));

            {
                uint8_t *kp_ref = NULL;
                size_t kp_ref_len = 0;
                if (mls_tls_read_opaque16(&secrets_reader, &kp_ref, &kp_ref_len) != 0) {
                    free(secrets_data); goto fail;
                }
                if (kp_ref_len != MLS_HASH_LEN) {
                    free(kp_ref); free(secrets_data); goto fail;
                }
                memcpy(egs->key_package_ref, kp_ref, MLS_HASH_LEN);
                free(kp_ref);
            }
            {
                uint8_t *kem = NULL;
                size_t kem_len = 0;
                if (mls_tls_read_opaque16(&secrets_reader, &kem, &kem_len) != 0) {
                    free(secrets_data); goto fail;
                }
                if (kem_len != MLS_KEM_ENC_LEN) { free(kem); free(secrets_data); goto fail; }
                memcpy(egs->kem_output, kem, MLS_KEM_ENC_LEN);
                free(kem);
            }
            if (mls_tls_read_opaque16(&secrets_reader, &egs->encrypted_joiner_secret,
                                       &egs->encrypted_joiner_secret_len) != 0) {
                free(secrets_data); goto fail;
            }
            w->secret_count++;
        }
    }
    free(secrets_data);

    if (mls_tls_read_opaque32(reader, &w->encrypted_group_info,
                               &w->encrypted_group_info_len) != 0)
        goto fail;

    return 0;
fail:
    mls_welcome_clear(w);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Welcome processing
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_welcome_process(const uint8_t *welcome_data, size_t welcome_len,
                    const MlsKeyPackage *kp,
                    const MlsKeyPackagePrivate *kp_priv,
                    const uint8_t *ratchet_tree, size_t tree_len,
                    MlsGroup *group_out)
{
    if (!welcome_data || !kp || !kp_priv || !group_out)
        return MARMOT_ERR_INVALID_ARG;

    MlsWelcome welcome;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, welcome_data, welcome_len);
    if (mls_welcome_deserialize(&reader, &welcome) != 0)
        return MARMOT_ERR_WELCOME_INVALID;

    int rc = mls_welcome_process_parsed_with_psks(&welcome, kp, kp_priv,
                                                   ratchet_tree, tree_len,
                                                   NULL, 0, group_out);
    mls_welcome_clear(&welcome);
    return rc;
}

int
mls_welcome_process_with_psks(const uint8_t *welcome_data, size_t welcome_len,
                              const MlsKeyPackage *kp,
                              const MlsKeyPackagePrivate *kp_priv,
                              const uint8_t *ratchet_tree, size_t tree_len,
                              const MlsPskInput *psks, size_t psk_count,
                              MlsGroup *group_out)
{
    if (!welcome_data || !kp || !kp_priv || !group_out ||
        (psk_count > 0 && !psks))
        return MARMOT_ERR_INVALID_ARG;

    MlsWelcome welcome;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, welcome_data, welcome_len);
    if (mls_welcome_deserialize(&reader, &welcome) != 0)
        return MARMOT_ERR_WELCOME_INVALID;

    int rc = mls_welcome_process_parsed_with_psks(&welcome, kp, kp_priv,
                                                   ratchet_tree, tree_len,
                                                   psks, psk_count,
                                                   group_out);
    mls_welcome_clear(&welcome);
    return rc;
}

int
mls_welcome_process_parsed(const MlsWelcome *welcome,
                           const MlsKeyPackage *kp,
                           const MlsKeyPackagePrivate *kp_priv,
                           const uint8_t *ratchet_tree, size_t tree_len,
                           MlsGroup *group_out)
{
    return mls_welcome_process_parsed_with_psks(welcome, kp, kp_priv,
                                                ratchet_tree, tree_len,
                                                NULL, 0, group_out);
}

/* `path_secret` is scratch space owned (and wiped) by the caller, so every
 * return path below leaves no copy of GroupSecrets.path_secret behind. */
static int
welcome_process_impl(const MlsWelcome *welcome,
                     const MlsKeyPackage *kp,
                     const MlsKeyPackagePrivate *kp_priv,
                     const uint8_t *ratchet_tree, size_t tree_len,
                     const MlsPskInput *psks, size_t psk_count,
                     MlsGroup *group_out,
                     uint8_t path_secret[MLS_HASH_LEN],
                     uint32_t *out_signer_leaf)
{
    if (!welcome || !kp || !kp_priv || !group_out ||
        (psk_count > 0 && !psks))
        return MARMOT_ERR_INVALID_ARG;

    memset(group_out, 0, sizeof(*group_out));

    /* Verify ciphersuite */
    if (welcome->cipher_suite != MARMOT_CIPHERSUITE)
        return MARMOT_ERR_UNSUPPORTED;

    /* Find our EncryptedGroupSecrets entry */
    uint8_t our_kp_ref[MLS_HASH_LEN];
    if (mls_key_package_ref(kp, our_kp_ref) != 0)
        return MARMOT_ERR_INTERNAL;

    const MlsEncryptedGroupSecrets *our_egs = NULL;
    for (size_t i = 0; i < welcome->secret_count; i++) {
        if (memcmp(welcome->secrets[i].key_package_ref, our_kp_ref, MLS_HASH_LEN) == 0) {
            our_egs = &welcome->secrets[i];
            break;
        }
    }
    if (!our_egs)
        return MARMOT_ERR_WELCOME_NOT_FOUND;

    uint8_t *group_secrets = malloc(our_egs->encrypted_joiner_secret_len);
    if (!group_secrets) return MARMOT_ERR_MEMORY;
    size_t group_secrets_len = 0;
    if (hpke_decrypt_with_label(group_secrets, &group_secrets_len,
                                our_egs->kem_output,
                                kp_priv->init_key_private, kp->init_key,
                                "Welcome",
                                welcome->encrypted_group_info,
                                welcome->encrypted_group_info_len,
                                our_egs->encrypted_joiner_secret,
                                our_egs->encrypted_joiner_secret_len) != 0) {
        free_secret(group_secrets, our_egs->encrypted_joiner_secret_len);
        return MARMOT_ERR_CRYPTO;
    }

    uint8_t joiner_secret[MLS_HASH_LEN];
    /* GroupSecrets.path_secret (nostrc-il4i): installed into the joiner's
     * path-key cache once the Welcome has been fully verified, below. */
    bool has_path_secret = false;
    MlsWelcomePskId *welcome_psk_ids = NULL;
    size_t welcome_psk_id_count = 0;
    if (parse_group_secrets(group_secrets, group_secrets_len, joiner_secret,
                            path_secret, &has_path_secret,
                            &welcome_psk_ids, &welcome_psk_id_count) != 0) {
        free_secret(group_secrets, our_egs->encrypted_joiner_secret_len);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        return MARMOT_ERR_WELCOME_INVALID;
    }
    free_secret(group_secrets, our_egs->encrypted_joiner_secret_len);

    uint8_t psk_secret[MLS_HASH_LEN];
    MlsPskInput *effective_psks = NULL;
    size_t effective_psk_count = 0;
    if (welcome_psk_id_count > 0) {
        if (psk_count != welcome_psk_id_count) {
            welcome_psk_ids_free(welcome_psk_ids, welcome_psk_id_count);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            return MARMOT_ERR_WELCOME_INVALID;
        }
        effective_psks = calloc(welcome_psk_id_count, sizeof(*effective_psks));
        if (!effective_psks) {
            welcome_psk_ids_free(welcome_psk_ids, welcome_psk_id_count);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            return MARMOT_ERR_MEMORY;
        }
        effective_psk_count = welcome_psk_id_count;
        for (size_t i = 0; i < welcome_psk_id_count; i++) {
            const MlsPskInput *match = NULL;
            for (size_t j = 0; j < psk_count; j++) {
                if (psks[j].psk_id_len == welcome_psk_ids[i].psk_id_len &&
                    memcmp(psks[j].psk_id, welcome_psk_ids[i].psk_id,
                           welcome_psk_ids[i].psk_id_len) == 0) {
                    match = &psks[j];
                    break;
                }
            }
            if (!match || !match->psk || match->psk_len == 0) {
                free(effective_psks);
                welcome_psk_ids_free(welcome_psk_ids, welcome_psk_id_count);
                sodium_memzero(joiner_secret, sizeof(joiner_secret));
                return MARMOT_ERR_WELCOME_INVALID;
            }
            effective_psks[i].psk_id = welcome_psk_ids[i].psk_id;
            effective_psks[i].psk_id_len = welcome_psk_ids[i].psk_id_len;
            effective_psks[i].psk = match->psk;
            effective_psks[i].psk_len = match->psk_len;
            effective_psks[i].psk_nonce = welcome_psk_ids[i].psk_nonce;
            effective_psks[i].psk_nonce_len = welcome_psk_ids[i].psk_nonce_len;
        }
    } else {
        effective_psks = (MlsPskInput *)psks;
        effective_psk_count = psk_count;
    }
    if (mls_psk_secret_compute(effective_psks, effective_psk_count, psk_secret) != 0) {
        if (welcome_psk_id_count > 0) free(effective_psks);
        welcome_psk_ids_free(welcome_psk_ids, welcome_psk_id_count);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        return MARMOT_ERR_INTERNAL;
    }
    if (welcome_psk_id_count > 0) free(effective_psks);
    welcome_psk_ids_free(welcome_psk_ids, welcome_psk_id_count);

    /* Derive welcome_secret per RFC 9420 §8.3:
     * member_secret = Extract(joiner_secret, psk_secret_or_zero)
     * welcome_secret = DeriveSecret(member_secret, "welcome")
     * welcome_key = ExpandWithLabel(welcome_secret, "key", "", key_len)
     * welcome_nonce = ExpandWithLabel(welcome_secret, "nonce", "", nonce_len) */
    uint8_t member_secret_early[MLS_HASH_LEN];
    if (mls_crypto_hkdf_extract(member_secret_early, joiner_secret, MLS_HASH_LEN,
                                 psk_secret, MLS_HASH_LEN) != 0) {
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(psk_secret, sizeof(psk_secret));
        return MARMOT_ERR_INTERNAL;
    }
    uint8_t welcome_secret[MLS_HASH_LEN];
    if (mls_crypto_derive_secret(welcome_secret, member_secret_early, "welcome") != 0) {
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(member_secret_early, sizeof(member_secret_early));
        return MARMOT_ERR_INTERNAL;
    }
    sodium_memzero(member_secret_early, sizeof(member_secret_early));

    uint8_t welcome_key[MLS_AEAD_KEY_LEN];
    uint8_t welcome_nonce[MLS_AEAD_NONCE_LEN];
    if (mls_crypto_expand_with_label(welcome_key, MLS_AEAD_KEY_LEN,
                                      welcome_secret, "key", NULL, 0) != 0 ||
        mls_crypto_expand_with_label(welcome_nonce, MLS_AEAD_NONCE_LEN,
                                      welcome_secret, "nonce", NULL, 0) != 0) {
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(welcome_secret, sizeof(welcome_secret));
        return MARMOT_ERR_INTERNAL;
    }

    /* Decrypt GroupInfo */
    uint8_t *gi_data = malloc(welcome->encrypted_group_info_len);
    if (!gi_data) {
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(welcome_secret, sizeof(welcome_secret));
        return MARMOT_ERR_MEMORY;
    }

    size_t gi_len = 0;
    if (mls_crypto_aead_decrypt(gi_data, &gi_len,
                                 welcome_key, welcome_nonce,
                                 welcome->encrypted_group_info,
                                 welcome->encrypted_group_info_len,
                                 NULL, 0) != 0) {
        free(gi_data);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(welcome_secret, sizeof(welcome_secret));
        sodium_memzero(welcome_key, sizeof(welcome_key));
        return MARMOT_ERR_CRYPTO;
    }
    sodium_memzero(welcome_key, sizeof(welcome_key));
    sodium_memzero(welcome_nonce, sizeof(welcome_nonce));
    sodium_memzero(welcome_secret, sizeof(welcome_secret));

    /* Parse GroupInfo. MDK vectors may carry it as an MLSMessage/group_info
     * wrapper; GroupInfo itself starts after the 4-byte MLSMessage header. */
    const uint8_t *gi_parse = gi_data;
    size_t gi_parse_len = gi_len;
    if (gi_parse_len >= 4 && gi_parse[0] == 0x00 && gi_parse[1] == 0x01 &&
        gi_parse[2] == 0x00 && gi_parse[3] == MLS_WIRE_FORMAT_GROUP_INFO) {
        gi_parse += 4;
        gi_parse_len -= 4;
    }
    MlsGroupInfo gi;
    MlsTlsReader gi_reader;
    mls_tls_reader_init(&gi_reader, gi_parse, gi_parse_len);
    int gi_parse_rc = mls_group_info_deserialize(&gi_reader, &gi);
    if (gi_parse_rc != 0 || !mls_tls_reader_done(&gi_reader)) {
        size_t pos = gi_reader.pos;
        fprintf(stderr, "welcome invalid: groupinfo parse rc=%d gi_len=%zu parse_len=%zu pos=%zu\n",
                gi_parse_rc, gi_len, gi_parse_len, pos);
        free(gi_data);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        return MARMOT_ERR_WELCOME_INVALID;
    }

    free(gi_data);

    /* Initialize group state from GroupInfo */
    group_out->group_id = malloc(gi.group_id_len);
    if (!group_out->group_id) {
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        return MARMOT_ERR_MEMORY;
    }
    memcpy(group_out->group_id, gi.group_id, gi.group_id_len);
    group_out->group_id_len = gi.group_id_len;
    group_out->epoch = gi.epoch;
    group_out->max_forward_distance = 1000;

    /* Copy transcript hash from GroupInfo */
    memcpy(group_out->confirmed_transcript_hash,
           gi.confirmed_transcript_hash, MLS_HASH_LEN);

    /* Compute interim transcript hash from confirmed + confirmation_tag */
    {
        MlsTlsBuf tbuf;
        if (mls_tls_buf_init(&tbuf, MLS_HASH_LEN * 2) != 0) {
            mls_group_info_clear(&gi);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            mls_group_free(group_out);
            return MARMOT_ERR_MEMORY;
        }
        mls_tls_buf_append(&tbuf, gi.confirmed_transcript_hash, MLS_HASH_LEN);
        mls_tls_write_opaque32(&tbuf, gi.confirmation_tag, MLS_HASH_LEN);
        mls_crypto_hash(group_out->interim_transcript_hash, tbuf.data, tbuf.len);
        mls_tls_buf_free(&tbuf);
    }

    /* The GroupContext fixes the group's profile (nostrc-qp24.5.1).  An
     * adopted one is admitted only when complete, canonical and limited to
     * requirements libmarmot honours (mls_app_components.h); a legacy one
     * keeps its historical handling.  No fallback between the two. */
    group_out->profile = mls_group_context_profile_of(gi.extensions_data, gi.extensions_len);
    if (group_out->profile == MARMOT_GROUP_PROFILE_ADOPTED) {
        MlsAdoptedGroupContext adopted_gc;
        int gc_rc = mls_adopted_group_context_parse(gi.extensions_data, gi.extensions_len,
                                                    &adopted_gc);
        if (gc_rc != 0) {
            mls_group_info_clear(&gi);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            mls_group_free(group_out);
            return gc_rc;
        }
    }

    /* Extensions */
    if (gi.extensions_data && gi.extensions_len > 0) {
        group_out->extensions_data = malloc(gi.extensions_len);
        if (!group_out->extensions_data) {
            mls_group_info_clear(&gi);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            mls_group_free(group_out);
            return MARMOT_ERR_MEMORY;
        }
        memcpy(group_out->extensions_data, gi.extensions_data, gi.extensions_len);
        group_out->extensions_len = gi.extensions_len;
    }

    /* Store our signing key and encryption key */
    memcpy(group_out->own_signature_key, kp_priv->signature_key_private, MLS_SIG_SK_LEN);
    memcpy(group_out->own_encryption_key, kp_priv->encryption_key_private, MLS_KEM_SK_LEN);

    /* Initialize ratchet tree from an out-of-band tree or the GroupInfo ratchet_tree extension. */
    const uint8_t *tree_src = ratchet_tree;
    size_t tree_src_len = tree_len;
    uint8_t *tree_ext_alloc = NULL;
    if (!tree_src || tree_src_len == 0) {
        const uint8_t *ext_tree = NULL;
        size_t ext_tree_len = 0;
        if (find_ratchet_tree_extension(gi.group_info_extensions_data,
                                        gi.group_info_extensions_len,
                                        &ext_tree, &ext_tree_len) != 0) {
            fprintf(stderr, "welcome invalid: ratchet tree extension missing\n");
            mls_group_info_clear(&gi);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            mls_group_free(group_out);
            return MARMOT_ERR_WELCOME_INVALID;
        }
        tree_ext_alloc = (uint8_t *)ext_tree;
        tree_src = ext_tree;
        tree_src_len = ext_tree_len;
    }

    if (mls_ratchet_tree_deserialize(tree_src, tree_src_len, &group_out->tree) != 0) {
        fprintf(stderr, "welcome invalid: ratchet tree deserialize\n");
        free(tree_ext_alloc);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_WELCOME_INVALID;
    }
    free(tree_ext_alloc);

    uint8_t actual_tree_hash[MLS_HASH_LEN];
    if (mls_tree_root_hash(&group_out->tree, actual_tree_hash) != 0 ||
        sodium_memcmp(actual_tree_hash, gi.tree_hash, MLS_HASH_LEN) != 0 ||
        mls_tree_verify_parent_hashes(&group_out->tree) != 0) {
        fprintf(stderr, "welcome invalid: tree hash/parent hash\n");
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_WELCOME_INVALID;
    }

    if (gi.signer_leaf >= group_out->tree.n_leaves) {
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_WELCOME_INVALID;
    }
    uint32_t signer_node = mls_tree_leaf_to_node(gi.signer_leaf);
    if (group_out->tree.nodes[signer_node].type != MLS_NODE_LEAF) {
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_WELCOME_INVALID;
    }
    MlsTlsBuf gi_tbs;
    if (mls_tls_buf_init(&gi_tbs, 512) != 0 ||
        mls_group_info_tbs_serialize_local(&gi, &gi_tbs) != 0 ||
        gi.signature_len != MLS_SIG_LEN ||
        mls_crypto_verify_with_label(gi.signature,
            group_out->tree.nodes[signer_node].leaf.signature_key,
            "GroupInfoTBS", gi_tbs.data, gi_tbs.len) != 0) {
        fprintf(stderr, "welcome invalid: groupinfo signature\n");
        mls_tls_buf_free(&gi_tbs);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_WELCOME_INVALID;
    }
    mls_tls_buf_free(&gi_tbs);

    /* Adopted profile (nostrc-qp24.5.1; RFC 9420 §12.4.3.1): with the tree
     * authenticated by the GroupInfo signature, every member leaf must
     * itself verify -- its signature covers the account-proof dictionary --
     * and satisfy the group's capability and component requirements; every
     * admin must be a member. */
    if (group_out->profile == MARMOT_GROUP_PROFILE_ADOPTED) {
        int adm_rc = mls_group_profile_check_entered(group_out);
        for (uint32_t i = 0; adm_rc == 0 && i < group_out->tree.n_leaves; i++) {
            const MlsNode *n = &group_out->tree.nodes[mls_tree_leaf_to_node(i)];
            if (n->type == MLS_NODE_LEAF &&
                mls_leaf_node_verify_signature(&n->leaf, group_out->group_id,
                                               group_out->group_id_len, i) != 0)
                adm_rc = MARMOT_ERR_WELCOME_INVALID;
        }
        if (adm_rc != 0) {
            mls_group_info_clear(&gi);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            mls_group_free(group_out);
            return adm_rc;
        }
    }

    /* Find our leaf index by matching our KeyPackage's HPKE encryption key
     * against leaf nodes in the tree (RFC 9420 §12.4.3.1).
     * The committer places the joiner's LeafNode into the tree, so the
     * encryption key will match our KeyPackage's leaf_node.encryption_key. */
    bool found_self = false;

    for (uint32_t leaf_idx = 0; leaf_idx < group_out->tree.n_leaves; leaf_idx++) {
        uint32_t node_idx = mls_tree_leaf_to_node(leaf_idx);
        MlsNode *node = &group_out->tree.nodes[node_idx];

        if (node->type == MLS_NODE_LEAF &&
            memcmp(node->leaf.encryption_key,
                   kp->leaf_node.encryption_key,
                   MLS_KEM_PK_LEN) == 0) {
            group_out->own_leaf_index = leaf_idx;
            found_self = true;
            break;
        }
    }

    if (!found_self) {
        /* External ratchet tree provided but our encryption key wasn't
         * found in any leaf — cannot determine our position. */
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_WELCOME_NOT_FOUND;
    }

    /* Derive epoch secrets from joiner_secret.
     * The key schedule for a joiner uses:
     * - joiner_secret (instead of constructing from init_secret + commit_secret)
     * - GroupContext from the GroupInfo
     *
     * epoch_secret = ExpandWithLabel(joiner_secret, "epoch", GroupContext, Hash.length)
     * Then derive all epoch secrets from epoch_secret.
     */

    /* Build GroupContext for key schedule */
    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    uint8_t tree_hash[MLS_HASH_LEN];
    /* Use the tree hash from GroupInfo since our tree may be incomplete */
    memcpy(tree_hash, gi.tree_hash, MLS_HASH_LEN);

    if (mls_group_context_serialize(
            group_out->group_id, group_out->group_id_len,
            group_out->epoch, tree_hash,
            group_out->confirmed_transcript_hash,
            group_out->extensions_data, group_out->extensions_len,
            &gc_data, &gc_len) != 0) {
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Per RFC 9420 §8.3, the joiner derives secrets from joiner_secret
     * through the same key schedule as the creator:
     *
     * 1. member_secret = Extract(joiner_secret, psk_secret_or_zero)
     * 2. welcome_secret = DeriveSecret(member_secret, "welcome")
     * 3. epoch_secret = ExpandWithLabel(member_secret, "epoch", GroupContext, Nh)
     * 4. All epoch secrets derived from epoch_secret
     *
     * This matches mls_key_schedule_derive steps 3-6. */
    /* Step 1: member_secret = Extract(joiner_secret, psk_secret)
     * salt = joiner_secret, ikm = psk_secret (zero if no PSK) */
    uint8_t member_secret[MLS_HASH_LEN];
    if (mls_crypto_hkdf_extract(member_secret, joiner_secret, MLS_HASH_LEN,
                                 psk_secret, MLS_HASH_LEN) != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Step 2: welcome_secret = DeriveSecret(member_secret, "welcome") */
    MlsEpochSecrets *es = &group_out->epoch_secrets;
    memcpy(es->joiner_secret, joiner_secret, MLS_HASH_LEN);
    if (mls_crypto_derive_secret(es->welcome_secret, member_secret, "welcome") != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(member_secret, sizeof(member_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Step 3: epoch_secret = ExpandWithLabel(member_secret, "epoch", GroupContext, Nh) */
    uint8_t epoch_secret[MLS_HASH_LEN];
    if (mls_crypto_expand_with_label(epoch_secret, MLS_HASH_LEN,
                                      member_secret, "epoch",
                                      gc_data, gc_len) != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(member_secret, sizeof(member_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }
    sodium_memzero(member_secret, sizeof(member_secret));

    /* Step 4: Derive all epoch secrets from epoch_secret */
    if (mls_crypto_derive_secret(es->sender_data_secret, epoch_secret, "sender data") != 0 ||
        mls_crypto_derive_secret(es->encryption_secret, epoch_secret, "encryption") != 0 ||
        mls_crypto_derive_secret(es->exporter_secret, epoch_secret, "exporter") != 0 ||
        mls_crypto_derive_secret(es->external_secret, epoch_secret, "external") != 0 ||
        mls_crypto_derive_secret(es->confirmation_key, epoch_secret, "confirm") != 0 ||
        mls_crypto_derive_secret(es->membership_key, epoch_secret, "membership") != 0 ||
        mls_crypto_derive_secret(es->resumption_psk, epoch_secret, "resumption") != 0 ||
        mls_crypto_derive_secret(es->epoch_authenticator, epoch_secret, "authentication") != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Step 5: init_secret for next epoch */
    if (mls_crypto_derive_secret(es->init_secret, epoch_secret, "init") != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Initialize secret tree; its root is then deleted (RFC 9420 §9.2). */
    int st_rc = mls_secret_tree_init(&group_out->secret_tree,
                                     es->encryption_secret,
                                     mls_tree_canonical_leaves(&group_out->tree));
    sodium_memzero(es->encryption_secret, MLS_HASH_LEN);
    /* A joiner has no further use for the joiner and welcome secrets (the
     * GroupInfo is already open): consumed, deleted (RFC 9420 §9.2). */
    sodium_memzero(es->joiner_secret, MLS_HASH_LEN);
    sodium_memzero(es->welcome_secret, MLS_HASH_LEN);
    if (st_rc != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Verify confirmation tag */
    uint8_t expected_tag[MLS_HASH_LEN];
    if (mls_compute_confirmation_tag(es->confirmation_key,
                                      group_out->confirmed_transcript_hash,
                                      expected_tag) != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    if (sodium_memcmp(expected_tag, gi.confirmation_tag, MLS_HASH_LEN) != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_WELCOME_INVALID;
    }

    /* RFC 9420 §12.4.3.1: with a path_secret, derive the keys of our lowest
     * common ancestor with the committer (GroupInfo.signer) and of the nodes
     * above it on the committer's filtered direct path, verify each against
     * the (now authenticated) tree, and keep the private keys (nostrc-il4i).
     * Its absence is legitimate: the Commit may have had no UpdatePath. */
    if (has_path_secret) {
        int path_rc = mls_group_welcome_install_path_secret(group_out,
                                                            gi.signer_leaf,
                                                            path_secret);
        if (path_rc != 0) {
            free(gc_data);
            mls_group_info_clear(&gi);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            sodium_memzero(psk_secret, sizeof(psk_secret));
            sodium_memzero(epoch_secret, sizeof(epoch_secret));
            mls_group_free(group_out);
            return path_rc == MARMOT_ERR_INTERNAL ? MARMOT_ERR_INTERNAL
                                                  : MARMOT_ERR_WELCOME_INVALID;
        }
    }

    if (out_signer_leaf) *out_signer_leaf = gi.signer_leaf;
    free(gc_data);
    mls_group_info_clear(&gi);
    sodium_memzero(joiner_secret, sizeof(joiner_secret));
    sodium_memzero(psk_secret, sizeof(psk_secret));
    sodium_memzero(epoch_secret, sizeof(epoch_secret));

    return 0;
}

int
mls_welcome_process_parsed_with_psks(const MlsWelcome *welcome,
                                     const MlsKeyPackage *kp,
                                     const MlsKeyPackagePrivate *kp_priv,
                                     const uint8_t *ratchet_tree, size_t tree_len,
                                     const MlsPskInput *psks, size_t psk_count,
                                     MlsGroup *group_out)
{
    uint8_t path_secret[MLS_HASH_LEN];
    memset(path_secret, 0, sizeof(path_secret));
    int rc = welcome_process_impl(welcome, kp, kp_priv, ratchet_tree, tree_len,
                                  psks, psk_count, group_out, path_secret, NULL);
    sodium_memzero(path_secret, sizeof(path_secret));
    return rc;
}

int
mls_welcome_process_parsed_signer(const MlsWelcome *welcome,
                                  const MlsKeyPackage *kp,
                                  const MlsKeyPackagePrivate *kp_priv,
                                  MlsGroup *group_out,
                                  uint32_t *out_signer_leaf)
{
    if (!out_signer_leaf) return MARMOT_ERR_INVALID_ARG;
    uint8_t path_secret[MLS_HASH_LEN];
    memset(path_secret, 0, sizeof(path_secret));
    int rc = welcome_process_impl(welcome, kp, kp_priv, NULL, 0, NULL, 0, group_out,
                                  path_secret, out_signer_leaf);
    sodium_memzero(path_secret, sizeof(path_secret));
    return rc;
}
