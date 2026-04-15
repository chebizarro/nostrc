/*
 * libmarmot - MLS Message Framing implementation (RFC 9420 §6)
 *
 * PrivateMessage encryption/decryption and sender data protection.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls_framing.h"
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Reuse guard
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_apply_reuse_guard(uint8_t nonce[MLS_AEAD_NONCE_LEN],
                      const uint8_t reuse_guard[4])
{
    for (int i = 0; i < 4; i++) {
        nonce[i] ^= reuse_guard[i];
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Content AAD (RFC 9420 §6.3.2)
 *
 * struct {
 *   opaque group_id<V>;
 *   uint64 epoch;
 *   ContentType content_type;
 *   opaque authenticated_data<V>;
 * } PrivateContentAAD;
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_build_content_aad(const uint8_t *group_id, size_t group_id_len,
                       uint64_t epoch, uint8_t content_type,
                       const uint8_t *authenticated_data, size_t aad_len,
                       uint8_t **out, size_t *out_len)
{
    if (!out || !out_len) return -1;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 64) != 0) return -1;

    /* group_id: opaque<V> */
    if (mls_tls_write_opaque8(&buf, group_id, group_id_len) != 0) goto fail;
    /* epoch: uint64 */
    if (mls_tls_write_u64(&buf, epoch) != 0) goto fail;
    /* content_type: uint8 */
    if (mls_tls_write_u8(&buf, content_type) != 0) goto fail;
    /* authenticated_data: opaque<V> */
    if (mls_tls_write_opaque32(&buf, authenticated_data, aad_len) != 0) goto fail;

    *out = buf.data;
    *out_len = buf.len;
    return 0;

fail:
    mls_tls_buf_free(&buf);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Sender data encryption/decryption (RFC 9420 §6.3.1)
 *
 * SenderData:
 *   uint32 leaf_index;
 *   uint32 generation;
 *   opaque reuse_guard[4];
 *
 * sender_data_key = ExpandWithLabel(sender_data_secret, "key",
 *                                    ciphertext_sample, AEAD_KEY_LEN)
 * sender_data_nonce = ExpandWithLabel(sender_data_secret, "nonce",
 *                                      ciphertext_sample, AEAD_NONCE_LEN)
 * encrypted_sender_data = AEAD.Seal(key, nonce, "", plaintext_sender_data)
 * ══════════════════════════════════════════════════════════════════════════ */

/** Serialize SenderData to bytes: leaf_index(4) + generation(4) + reuse_guard(4) = 12 bytes */
static int
sender_data_serialize(const MlsSenderData *sd, uint8_t out[12])
{
    out[0] = (uint8_t)(sd->leaf_index >> 24);
    out[1] = (uint8_t)(sd->leaf_index >> 16);
    out[2] = (uint8_t)(sd->leaf_index >> 8);
    out[3] = (uint8_t)(sd->leaf_index);
    out[4] = (uint8_t)(sd->generation >> 24);
    out[5] = (uint8_t)(sd->generation >> 16);
    out[6] = (uint8_t)(sd->generation >> 8);
    out[7] = (uint8_t)(sd->generation);
    memcpy(out + 8, sd->reuse_guard, 4);
    return 0;
}

static int
sender_data_deserialize(const uint8_t in[12], MlsSenderData *sd)
{
    sd->leaf_index = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                     ((uint32_t)in[2] << 8) | (uint32_t)in[3];
    sd->generation = ((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16) |
                     ((uint32_t)in[6] << 8) | (uint32_t)in[7];
    memcpy(sd->reuse_guard, in + 8, 4);
    return 0;
}

/** Derive sender data key and nonce from secret + ciphertext sample. */
static int
derive_sender_data_keys(const uint8_t sender_data_secret[MLS_HASH_LEN],
                        const uint8_t *ciphertext_sample, size_t sample_len,
                        uint8_t key[MLS_AEAD_KEY_LEN],
                        uint8_t nonce[MLS_AEAD_NONCE_LEN])
{
    /* Clamp sample to AEAD_KEY_LEN bytes */
    size_t actual_sample_len = sample_len < MLS_AEAD_KEY_LEN ? sample_len : MLS_AEAD_KEY_LEN;
    uint8_t sample[MLS_AEAD_KEY_LEN];
    if (actual_sample_len > 0) {
        memcpy(sample, ciphertext_sample, actual_sample_len);
    }
    if (actual_sample_len < MLS_AEAD_KEY_LEN) {
        memset(sample + actual_sample_len, 0, MLS_AEAD_KEY_LEN - actual_sample_len);
    }

    if (mls_crypto_expand_with_label(key, MLS_AEAD_KEY_LEN,
                                      sender_data_secret, "key",
                                      sample, MLS_AEAD_KEY_LEN) != 0)
        return -1;
    if (mls_crypto_expand_with_label(nonce, MLS_AEAD_NONCE_LEN,
                                      sender_data_secret, "nonce",
                                      sample, MLS_AEAD_KEY_LEN) != 0)
        return -1;
    return 0;
}

int
mls_sender_data_encrypt(const uint8_t sender_data_secret[MLS_HASH_LEN],
                         const uint8_t *ciphertext_sample, size_t sample_len,
                         const MlsSenderData *sender_data,
                         uint8_t *out, size_t *out_len)
{
    if (!sender_data_secret || !sender_data || !out || !out_len) return -1;

    uint8_t key[MLS_AEAD_KEY_LEN], nonce[MLS_AEAD_NONCE_LEN];
    if (derive_sender_data_keys(sender_data_secret, ciphertext_sample,
                                 sample_len, key, nonce) != 0)
        return -1;

    /* Serialize sender data (12 bytes) */
    uint8_t sd_plain[12];
    sender_data_serialize(sender_data, sd_plain);

    /* Encrypt with empty AAD */
    int rc = mls_crypto_aead_encrypt(out, out_len, key, nonce,
                                      sd_plain, 12, NULL, 0);

    sodium_memzero(key, sizeof(key));
    sodium_memzero(nonce, sizeof(nonce));
    sodium_memzero(sd_plain, sizeof(sd_plain));
    return rc;
}

int
mls_sender_data_decrypt(const uint8_t sender_data_secret[MLS_HASH_LEN],
                         const uint8_t *ciphertext_sample, size_t sample_len,
                         const uint8_t *encrypted, size_t encrypted_len,
                         MlsSenderData *out)
{
    if (!sender_data_secret || !encrypted || !out) return -1;
    /* Validate minimum ciphertext length */
    if (encrypted_len < MLS_AEAD_TAG_LEN) return -1;

    uint8_t key[MLS_AEAD_KEY_LEN], nonce[MLS_AEAD_NONCE_LEN];
    if (derive_sender_data_keys(sender_data_secret, ciphertext_sample,
                                 sample_len, key, nonce) != 0)
        return -1;

    uint8_t sd_plain[12];
    size_t pt_len = 0;
    int rc = mls_crypto_aead_decrypt(sd_plain, &pt_len, key, nonce,
                                      encrypted, encrypted_len, NULL, 0);
    sodium_memzero(key, sizeof(key));
    sodium_memzero(nonce, sizeof(nonce));

    if (rc != 0) return -1;
    if (pt_len != 12) return -1;

    sender_data_deserialize(sd_plain, out);
    sodium_memzero(sd_plain, sizeof(sd_plain));
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PrivateMessage lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_private_message_clear(MlsPrivateMessage *msg)
{
    if (!msg) return;
    free(msg->group_id);
    free(msg->authenticated_data);
    free(msg->encrypted_sender_data);
    if (msg->ciphertext) {
        sodium_memzero(msg->ciphertext, msg->ciphertext_len);
        free(msg->ciphertext);
    }
    memset(msg, 0, sizeof(*msg));
}

/* ══════════════════════════════════════════════════════════════════════════
 * PrivateMessage encryption
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_private_message_encrypt(const uint8_t *group_id, size_t group_id_len,
                             uint64_t epoch,
                             uint8_t content_type,
                             const uint8_t *authenticated_data, size_t aad_len,
                             const uint8_t *plaintext, size_t plaintext_len,
                             const uint8_t sender_data_secret[MLS_HASH_LEN],
                             const MlsMessageKeys *message_keys,
                             uint32_t sender_leaf_index,
                             const uint8_t reuse_guard[4],
                             MlsPrivateMessage *out)
{
    if (!group_id || !plaintext || !sender_data_secret || !message_keys || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    /* Step 1: Build content AAD */
    uint8_t *content_aad = NULL;
    size_t content_aad_len = 0;
    if (mls_build_content_aad(group_id, group_id_len, epoch, content_type,
                               authenticated_data, aad_len,
                               &content_aad, &content_aad_len) != 0)
        return -1;

    /* Step 2: Apply reuse guard to message nonce */
    uint8_t nonce[MLS_AEAD_NONCE_LEN];
    memcpy(nonce, message_keys->nonce, MLS_AEAD_NONCE_LEN);
    mls_apply_reuse_guard(nonce, reuse_guard);

    /* Step 3: Encrypt the content */
    size_t ct_max = plaintext_len + MLS_AEAD_TAG_LEN;
    uint8_t *ciphertext = malloc(ct_max);
    if (!ciphertext) { free(content_aad); return -1; }

    size_t ct_len = 0;
    int rc = mls_crypto_aead_encrypt(ciphertext, &ct_len,
                                      message_keys->key, nonce,
                                      plaintext, plaintext_len,
                                      content_aad, content_aad_len);
    free(content_aad);
    if (rc != 0) { free(ciphertext); return -1; }

    /* Step 4: Encrypt sender data */
    /* Ciphertext sample = first AEAD_KEY_LEN bytes of ciphertext */
    size_t sample_len = ct_len < MLS_AEAD_KEY_LEN ? ct_len : MLS_AEAD_KEY_LEN;

    MlsSenderData sd = {
        .leaf_index = sender_leaf_index,
        .generation = message_keys->generation,
    };
    memcpy(sd.reuse_guard, reuse_guard, 4);

    /* encrypted_sender_data: 12 + AEAD_TAG_LEN = 28 bytes */
    size_t esd_max = 12 + MLS_AEAD_TAG_LEN;
    uint8_t *esd = malloc(esd_max);
    if (!esd) { free(ciphertext); return -1; }

    size_t esd_len = 0;
    rc = mls_sender_data_encrypt(sender_data_secret, ciphertext, sample_len,
                                  &sd, esd, &esd_len);
    if (rc != 0) {
        free(ciphertext);
        free(esd);
        return -1;
    }

    /* Step 5: Populate output */
    out->group_id = malloc(group_id_len);
    if (!out->group_id) { free(ciphertext); free(esd); return -1; }
    memcpy(out->group_id, group_id, group_id_len);
    out->group_id_len = group_id_len;
    out->epoch = epoch;
    out->content_type = content_type;

    if (aad_len > 0 && authenticated_data) {
        out->authenticated_data = malloc(aad_len);
        if (!out->authenticated_data) { free(ciphertext); free(esd); mls_private_message_clear(out); return -1; }
        memcpy(out->authenticated_data, authenticated_data, aad_len);
        out->authenticated_data_len = aad_len;
    }

    out->ciphertext = ciphertext;
    out->ciphertext_len = ct_len;
    out->encrypted_sender_data = esd;
    out->encrypted_sender_data_len = esd_len;

    sodium_memzero(nonce, sizeof(nonce));
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PrivateMessage decryption
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_private_message_decrypt(const MlsPrivateMessage *msg,
                             const uint8_t sender_data_secret[MLS_HASH_LEN],
                             MlsSecretTree *st,
                             uint32_t max_forward_distance,
                             uint8_t **out_plaintext, size_t *out_pt_len,
                             MlsSenderData *out_sender)
{
    if (!msg || !sender_data_secret || !st || !out_plaintext || !out_pt_len || !out_sender)
        return -1;

    /* Step 1: Decrypt sender data */
    size_t sample_len = msg->ciphertext_len < MLS_AEAD_KEY_LEN
                         ? msg->ciphertext_len : MLS_AEAD_KEY_LEN;

    MlsSenderData sd;
    int rc = mls_sender_data_decrypt(sender_data_secret,
                                      msg->ciphertext, sample_len,
                                      msg->encrypted_sender_data,
                                      msg->encrypted_sender_data_len,
                                      &sd);
    if (rc != 0) return -1;

    /* Step 2: Get message keys from the secret tree */
    bool is_handshake = (msg->content_type == MLS_CONTENT_TYPE_PROPOSAL ||
                         msg->content_type == MLS_CONTENT_TYPE_COMMIT);

    MlsMessageKeys keys;
    rc = mls_secret_tree_get_keys_for_generation(st, sd.leaf_index, is_handshake,
                                                  sd.generation, max_forward_distance,
                                                  &keys);
    if (rc != 0) return rc;

    /* Step 3: Build content AAD */
    uint8_t *content_aad = NULL;
    size_t content_aad_len = 0;
    rc = mls_build_content_aad(msg->group_id, msg->group_id_len,
                                msg->epoch, msg->content_type,
                                msg->authenticated_data, msg->authenticated_data_len,
                                &content_aad, &content_aad_len);
    if (rc != 0) { sodium_memzero(&keys, sizeof(keys)); return -1; }

    /* Step 4: Apply reuse guard to nonce */
    uint8_t nonce[MLS_AEAD_NONCE_LEN];
    memcpy(nonce, keys.nonce, MLS_AEAD_NONCE_LEN);
    mls_apply_reuse_guard(nonce, sd.reuse_guard);

    /* Step 5: Decrypt content */
    if (msg->ciphertext_len < MLS_AEAD_TAG_LEN) {
        free(content_aad);
        sodium_memzero(&keys, sizeof(keys));
        return -1;
    }
    size_t pt_max = msg->ciphertext_len - MLS_AEAD_TAG_LEN;
    uint8_t *plaintext = malloc(pt_max > 0 ? pt_max : 1);
    if (!plaintext) {
        free(content_aad);
        sodium_memzero(&keys, sizeof(keys));
        return -1;
    }

    size_t pt_len = 0;
    rc = mls_crypto_aead_decrypt(plaintext, &pt_len, keys.key, nonce,
                                  msg->ciphertext, msg->ciphertext_len,
                                  content_aad, content_aad_len);

    free(content_aad);
    sodium_memzero(&keys, sizeof(keys));
    sodium_memzero(nonce, sizeof(nonce));

    if (rc != 0) {
        free(plaintext);
        return MARMOT_ERR_CRYPTO;
    }

    *out_plaintext = plaintext;
    *out_pt_len = pt_len;
    *out_sender = sd;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PrivateMessage TLS serialization
 *
 * struct {
 *   opaque group_id<V>;
 *   uint64 epoch;
 *   ContentType content_type;
 *   opaque authenticated_data<V>;
 *   opaque encrypted_sender_data<V>;
 *   opaque ciphertext<V>;
 * } PrivateMessage;
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_private_message_serialize(const MlsPrivateMessage *msg, MlsTlsBuf *buf)
{
    if (!msg || !buf) return -1;

    if (mls_tls_write_opaque8(buf, msg->group_id, msg->group_id_len) != 0) return -1;
    if (mls_tls_write_u64(buf, msg->epoch) != 0) return -1;
    if (mls_tls_write_u8(buf, msg->content_type) != 0) return -1;
    if (mls_tls_write_opaque32(buf, msg->authenticated_data,
                                msg->authenticated_data_len) != 0) return -1;
    if (mls_tls_write_opaque8(buf, msg->encrypted_sender_data,
                               msg->encrypted_sender_data_len) != 0) return -1;
    if (mls_tls_write_opaque32(buf, msg->ciphertext, msg->ciphertext_len) != 0) return -1;
    return 0;
}

int
mls_private_message_deserialize(MlsTlsReader *reader, MlsPrivateMessage *msg)
{
    if (!reader || !msg) return -1;
    memset(msg, 0, sizeof(*msg));

    if (mls_tls_read_opaque8(reader, &msg->group_id, &msg->group_id_len) != 0) goto fail;
    if (mls_tls_read_u64(reader, &msg->epoch) != 0) goto fail;
    if (mls_tls_read_u8(reader, &msg->content_type) != 0) goto fail;
    if (mls_tls_read_opaque32(reader, &msg->authenticated_data,
                               &msg->authenticated_data_len) != 0) goto fail;
    if (mls_tls_read_opaque8(reader, &msg->encrypted_sender_data,
                              &msg->encrypted_sender_data_len) != 0) goto fail;
    if (mls_tls_read_opaque32(reader, &msg->ciphertext, &msg->ciphertext_len) != 0) goto fail;
    return 0;

fail:
    mls_private_message_clear(msg);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * FramedContent (RFC 9420 §6.1)
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_framed_content_clear(MlsFramedContent *fc)
{
    if (!fc) return;
    free(fc->group_id);
    free(fc->authenticated_data);
    free(fc->content);
    memset(fc, 0, sizeof(*fc));
}

/** Serialize Sender to TLS. */
static int
sender_tls_serialize(const MlsSender *s, MlsTlsBuf *buf)
{
    if (mls_tls_write_u8(buf, s->sender_type) != 0) return -1;
    if (s->sender_type == MLS_SENDER_TYPE_MEMBER) {
        if (mls_tls_write_u32(buf, s->leaf_index) != 0) return -1;
    }
    /* external / new_member types have no extra data for our purposes */
    return 0;
}

/** Deserialize Sender from TLS. */
static int
sender_tls_deserialize(MlsTlsReader *reader, MlsSender *s)
{
    if (mls_tls_read_u8(reader, &s->sender_type) != 0) return -1;
    if (s->sender_type == MLS_SENDER_TYPE_MEMBER) {
        if (mls_tls_read_u32(reader, &s->leaf_index) != 0) return -1;
    } else {
        s->leaf_index = 0;
    }
    return 0;
}

int
mls_framed_content_serialize(const MlsFramedContent *fc, MlsTlsBuf *buf)
{
    if (!fc || !buf) return -1;

    if (mls_tls_write_opaque8(buf, fc->group_id, fc->group_id_len) != 0) return -1;
    if (mls_tls_write_u64(buf, fc->epoch) != 0) return -1;
    if (sender_tls_serialize(&fc->sender, buf) != 0) return -1;
    if (mls_tls_write_opaque32(buf, fc->authenticated_data,
                                fc->authenticated_data_len) != 0) return -1;
    if (mls_tls_write_u8(buf, fc->content_type) != 0) return -1;
    /* Content body is already serialized; write as opaque<V> */
    if (mls_tls_write_opaque32(buf, fc->content, fc->content_len) != 0) return -1;
    return 0;
}

int
mls_framed_content_deserialize(MlsTlsReader *reader, MlsFramedContent *fc)
{
    if (!reader || !fc) return -1;
    memset(fc, 0, sizeof(*fc));

    if (mls_tls_read_opaque8(reader, &fc->group_id, &fc->group_id_len) != 0) goto fail;
    if (mls_tls_read_u64(reader, &fc->epoch) != 0) goto fail;
    if (sender_tls_deserialize(reader, &fc->sender) != 0) goto fail;
    if (mls_tls_read_opaque32(reader, &fc->authenticated_data,
                               &fc->authenticated_data_len) != 0) goto fail;
    if (mls_tls_read_u8(reader, &fc->content_type) != 0) goto fail;
    if (mls_tls_read_opaque32(reader, &fc->content, &fc->content_len) != 0) goto fail;
    return 0;

fail:
    mls_framed_content_clear(fc);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * FramedContentTBS (RFC 9420 §6.1)
 *
 * struct {
 *   ProtocolVersion version = mls10;
 *   WireFormat wire_format;
 *   FramedContent content;
 *   select (content.sender.sender_type) {
 *     case member: GroupContext context;
 *   };
 * } FramedContentTBS;
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_framed_content_tbs_serialize(const MlsFramedContent *fc,
                                  uint16_t wire_format,
                                  const uint8_t *group_context, size_t group_context_len,
                                  uint8_t **out, size_t *out_len)
{
    if (!fc || !out || !out_len) return -1;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 256) != 0) return -1;

    /* version: uint16 = 1 (mls10) */
    if (mls_tls_write_u16(&buf, 1) != 0) goto fail;
    /* wire_format: uint16 */
    if (mls_tls_write_u16(&buf, wire_format) != 0) goto fail;
    /* content: FramedContent */
    if (mls_framed_content_serialize(fc, &buf) != 0) goto fail;
    /* For member senders, append GroupContext */
    if (fc->sender.sender_type == MLS_SENDER_TYPE_MEMBER) {
        if (!group_context || group_context_len == 0) goto fail;
        if (mls_tls_buf_append(&buf, group_context, group_context_len) != 0) goto fail;
    }

    *out = buf.data;
    *out_len = buf.len;
    return 0;

fail:
    mls_tls_buf_free(&buf);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Content signing / verification (RFC 9420 §6.1)
 *
 * SignWithLabel(signature_key, "FramedContentTBS", FramedContentTBS)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Build the SignContent input for SignWithLabel.
 *
 * struct {
 *   opaque label<V> = "MLS 1.0 " + Label;
 *   opaque content<V>;
 * } SignContent;
 */
static int
build_sign_content(const uint8_t *tbs, size_t tbs_len,
                   uint8_t **out, size_t *out_len)
{
    const char *full_label = "MLS 1.0 FramedContentTBS";
    size_t label_len = strlen(full_label);

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, label_len + tbs_len + 8) != 0) return -1;

    /* label: opaque<V> with uint8 length prefix */
    if (mls_tls_write_opaque8(&buf, (const uint8_t *)full_label, label_len) != 0) {
        mls_tls_buf_free(&buf);
        return -1;
    }
    /* content: opaque<V> with uint32 length prefix */
    if (mls_tls_write_opaque32(&buf, tbs, tbs_len) != 0) {
        mls_tls_buf_free(&buf);
        return -1;
    }

    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

int
mls_framed_content_sign(const MlsFramedContent *fc,
                         uint16_t wire_format,
                         const uint8_t *group_context, size_t group_context_len,
                         const uint8_t signature_key[MLS_SIG_SK_LEN],
                         MlsFramedContentAuthData *auth)
{
    if (!fc || !signature_key || !auth) return -1;
    memset(auth, 0, sizeof(*auth));

    /* Build FramedContentTBS */
    uint8_t *tbs = NULL;
    size_t tbs_len = 0;
    if (mls_framed_content_tbs_serialize(fc, wire_format,
                                          group_context, group_context_len,
                                          &tbs, &tbs_len) != 0)
        return -1;

    /* Build SignContent (SignWithLabel wrapper) */
    uint8_t *sign_content = NULL;
    size_t sign_content_len = 0;
    if (build_sign_content(tbs, tbs_len, &sign_content, &sign_content_len) != 0) {
        free(tbs);
        return -1;
    }
    free(tbs);

    /* Sign */
    int rc = mls_crypto_sign(auth->signature, signature_key,
                              sign_content, sign_content_len);
    free(sign_content);

    auth->has_confirmation_tag = false;
    return rc;
}

int
mls_framed_content_verify(const MlsFramedContent *fc,
                           const MlsFramedContentAuthData *auth,
                           uint16_t wire_format,
                           const uint8_t *group_context, size_t group_context_len,
                           const uint8_t verification_key[MLS_SIG_PK_LEN])
{
    if (!fc || !auth || !verification_key) return -1;

    /* Build FramedContentTBS */
    uint8_t *tbs = NULL;
    size_t tbs_len = 0;
    if (mls_framed_content_tbs_serialize(fc, wire_format,
                                          group_context, group_context_len,
                                          &tbs, &tbs_len) != 0)
        return -1;

    /* Build SignContent */
    uint8_t *sign_content = NULL;
    size_t sign_content_len = 0;
    if (build_sign_content(tbs, tbs_len, &sign_content, &sign_content_len) != 0) {
        free(tbs);
        return -1;
    }
    free(tbs);

    /* Verify */
    int rc = mls_crypto_verify(auth->signature, verification_key,
                                sign_content, sign_content_len);
    free(sign_content);
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Confirmation tag (RFC 9420 §8.1)
 *
 * confirmation_tag = HMAC(confirmation_key, confirmed_transcript_hash)
 *
 * We use HKDF-Expand-Label as a MAC since our crypto layer provides that.
 * Specifically: MAC(key, msg) = ExpandWithLabel(key, "MAC", msg, HASH_LEN)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_compute_confirmation_tag(const uint8_t confirmation_key[MLS_HASH_LEN],
                              const uint8_t confirmed_transcript_hash[MLS_HASH_LEN],
                              uint8_t out[MLS_HASH_LEN])
{
    if (!confirmation_key || !confirmed_transcript_hash || !out) return -1;
    return mls_crypto_expand_with_label(out, MLS_HASH_LEN,
                                         confirmation_key, "MAC",
                                         confirmed_transcript_hash, MLS_HASH_LEN);
}

/* ══════════════════════════════════════════════════════════════════════════
 * PublicMessage (RFC 9420 §6.2)
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_public_message_clear(MlsPublicMessage *msg)
{
    if (!msg) return;
    mls_framed_content_clear(&msg->content);
    memset(msg, 0, sizeof(*msg));
}

/**
 * Serialize FramedContentAuthData to TLS.
 */
static int
auth_data_serialize(const MlsFramedContentAuthData *auth, uint8_t content_type,
                    MlsTlsBuf *buf)
{
    if (mls_tls_write_opaque16(buf, auth->signature, MLS_SIG_LEN) != 0) return -1;
    if (content_type == MLS_CONTENT_TYPE_COMMIT) {
        /* confirmation_tag: MAC = opaque<V> */
        if (mls_tls_write_opaque8(buf, auth->confirmation_tag, MLS_HASH_LEN) != 0) return -1;
    }
    return 0;
}

/**
 * Deserialize FramedContentAuthData from TLS.
 */
static int
auth_data_deserialize(MlsTlsReader *reader, uint8_t content_type,
                      MlsFramedContentAuthData *auth)
{
    memset(auth, 0, sizeof(*auth));

    uint8_t *sig = NULL;
    size_t sig_len = 0;
    if (mls_tls_read_opaque16(reader, &sig, &sig_len) != 0) return -1;
    if (sig_len != MLS_SIG_LEN) { free(sig); return -1; }
    memcpy(auth->signature, sig, MLS_SIG_LEN);
    free(sig);

    if (content_type == MLS_CONTENT_TYPE_COMMIT) {
        uint8_t *tag = NULL;
        size_t tag_len = 0;
        if (mls_tls_read_opaque8(reader, &tag, &tag_len) != 0) return -1;
        if (tag_len != MLS_HASH_LEN) { free(tag); return -1; }
        memcpy(auth->confirmation_tag, tag, MLS_HASH_LEN);
        free(tag);
        auth->has_confirmation_tag = true;
    }
    return 0;
}

int
mls_public_message_serialize(const MlsPublicMessage *msg, MlsTlsBuf *buf)
{
    if (!msg || !buf) return -1;

    /* content: FramedContent */
    if (mls_framed_content_serialize(&msg->content, buf) != 0) return -1;
    /* auth: FramedContentAuthData */
    if (auth_data_serialize(&msg->auth, msg->content.content_type, buf) != 0) return -1;
    /* membership_tag (for member senders) */
    if (msg->content.sender.sender_type == MLS_SENDER_TYPE_MEMBER) {
        if (mls_tls_write_opaque8(buf, msg->membership_tag, MLS_HASH_LEN) != 0) return -1;
    }
    return 0;
}

int
mls_public_message_deserialize(MlsTlsReader *reader, MlsPublicMessage *msg)
{
    if (!reader || !msg) return -1;
    memset(msg, 0, sizeof(*msg));

    if (mls_framed_content_deserialize(reader, &msg->content) != 0) goto fail;
    if (auth_data_deserialize(reader, msg->content.content_type, &msg->auth) != 0) goto fail;
    if (msg->content.sender.sender_type == MLS_SENDER_TYPE_MEMBER) {
        uint8_t *tag = NULL;
        size_t tag_len = 0;
        if (mls_tls_read_opaque8(reader, &tag, &tag_len) != 0) goto fail;
        if (tag_len != MLS_HASH_LEN) { free(tag); goto fail; }
        memcpy(msg->membership_tag, tag, MLS_HASH_LEN);
        free(tag);
        msg->has_membership_tag = true;
    }
    return 0;

fail:
    mls_public_message_clear(msg);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Membership tag (RFC 9420 §6.2)
 *
 * AuthenticatedContentTBM = FramedContentTBS || FramedContentAuthData
 * membership_tag = MAC(membership_key, AuthenticatedContentTBM)
 * ══════════════════════════════════════════════════════════════════════════ */

static int
build_authenticated_content_tbm(const MlsPublicMessage *msg,
                                 const uint8_t *group_context, size_t group_context_len,
                                 uint8_t **out, size_t *out_len)
{
    /* FramedContentTBS */
    uint8_t *tbs = NULL;
    size_t tbs_len = 0;
    if (mls_framed_content_tbs_serialize(&msg->content,
                                          MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                          group_context, group_context_len,
                                          &tbs, &tbs_len) != 0)
        return -1;

    /* Serialize auth data */
    MlsTlsBuf auth_buf;
    if (mls_tls_buf_init(&auth_buf, 128) != 0) { free(tbs); return -1; }
    if (auth_data_serialize(&msg->auth, msg->content.content_type, &auth_buf) != 0) {
        free(tbs);
        mls_tls_buf_free(&auth_buf);
        return -1;
    }

    /* Concatenate TBS || auth */
    size_t total = tbs_len + auth_buf.len;
    uint8_t *tbm = malloc(total);
    if (!tbm) { free(tbs); mls_tls_buf_free(&auth_buf); return -1; }

    memcpy(tbm, tbs, tbs_len);
    memcpy(tbm + tbs_len, auth_buf.data, auth_buf.len);

    free(tbs);
    mls_tls_buf_free(&auth_buf);

    *out = tbm;
    *out_len = total;
    return 0;
}

int
mls_public_message_compute_membership_tag(MlsPublicMessage *msg,
                                           const uint8_t membership_key[MLS_HASH_LEN],
                                           const uint8_t *group_context, size_t group_context_len)
{
    if (!msg || !membership_key) return -1;

    uint8_t *tbm = NULL;
    size_t tbm_len = 0;
    if (build_authenticated_content_tbm(msg, group_context, group_context_len,
                                         &tbm, &tbm_len) != 0)
        return -1;

    /* MAC(membership_key, tbm) = ExpandWithLabel(key, "MAC", tbm, HASH_LEN) */
    int rc = mls_crypto_expand_with_label(msg->membership_tag, MLS_HASH_LEN,
                                           membership_key, "MAC",
                                           tbm, tbm_len);
    free(tbm);
    msg->has_membership_tag = (rc == 0);
    return rc;
}

int
mls_public_message_verify_membership_tag(const MlsPublicMessage *msg,
                                          const uint8_t membership_key[MLS_HASH_LEN],
                                          const uint8_t *group_context, size_t group_context_len)
{
    if (!msg || !membership_key) return -1;
    if (!msg->has_membership_tag) return -1;

    uint8_t *tbm = NULL;
    size_t tbm_len = 0;
    if (build_authenticated_content_tbm(msg, group_context, group_context_len,
                                         &tbm, &tbm_len) != 0)
        return -1;

    uint8_t expected[MLS_HASH_LEN];
    int rc = mls_crypto_expand_with_label(expected, MLS_HASH_LEN,
                                           membership_key, "MAC",
                                           tbm, tbm_len);
    free(tbm);
    if (rc != 0) return -1;

    if (sodium_memcmp(expected, msg->membership_tag, MLS_HASH_LEN) != 0)
        return MARMOT_ERR_CRYPTO;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * MLSMessage (RFC 9420 §6)
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_message_clear(MlsMLSMessage *msg)
{
    if (!msg) return;
    switch (msg->wire_format) {
    case MLS_WIRE_FORMAT_PUBLIC_MESSAGE:
        mls_public_message_clear(&msg->public_message);
        break;
    case MLS_WIRE_FORMAT_PRIVATE_MESSAGE:
        mls_private_message_clear(&msg->private_message);
        break;
    }
    memset(msg, 0, sizeof(*msg));
}

int
mls_message_serialize(const MlsMLSMessage *msg, MlsTlsBuf *buf)
{
    if (!msg || !buf) return -1;

    /* version: uint16 = 1 (mls10) */
    if (mls_tls_write_u16(buf, 1) != 0) return -1;
    /* cipher_suite: uint16 */
    if (mls_tls_write_u16(buf, msg->cipher_suite) != 0) return -1;
    /* wire_format: uint16 */
    if (mls_tls_write_u16(buf, msg->wire_format) != 0) return -1;

    switch (msg->wire_format) {
    case MLS_WIRE_FORMAT_PUBLIC_MESSAGE:
        return mls_public_message_serialize(&msg->public_message, buf);
    case MLS_WIRE_FORMAT_PRIVATE_MESSAGE:
        return mls_private_message_serialize(&msg->private_message, buf);
    default:
        return -1; /* Unsupported wire format */
    }
}

int
mls_message_deserialize(MlsTlsReader *reader, MlsMLSMessage *msg)
{
    if (!reader || !msg) return -1;
    memset(msg, 0, sizeof(*msg));

    uint16_t version;
    if (mls_tls_read_u16(reader, &version) != 0) return -1;
    if (version != 1) return -1; /* Only MLS 1.0 */

    if (mls_tls_read_u16(reader, &msg->cipher_suite) != 0) return -1;
    if (mls_tls_read_u16(reader, &msg->wire_format) != 0) return -1;

    switch (msg->wire_format) {
    case MLS_WIRE_FORMAT_PUBLIC_MESSAGE:
        return mls_public_message_deserialize(reader, &msg->public_message);
    case MLS_WIRE_FORMAT_PRIVATE_MESSAGE:
        return mls_private_message_deserialize(reader, &msg->private_message);
    default:
        return -1;
    }
}
