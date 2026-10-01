/*
 * libmarmot - Nostr Group Data Extension (0xF2EE) serialization
 *
 * MIP-01 marmot_group_data, version 2, exactly as MIP-01 specifies it
 * (marmot-protocol/marmot 64bf159, #41 "update extension format to v2 with
 * QUIC varint encoding"; unchanged through the last legacy text, cc73aa8)
 * and as MDK 0.8 encodes it (tls_codec 0.4 Vec<T>):
 *
 *   uint16   version                 (1 or 2; later versions: see below)
 *   opaque   nostr_group_id[32]
 *   opaque   name<V>                 (UTF-8)
 *   opaque   description<V>          (UTF-8)
 *   opaque   admin_pubkeys<V>        (raw 32-byte x-only keys, concatenated)
 *   RelayUrl relays<V>               (each element: opaque url<V>, UTF-8)
 *   opaque   image_hash<V>           (0 or 32 bytes)
 *   opaque   image_key<V>            (0 or 32 bytes)
 *   opaque   image_nonce<V>          (0 or 12 bytes)
 *   opaque   image_upload_key<V>     (0 or 32 bytes)
 *
 * <V> is a QUIC variable-length integer length prefix (RFC 9000 section 16,
 * RFC 9420 section 2.1.2). A later version appends fields (v3:
 * disappearing_message_secs<V>): they are kept verbatim in `extra` and
 * written back unchanged, so a metadata Commit made here does not drop them
 * (MIP-01 "Forward Compatibility").
 *
 * libmarmot 0.10.0 and older wrote a layout of their own instead: a fixed
 * uint32 admins length, and a has_image byte followed by fixed-size image
 * fields (and a has_upload_key byte). No other implementation reads it, and
 * libmarmot read nobody else's (nostrc-7gx7). It is read only from our own
 * stored group state (marmot_group_data_extension_deserialize_stored()), so
 * groups those versions made keep loading; a Welcome's or a Commit's
 * GroupData must be MIP-01 (nostrc-c7ho). It is never written.
 *
 * Version 1 is read in both encodings MDK wrote: without image_upload_key
 * (MDK before December 2025) and with it empty (MDK 0.8). Its image_key is
 * the image's encryption key itself, not a v2 seed.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot-types.h>
#include <marmot/marmot-error.h>
#include "mls/mls-internal.h"
#include <stdlib.h>
#include <string.h>

#define MARMOT_EXTENSION_MAX_ADMINS        1000u
#define MARMOT_EXTENSION_MAX_RELAYS        100u
#define MARMOT_EXTENSION_MAX_RELAY_URL_LEN 4096u
#define MARMOT_EXTENSION_MAX_ENCODED_SIZE  (1024u * 1024u)

/* ──────────────────────────────────────────────────────────────────────────
 * Constructor / Destructor
 * ──────────────────────────────────────────────────────────────────────── */

MarmotGroupDataExtension *
marmot_group_data_extension_new(void)
{
    MarmotGroupDataExtension *ext = calloc(1, sizeof(MarmotGroupDataExtension));
    if (ext) ext->version = MARMOT_EXTENSION_VERSION;
    return ext;
}

void
marmot_group_data_extension_free(MarmotGroupDataExtension *ext)
{
    if (!ext) return;
    free(ext->name);
    free(ext->description);
    free(ext->admins);
    if (ext->relays) {
        for (size_t i = 0; i < ext->relay_count; i++)
            free(ext->relays[i]);
        free(ext->relays);
    }
    free(ext->image_hash);
    free(ext->image_key);
    free(ext->image_nonce);
    free(ext->image_upload_key);
    free(ext->extra);
    free(ext);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Serialize
 * ──────────────────────────────────────────────────────────────────────── */

/* An optional fixed-size field as opaque<V>: empty when absent. */
static int
write_optional(MlsTlsBuf *buf, const uint8_t *field, size_t size)
{
    return mls_tls_write_opaque32(buf, field, field ? size : 0);
}

int
marmot_group_data_extension_serialize(const MarmotGroupDataExtension *ext,
                                       uint8_t **out_data, size_t *out_len)
{
    if (!ext || !out_data || !out_len) return MARMOT_ERR_INVALID_ARG;
    if (ext->version < 1) return MARMOT_ERR_EXTENSION_FORMAT;
    /* Fields of a later version exist only in a later version. */
    if (ext->extra_len > 0 && (!ext->extra || ext->version <= MARMOT_EXTENSION_VERSION))
        return MARMOT_ERR_EXTENSION_FORMAT;

    size_t name_len = ext->name ? strlen(ext->name) : 0;
    size_t desc_len = ext->description ? strlen(ext->description) : 0;
    if (name_len > 65535 || desc_len > 65535)
        return MARMOT_ERR_EXTENSION_FORMAT;

    if (ext->admin_count > MARMOT_EXTENSION_MAX_ADMINS)
        return MARMOT_ERR_EXTENSION_FORMAT;
    if (ext->admin_count > 0 && !ext->admins)
        return MARMOT_ERR_EXTENSION_FORMAT;
    size_t admins_bytes = ext->admin_count * 32;

    if (ext->relay_count > MARMOT_EXTENSION_MAX_RELAYS)
        return MARMOT_ERR_EXTENSION_FORMAT;
    if (ext->relay_count > 0 && !ext->relays)
        return MARMOT_ERR_EXTENSION_FORMAT;
    for (size_t i = 0; i < ext->relay_count; i++) {
        size_t url_len = ext->relays[i] ? strlen(ext->relays[i]) : 0;
        if (url_len > MARMOT_EXTENSION_MAX_RELAY_URL_LEN)
            return MARMOT_ERR_EXTENSION_FORMAT;
    }

    /* The image is all or nothing (hash, key, nonce); its upload key needs it. */
    bool has_image = ext->image_hash != NULL;
    if (has_image != (ext->image_key != NULL) || has_image != (ext->image_nonce != NULL))
        return MARMOT_ERR_EXTENSION_FORMAT;
    if (ext->image_upload_key && !has_image)
        return MARMOT_ERR_EXTENSION_FORMAT;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 512) != 0)
        return MARMOT_ERR_MEMORY;
    MlsTlsBuf relays;
    if (mls_tls_buf_init(&relays, 128) != 0) {
        mls_tls_buf_free(&buf);
        return MARMOT_ERR_MEMORY;
    }

    int rc = MARMOT_ERR_TLS_CODEC;
    for (size_t i = 0; i < ext->relay_count; i++) {
        size_t url_len = ext->relays[i] ? strlen(ext->relays[i]) : 0;
        if (mls_tls_write_opaque32(&relays, (const uint8_t *)ext->relays[i], url_len) != 0)
            goto fail;
    }
    if (mls_tls_write_u16(&buf, ext->version) != 0 ||
        mls_tls_buf_append(&buf, ext->nostr_group_id, 32) != 0 ||
        mls_tls_write_opaque16(&buf, (const uint8_t *)ext->name, name_len) != 0 ||
        mls_tls_write_opaque16(&buf, (const uint8_t *)ext->description, desc_len) != 0 ||
        mls_tls_write_opaque32(&buf, (const uint8_t *)ext->admins, admins_bytes) != 0 ||
        mls_tls_write_opaque32(&buf, relays.data, relays.len) != 0 ||
        write_optional(&buf, ext->image_hash, 32) != 0 ||
        write_optional(&buf, ext->image_key, 32) != 0 ||
        write_optional(&buf, ext->image_nonce, 12) != 0 ||
        write_optional(&buf, ext->image_upload_key, 32) != 0 ||
        (ext->extra_len > 0 && mls_tls_buf_append(&buf, ext->extra, ext->extra_len) != 0))
        goto fail;
    if (buf.len > MARMOT_EXTENSION_MAX_ENCODED_SIZE) {
        rc = MARMOT_ERR_EXTENSION_FORMAT;
        goto fail;
    }

    mls_tls_buf_free(&relays);
    *out_data = buf.data;   /* ownership moves to the caller */
    *out_len = buf.len;
    return MARMOT_OK;

fail:
    mls_tls_buf_free(&relays);
    mls_tls_buf_free(&buf);
    return rc;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Deserialize
 * ──────────────────────────────────────────────────────────────────────── */

/* A UTF-8 string field as a NUL-terminated copy (NULL when empty). */
static int
take_string(uint8_t *data, size_t len, char **out)
{
    *out = NULL;
    if (len > 0) {
        *out = malloc(len + 1);
        if (!*out) {
            free(data);
            return -1;
        }
        memcpy(*out, data, len);
        (*out)[len] = '\0';
    }
    free(data);
    return 0;
}

static int
read_string(MlsTlsReader *r, char **out)
{
    uint8_t *data = NULL;
    size_t len = 0;
    if (mls_tls_read_opaque16(r, &data, &len) != 0) return -1;
    return take_string(data, len, out);
}

/* The name and description, common to both layouts. */
static int
read_head(MlsTlsReader *r, MarmotGroupDataExtension *ext)
{
    if (mls_tls_read_u16(r, &ext->version) != 0 || ext->version < 1) return -1;
    if (mls_tls_read_fixed(r, ext->nostr_group_id, 32) != 0) return -1;
    if (read_string(r, &ext->name) != 0) return -1;
    return read_string(r, &ext->description);
}

static int
take_admins(uint8_t *data, size_t len, MarmotGroupDataExtension *ext)
{
    if (len % 32 != 0 || len / 32 > MARMOT_EXTENSION_MAX_ADMINS) {
        free(data);
        return -1;
    }
    ext->admin_count = len / 32;
    if (len == 0) {
        free(data);
        return 0;
    }
    ext->admins = (uint8_t (*)[32])data;
    return 0;
}

/* relays<V>: a vector of opaque url<V>. */
static int
read_relays(MlsTlsReader *r, MarmotGroupDataExtension *ext)
{
    uint8_t *data = NULL;
    size_t len = 0;
    if (mls_tls_read_opaque32(r, &data, &len) != 0) return -1;
    int rc = -1;
    MlsTlsReader rr;
    mls_tls_reader_init(&rr, data, len);
    size_t count = 0;
    while (!mls_tls_reader_done(&rr)) {
        size_t url_len;
        if (mls_tls_read_vli(&rr, &url_len) != 0 ||
            mls_tls_reader_remaining(&rr) < url_len ||
            url_len > MARMOT_EXTENSION_MAX_RELAY_URL_LEN ||
            ++count > MARMOT_EXTENSION_MAX_RELAYS)
            goto out;
        rr.pos += url_len;
    }
    if (count > 0) {
        ext->relays = calloc(count, sizeof(char *));
        if (!ext->relays) goto out;
        ext->relay_count = count;
    }
    mls_tls_reader_init(&rr, data, len);
    for (size_t i = 0; i < count; i++) {
        uint8_t *url = NULL;
        size_t url_len = 0;
        if (mls_tls_read_opaque32(&rr, &url, &url_len) != 0 ||
            take_string(url, url_len, &ext->relays[i]) != 0)
            goto out;
        if (!ext->relays[i] && !(ext->relays[i] = strdup("")))
            goto out;
    }
    rc = 0;
out:
    free(data);
    return rc;
}

/* An optional fixed-size field as opaque<V>: empty (NULL) or exactly size. */
static int
read_optional(MlsTlsReader *r, size_t size, uint8_t **out)
{
    uint8_t *data = NULL;
    size_t len = 0;
    if (mls_tls_read_opaque32(r, &data, &len) != 0) return -1;
    if (len != 0 && len != size) {
        free(data);
        return -1;
    }
    if (len == 0) {
        free(data);
        data = NULL;
    }
    *out = data;
    return 0;
}

/* MIP-01 version 2 (and 1, and the v2 fields of later versions). */
static MarmotGroupDataExtension *
decode_mip01(const uint8_t *data, size_t len)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    if (!ext) return NULL;
    uint8_t *admins = NULL;
    size_t admins_len = 0;
    if (read_head(&r, ext) != 0 ||
        mls_tls_read_opaque32(&r, &admins, &admins_len) != 0 ||
        take_admins(admins, admins_len, ext) != 0 ||
        read_relays(&r, ext) != 0 ||
        read_optional(&r, 32, &ext->image_hash) != 0 ||
        read_optional(&r, 32, &ext->image_key) != 0 ||
        read_optional(&r, 12, &ext->image_nonce) != 0 ||
        /* Version 1 as MDK wrote it before image_upload_key existed ends
         * here; MDK 0.8 writes v1 with the field, empty (nostrc-c7ho). */
        (!(ext->version == 1 && mls_tls_reader_done(&r)) &&
         read_optional(&r, 32, &ext->image_upload_key) != 0))
        goto fail;
    /* The image is all or nothing; its upload key needs it. */
    bool has_image = ext->image_hash != NULL;
    if (has_image != (ext->image_key != NULL) || has_image != (ext->image_nonce != NULL) ||
        (ext->image_upload_key && !has_image))
        goto fail;
    size_t rest = mls_tls_reader_remaining(&r);
    if (rest > 0) {
        /* Only a later version may carry more: kept, not interpreted. */
        if (ext->version <= MARMOT_EXTENSION_VERSION) goto fail;
        ext->extra = malloc(rest);
        if (!ext->extra) goto fail;
        memcpy(ext->extra, data + r.pos, rest);
        ext->extra_len = rest;
    }
    return ext;
fail:
    marmot_group_data_extension_free(ext);
    return NULL;
}

/* The layout libmarmot 0.10.0 and older wrote (see the top of the file). */
static MarmotGroupDataExtension *
decode_libmarmot_0_10(const uint8_t *data, size_t len)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    if (!ext) return NULL;
    uint32_t admins_len = 0;
    if (read_head(&r, ext) != 0 || ext->version > 2 ||
        mls_tls_read_u32(&r, &admins_len) != 0 ||
        mls_tls_reader_remaining(&r) < admins_len)
        goto fail;
    if (admins_len > 0) {
        uint8_t *admins = malloc(admins_len);
        if (!admins) goto fail;
        if (mls_tls_read_fixed(&r, admins, admins_len) != 0) {
            free(admins);
            goto fail;
        }
        if (take_admins(admins, admins_len, ext) != 0) goto fail;
    }
    if (read_relays(&r, ext) != 0) goto fail;
    uint8_t has_image = 0;
    if (mls_tls_read_u8(&r, &has_image) != 0 || has_image > 1) goto fail;
    if (has_image) {
        ext->image_hash = malloc(32);
        ext->image_key = malloc(32);
        ext->image_nonce = malloc(12);
        if (!ext->image_hash || !ext->image_key || !ext->image_nonce ||
            mls_tls_read_fixed(&r, ext->image_hash, 32) != 0 ||
            mls_tls_read_fixed(&r, ext->image_key, 32) != 0 ||
            mls_tls_read_fixed(&r, ext->image_nonce, 12) != 0)
            goto fail;
        if (ext->version >= 2) {
            uint8_t has_upload = 0;
            if (mls_tls_read_u8(&r, &has_upload) != 0 || has_upload > 1) goto fail;
            if (has_upload) {
                ext->image_upload_key = malloc(32);
                if (!ext->image_upload_key ||
                    mls_tls_read_fixed(&r, ext->image_upload_key, 32) != 0)
                    goto fail;
            }
        }
    }
    if (!mls_tls_reader_done(&r)) goto fail;
    return ext;
fail:
    marmot_group_data_extension_free(ext);
    return NULL;
}

MarmotGroupDataExtension *
marmot_group_data_extension_deserialize(const uint8_t *data, size_t len)
{
    if (!data || len < 2 || len > MARMOT_EXTENSION_MAX_ENCODED_SIZE) return NULL;
    return decode_mip01(data, len);
}

MarmotGroupDataExtension *
marmot_group_data_extension_deserialize_stored(const uint8_t *data, size_t len)
{
    MarmotGroupDataExtension *ext = marmot_group_data_extension_deserialize(data, len);
    if (ext || !data || len < 2 || len > MARMOT_EXTENSION_MAX_ENCODED_SIZE) return ext;
    return decode_libmarmot_0_10(data, len);
}
