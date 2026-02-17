/*
 * libmarmot - Nostr Group Data Extension (0xF2EE) serialization
 *
 * Implements TLS presentation language serialization matching MDK's
 * NostrGroupDataExtension / TlsNostrGroupDataExtension.
 *
 * Wire format (MIP-01):
 *   uint16   version            (current: 2)
 *   opaque   nostr_group_id[32]
 *   opaque   name<2>            (UTF-8, length-prefixed)
 *   opaque   description<2>     (UTF-8, length-prefixed)
 *   opaque   admins<4>          (list of 32-byte pubkeys)
 *   opaque   relays<4>          (list of length-prefixed UTF-8 URLs)
 *   uint8    has_image           (0 or 1)
 *   [if has_image:]
 *     opaque image_hash[32]
 *     opaque image_key[32]
 *     opaque image_nonce[12]
 *   [if version >= 2 && has_image:]
 *     uint8  has_upload_key      (0 or 1)
 *     [if has_upload_key:]
 *       opaque image_upload_key[32]
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot-types.h>
#include <marmot/marmot-error.h>
#include "mls/mls-internal.h"
#include <stdlib.h>
#include <string.h>

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
    free(ext);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Serialize
 * ──────────────────────────────────────────────────────────────────────── */

int
marmot_group_data_extension_serialize(const MarmotGroupDataExtension *ext,
                                       uint8_t **out_data, size_t *out_len)
{
    if (!ext || !out_data || !out_len) return MARMOT_ERR_INVALID_ARG;

    /* Validate string lengths fit in uint16 */
    size_t name_len = ext->name ? strlen(ext->name) : 0;
    size_t desc_len = ext->description ? strlen(ext->description) : 0;
    if (name_len > 65535 || desc_len > 65535)
        return MARMOT_ERR_EXTENSION_FORMAT;

    /* Validate image fields: all present or all absent */
    bool has_image = (ext->image_hash != NULL);
    if (has_image) {
        if (!ext->image_key || !ext->image_nonce)
            return MARMOT_ERR_EXTENSION_FORMAT;
    }

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 512) != 0)
        return MARMOT_ERR_MEMORY;

    int rc = MARMOT_ERR_TLS_CODEC;

    /* version */
    if (mls_tls_write_u16(&buf, ext->version) != 0) goto fail;

    /* nostr_group_id[32] */
    if (mls_tls_buf_append(&buf, ext->nostr_group_id, 32) != 0) goto fail;

    /* name<2> */
    if (mls_tls_write_opaque16(&buf, (const uint8_t *)ext->name, name_len) != 0)
        goto fail;

    /* description<2> */
    if (mls_tls_write_opaque16(&buf, (const uint8_t *)ext->description, desc_len) != 0)
        goto fail;

    /* admins<4>: serialize as a vector of 32-byte pubkeys */
    {
        size_t admins_bytes = ext->admin_count * 32;
        if (mls_tls_write_u32(&buf, (uint32_t)admins_bytes) != 0) goto fail;
        for (size_t i = 0; i < ext->admin_count; i++) {
            if (mls_tls_buf_append(&buf, ext->admins[i], 32) != 0)
                goto fail;
        }
    }

    /* relays<4>: serialize as a vector of opaque<2> strings */
    {
        /* First compute total inner size */
        MlsTlsBuf inner;
        if (mls_tls_buf_init(&inner, 256) != 0) goto fail;
        for (size_t i = 0; i < ext->relay_count; i++) {
            size_t url_len = ext->relays[i] ? strlen(ext->relays[i]) : 0;
            if (mls_tls_write_opaque16(&inner,
                    (const uint8_t *)ext->relays[i], url_len) != 0) {
                mls_tls_buf_free(&inner);
                goto fail;
            }
        }
        if (mls_tls_write_opaque32(&buf, inner.data, inner.len) != 0) {
            mls_tls_buf_free(&inner);
            goto fail;
        }
        mls_tls_buf_free(&inner);
    }

    /* has_image flag */
    if (mls_tls_write_u8(&buf, has_image ? 1 : 0) != 0) goto fail;

    if (has_image) {
        /* All image fields are guaranteed non-NULL by validation above */
        if (mls_tls_buf_append(&buf, ext->image_hash, 32) != 0) goto fail;
        if (mls_tls_buf_append(&buf, ext->image_key, 32) != 0) goto fail;
        if (mls_tls_buf_append(&buf, ext->image_nonce, 12) != 0) goto fail;

        /* v2: upload key */
        if (ext->version >= 2) {
            bool has_upload = (ext->image_upload_key != NULL);
            if (mls_tls_write_u8(&buf, has_upload ? 1 : 0) != 0) goto fail;
            if (has_upload) {
                if (mls_tls_buf_append(&buf, ext->image_upload_key, 32) != 0)
                    goto fail;
            }
        }
    }

    *out_data = buf.data;
    *out_len = buf.len;
    /* Don't free buf.data — ownership transferred to caller */
    buf.data = NULL;
    return MARMOT_OK;

fail:
    mls_tls_buf_free(&buf);
    return rc;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Deserialize
 * ──────────────────────────────────────────────────────────────────────── */

MarmotGroupDataExtension *
marmot_group_data_extension_deserialize(const uint8_t *data, size_t len)
{
    if (!data || len < 2) return NULL;

    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);

    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    if (!ext) return NULL;

    /* version */
    if (mls_tls_read_u16(&r, &ext->version) != 0) goto fail;
    if (ext->version < 1 || ext->version > 2) goto fail;

    /* nostr_group_id[32] */
    if (mls_tls_read_fixed(&r, ext->nostr_group_id, 32) != 0) goto fail;

    /* name<2> */
    {
        uint8_t *name_data = NULL;
        size_t name_len = 0;
        if (mls_tls_read_opaque16(&r, &name_data, &name_len) != 0) goto fail;
        if (name_len > 0) {
            ext->name = malloc(name_len + 1);
            if (!ext->name) { free(name_data); goto fail; }
            memcpy(ext->name, name_data, name_len);
            ext->name[name_len] = '\0';
        }
        free(name_data);
    }

    /* description<2> */
    {
        uint8_t *desc_data = NULL;
        size_t desc_len = 0;
        if (mls_tls_read_opaque16(&r, &desc_data, &desc_len) != 0) goto fail;
        if (desc_len > 0) {
            ext->description = malloc(desc_len + 1);
            if (!ext->description) { free(desc_data); goto fail; }
            memcpy(ext->description, desc_data, desc_len);
            ext->description[desc_len] = '\0';
        }
        free(desc_data);
    }

    /* admins<4> */
    {
        uint32_t admins_len;
        if (mls_tls_read_u32(&r, &admins_len) != 0) goto fail;
        if (admins_len % 32 != 0) goto fail;
        ext->admin_count = admins_len / 32;
        /* Reasonable upper bound: 1000 admins */
        if (ext->admin_count > 1000) goto fail;
        if (ext->admin_count > 0) {
            ext->admins = malloc(admins_len);
            if (!ext->admins) goto fail;
            if (mls_tls_read_fixed(&r, (uint8_t *)ext->admins, admins_len) != 0)
                goto fail;
        }
    }

    /* relays<4> */
    {
        uint8_t *relays_data = NULL;
        size_t relays_len = 0;
        if (mls_tls_read_opaque32(&r, &relays_data, &relays_len) != 0) goto fail;

        if (relays_len > 0) {
            MlsTlsReader rr;
            mls_tls_reader_init(&rr, relays_data, relays_len);

            /* Count relays first */
            size_t count = 0;
            size_t save_pos = rr.pos;
            while (!mls_tls_reader_done(&rr)) {
                uint16_t url_len;
                if (mls_tls_read_u16(&rr, &url_len) != 0) { free(relays_data); goto fail; }
                if (mls_tls_reader_remaining(&rr) < url_len) { free(relays_data); goto fail; }
                rr.pos += url_len;
                count++;
                /* Reasonable upper bound: 100 relays */
                if (count > 100) { free(relays_data); goto fail; }
            }

            ext->relay_count = count;
            ext->relays = calloc(count, sizeof(char *));
            if (!ext->relays) { free(relays_data); goto fail; }

            /* Now actually read them */
            rr.pos = save_pos;
            for (size_t i = 0; i < count; i++) {
                uint8_t *url_data = NULL;
                size_t url_len = 0;
                if (mls_tls_read_opaque16(&rr, &url_data, &url_len) != 0) {
                    free(relays_data);
                    goto fail;
                }
                ext->relays[i] = malloc(url_len + 1);
                if (!ext->relays[i]) {
                    free(url_data);
                    free(relays_data);
                    goto fail;
                }
                if (url_len > 0)
                    memcpy(ext->relays[i], url_data, url_len);
                ext->relays[i][url_len] = '\0';
                free(url_data);
            }
        }
        free(relays_data);
    }

    /* has_image */
    {
        uint8_t has_image;
        if (mls_tls_read_u8(&r, &has_image) != 0) goto fail;

        if (has_image) {
            /* Allocate all three fields atomically to avoid partial allocation */
            ext->image_hash = malloc(32);
            if (!ext->image_hash) goto fail;
            ext->image_key = malloc(32);
            if (!ext->image_key) goto fail;
            ext->image_nonce = malloc(12);
            if (!ext->image_nonce) goto fail;

            if (mls_tls_read_fixed(&r, ext->image_hash, 32) != 0) goto fail;
            if (mls_tls_read_fixed(&r, ext->image_key, 32) != 0) goto fail;
            if (mls_tls_read_fixed(&r, ext->image_nonce, 12) != 0) goto fail;

            /* v2: upload key */
            if (ext->version >= 2) {
                uint8_t has_upload;
                if (mls_tls_read_u8(&r, &has_upload) != 0) goto fail;
                if (has_upload) {
                    ext->image_upload_key = malloc(32);
                    if (!ext->image_upload_key) goto fail;
                    if (mls_tls_read_fixed(&r, ext->image_upload_key, 32) != 0)
                        goto fail;
                }
            }
        }
    }

    return ext;

fail:
    marmot_group_data_extension_free(ext);
    return NULL;
}
